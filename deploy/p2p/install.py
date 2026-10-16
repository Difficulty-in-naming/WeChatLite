"""Install the pinned upstream Rust relay on a Debian/Ubuntu host.

Run as root on the SSH host. The installer never prints credentials and never
changes an existing proxy, firewall, SSH configuration, or WeChat data.
"""
import argparse
import hashlib
import ipaddress
import json
import os
from pathlib import Path
import secrets
import subprocess
import tarfile

VERSION = "1.3.0"
ARCHIVE_SHA256 = "50e0158f6b9aefc95b45b515485d3b94d786fb1bfa4155d25937c71b81905f8f"
CONFIG = Path("/etc/wechat-p2p")
BINARY_DIR = Path("/opt/wechat-p2p") / VERSION
UNIT = Path("/etc/systemd/system/wechat-p2p-relay.service")


def run(*args):
    result = subprocess.run(args, capture_output=True, text=True, timeout=30)
    if result.returncode:
        # Neither arguments nor command output can leak a token on failure.
        raise RuntimeError("Deployment command failed: " + args[0])
    return result.stdout.strip()


def atomic_write(path, content, mode, gid=0):
    temporary = path.with_name(path.name + ".new")
    fd = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_EXCL, mode)
    try:
        with os.fdopen(fd, "wb") as stream:
            stream.write(content.encode() if isinstance(content, str) else content)
            stream.flush()
            os.fsync(stream.fileno())
        os.chown(temporary, 0, gid)
        os.chmod(temporary, mode)
        os.replace(temporary, path)
    except BaseException:
        temporary.unlink(missing_ok=True)
        raise


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--archive", type=Path, required=True)
    parser.add_argument("--public-ip", required=True)
    args = parser.parse_args()
    import grp
    import pwd
    address = ipaddress.ip_address(args.public_ip)
    if address.version != 4 or not address.is_global:
        raise SystemExit("Provide the public IPv4 address of this SSH host.")
    public_ip = str(address)
    if os.geteuid() != 0:
        raise SystemExit("Run this installer as root.")
    assets = Path(__file__).resolve().parent
    archive = args.archive.resolve(strict=True)
    if hashlib.sha256(archive.read_bytes()).hexdigest() != ARCHIVE_SHA256:
        raise SystemExit("Official relay archive checksum mismatch.")
    marker = CONFIG / "installed.json"
    if CONFIG.is_symlink() or BINARY_DIR.is_symlink() or UNIT.is_symlink():
        raise SystemExit("Refusing a symbolic link at a deployment path.")
    if CONFIG.exists() and not marker.is_file():
        raise SystemExit("The configuration directory exists without our ownership marker.")
    if marker.exists():
        prior = json.loads(marker.read_text())
        if prior.get("app") != "wechat-lite-p2p" or prior.get("public_ip") != public_ip:
            raise SystemExit("Existing relay belongs to a different deployment.")
        if prior.get("version") != VERSION or prior.get("archive_sha256") != ARCHIVE_SHA256:
            raise SystemExit("Version changes require a reviewed upgrade.")
    elif UNIT.exists():
        raise SystemExit("The service unit already exists without our ownership marker.")
    try:
        user = pwd.getpwnam("wechat-p2p")
        if user.pw_shell not in ("/usr/sbin/nologin", "/sbin/nologin"):
            raise SystemExit("Existing wechat-p2p account is not a service account.")
    except KeyError:
        run("useradd", "--system", "--user-group", "--no-create-home", "--home-dir", "/nonexistent",
            "--shell", "/usr/sbin/nologin", "wechat-p2p")
    gid = grp.getgrnam("wechat-p2p").gr_gid
    CONFIG.mkdir(mode=0o750, parents=True, exist_ok=True)
    os.chown(CONFIG, 0, gid)
    os.chmod(CONFIG, 0o750)
    BINARY_DIR.mkdir(mode=0o755, parents=True, exist_ok=True)
    with tarfile.open(archive, "r:gz") as package:
        members = [member for member in package.getmembers() if member.isfile()]
        if len(members) != 1 or members[0].name not in ("iroh-relay", "./iroh-relay"):
            raise SystemExit("Unexpected upstream release contents.")
        source = package.extractfile(members[0])
        if source is None or members[0].size > 64 * 1024 * 1024:
            raise SystemExit("Invalid upstream relay binary.")
        binary = source.read()
        if not binary.startswith(b"\x7fELF"):
            raise SystemExit("Expected a Linux ELF executable.")
        atomic_write(BINARY_DIR / "iroh-relay", binary, 0o755)
    version_output = run(str(BINARY_DIR / "iroh-relay"), "--version")
    if VERSION not in version_output:
        raise SystemExit("Unexpected relay version.")
    if not marker.exists():
        # This private CA stays on the SSH host. Only its public certificate is shared.
        run("openssl", "req", "-x509", "-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:P-256",
            "-nodes", "-keyout", str(CONFIG / "ca.key"), "-out", str(CONFIG / "ca.crt"),
            "-days", "3650", "-subj", "/CN=WeChat Lite private relay CA",
            "-addext", "basicConstraints=critical,CA:TRUE,pathlen:0",
            "-addext", "keyUsage=critical,keyCertSign,cRLSign")
        run("openssl", "req", "-new", "-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:P-256",
            "-nodes", "-keyout", str(CONFIG / "server.key"), "-out", str(CONFIG / "server.csr"),
            "-subj", "/CN=" + public_ip)
        atomic_write(CONFIG / "server.ext", "\n".join([
            "basicConstraints=critical,CA:FALSE", "keyUsage=critical,digitalSignature",
            "extendedKeyUsage=serverAuth", "subjectAltName=IP:" + public_ip, ""]), 0o600)
        run("openssl", "x509", "-req", "-in", str(CONFIG / "server.csr"),
            "-CA", str(CONFIG / "ca.crt"), "-CAkey", str(CONFIG / "ca.key"), "-CAcreateserial",
            "-out", str(CONFIG / "server.crt"), "-days", "1095", "-sha256",
            "-extfile", str(CONFIG / "server.ext"))
        token = secrets.token_hex(32)
        configuration = (assets / "relay.toml.example").read_text().replace(
            "REPLACE_WITH_A_RANDOM_64_CHARACTER_SECRET", token)
        if configuration.count(token) != 1:
            raise SystemExit("Relay template must contain exactly one access token.")
        atomic_write(CONFIG / "relay.toml", configuration, 0o640, gid)
        public = {"enabled": True, "relay_url": "https://" + public_ip + ":54443/",
                  "relay_token": token, "relay_ca": (CONFIG / "ca.crt").read_text(),
                  "relay_quic_port": 54443}
        # This file is deliberately private: it includes the infrastructure access token.
        atomic_write(CONFIG / "endpoint.json", json.dumps(public, indent=2), 0o640, gid)
        atomic_write(marker, json.dumps({"app": "wechat-lite-p2p", "version": VERSION,
            "public_ip": public_ip, "archive_sha256": ARCHIVE_SHA256}, indent=2), 0o640, gid)
    for path in CONFIG.iterdir():
        if path.name in ("ca.key", "server.csr", "server.ext", "ca.srl"):
            os.chown(path, 0, 0)
            os.chmod(path, 0o600)
        elif path.name in ("server.key", "server.crt", "ca.crt"):
            os.chown(path, 0, gid)
            os.chmod(path, 0o640)
    run("openssl", "verify", "-CAfile", str(CONFIG / "ca.crt"), str(CONFIG / "server.crt"))
    atomic_write(UNIT, (assets / UNIT.name).read_bytes(), 0o644)
    run("systemd-analyze", "verify", str(UNIT))
    run("systemctl", "daemon-reload")
    run("systemctl", "enable", "--now", UNIT.name)
    if run("systemctl", "is-active", UNIT.name) != "active":
        raise SystemExit("Relay service did not become active.")
    print(json.dumps({"installed": True, "version": VERSION,
        "relay_url": "https://" + public_ip + ":54443/", "tcp_port": 54443,
        "udp_port": 54443, "metrics_public": False, "credentials_printed": False}))


if __name__ == "__main__":
    main()
