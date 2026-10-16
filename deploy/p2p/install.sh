#!/bin/sh
# Install the pinned upstream Rust relay. No downloads, Python, firewall or proxy changes.
set -eu
umask 077
PATH=/usr/sbin:/usr/bin:/sbin:/bin
export PATH

VERSION=1.3.0
ARCHIVE_SHA256=50e0158f6b9aefc95b45b515485d3b94d786fb1bfa4155d25937c71b81905f8f
CONFIG=/etc/wechat-p2p
BINARY_DIR=/opt/wechat-p2p/1.3.0
UNIT=/etc/systemd/system/wechat-p2p-relay.service
LOCK=/run/lock/wechat-p2p-install.lock
archive=
public_ip=
stage=
atomic_temp=

die() { printf '%s\n' "$1" >&2; exit 1; }
usage() { printf '%s\n' 'Usage: sudo sh install.sh --archive FILE.tar.gz --public-ip PUBLIC_IPV4'; }
cleanup() {
    [ -z "$atomic_temp" ] || rm -f -- "$atomic_temp"
    if [ -n "$stage" ]; then
        rm -f -- "$stage"/*
        rmdir -- "$stage" 2>/dev/null || :
    fi
}
trap cleanup 0
trap 'exit 1' 1 2 15

while [ "$#" -gt 0 ]; do
    case "$1" in
        --archive) [ "$#" -ge 2 ] || die 'Missing archive.'; archive=$2; shift 2 ;;
        --public-ip) [ "$#" -ge 2 ] || die 'Missing public IPv4.'; public_ip=$2; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) usage >&2; die 'Unknown installer argument.' ;;
    esac
done
[ -n "$archive" ] && [ -n "$public_ip" ] || { usage >&2; exit 1; }
[ "$(id -u)" = 0 ] || die 'Run this installer as root.'
[ "$(uname -s)" = Linux ] && [ "$(uname -m)" = x86_64 ] || die 'This archive requires Linux x86_64.'
for tool in jq openssl tar sha256sum timeout realpath mktemp stat sync flock getent useradd systemctl systemd-analyze; do
    command -v "$tool" >/dev/null 2>&1 || die "Missing dependency: $tool"
done
# Reject private, shared, loopback, link-local, documentation, benchmark and multicast/reserved space.
case "$public_ip" in *[!0-9.]*|'') die 'Provide the public IPv4 address of this SSH host.' ;; esac
printf '%s\n' "$public_ip" | awk -F '[.]' '
    NF != 4 { exit 1 }
    { for (i=1;i<=4;i++) if ($i !~ /^[0-9]+$/ || length($i)>3 || $i>255 || (length($i)>1 && substr($i,1,1)=="0")) exit 1
      a=$1+0;b=$2+0;c=$3+0
      if (a==0 || a==10 || a==127 || a>=224 || (a==100 && b>=64 && b<=127) ||
          (a==169 && b==254) || (a==172 && b>=16 && b<=31) ||
          (a==192 && ((b==168) || (b==0 && (c==0 || c==2)))) ||
          (a==198 && (b==18 || b==19 || (b==51 && c==100))) || (a==203 && b==0 && c==113)) exit 1
    }' || die 'Provide the public IPv4 address of this SSH host.'

archive=$(realpath -e -- "$archive") || die 'Archive does not exist.'
[ -f "$archive" ] || die 'Archive must be a regular file.'
assets=$(dirname -- "$(realpath -e -- "$0")")
stage=$(mktemp -d /tmp/wechat-p2p-install.XXXXXXXX) || die 'Cannot create private staging directory.'

# Capture command output privately: OpenSSL diagnostics and credentials never reach the terminal.
run() {
    timeout 30 "$@" >"$stage/command.out" 2>&1 || die "Deployment command failed: ${1##*/}"
}
secure_path() {
    [ ! -L "$1" ] || die 'Refusing a symbolic link at a deployment path.'
    if [ -e "$1" ]; then
        [ "$(stat -c %u -- "$1")" = 0 ] || die 'Deployment paths must be owned by root.'
        mode=$(stat -c %a -- "$1")
        [ "$((0$mode & 022))" -eq 0 ] || die 'Deployment paths must not be group/world writable.'
    fi
}
atomic_install() {
    source_file=$1 destination=$2 file_mode=$3 file_gid=$4
    secure_path "$destination"
    [ ! -e "$destination" ] || [ -f "$destination" ] || die 'Deployment target is not a regular file.'
    atomic_temp=$(mktemp "$destination.new.XXXXXXXX") || die 'Cannot stage deployment file.'
    cat -- "$source_file" >"$atomic_temp"
    chown "0:$file_gid" "$atomic_temp"
    chmod "$file_mode" "$atomic_temp"
    sync "$atomic_temp"
    mv -fT -- "$atomic_temp" "$destination"
    atomic_temp=
}

run sha256sum -- "$archive"
[ "$(cut -d ' ' -f 1 "$stage/command.out")" = "$ARCHIVE_SHA256" ] || die 'Official relay archive checksum mismatch.'
for path in "$CONFIG" /opt/wechat-p2p "$BINARY_DIR" "$UNIT" "$LOCK"; do secure_path "$path"; done
exec 9>"$LOCK"
flock -n 9 || die 'Another relay installation is running.'
marker=$CONFIG/installed.json
for name in installed.json relay.toml endpoint.json ca.key ca.crt server.key server.crt server.csr server.ext ca.srl; do
    secure_path "$CONFIG/$name"
done
existing=false
if [ -e "$CONFIG" ]; then
    [ -d "$CONFIG" ] && [ -f "$marker" ] || die 'The configuration directory exists without our ownership marker.'
    [ "$(wc -c <"$marker")" -le 16384 ] || die 'Invalid deployment marker.'
    jq -e --arg ip "$public_ip" --arg version "$VERSION" --arg sha "$ARCHIVE_SHA256" \
        'type=="object" and .app=="wechat-lite-p2p" and .public_ip==$ip and .version==$version and .archive_sha256==$sha' \
        "$marker" >/dev/null 2>&1 || die 'Existing deployment identity differs; version/IP changes require a reviewed upgrade.'
    existing=true
    for name in relay.toml endpoint.json ca.key ca.crt server.key server.crt; do
        [ -f "$CONFIG/$name" ] || die 'Existing relay files are incomplete; refusing to regenerate credentials.'
    done
elif [ -e "$UNIT" ]; then
    die 'The service unit already exists without our ownership marker.'
fi
if getent passwd wechat-p2p >"$stage/passwd"; then
    service_shell=$(cut -d : -f 7 "$stage/passwd")
    case "$service_shell" in /usr/sbin/nologin|/sbin/nologin) : ;; *) die 'Existing wechat-p2p account is not a service account.' ;; esac
    [ "$(id -u wechat-p2p)" != 0 ] || die 'The relay cannot run as root.'
else
    run useradd --system --user-group --no-create-home --home-dir /nonexistent --shell /usr/sbin/nologin wechat-p2p
fi
getent group wechat-p2p >"$stage/group" || die 'Relay service group is missing.'
gid=$(cut -d : -f 3 "$stage/group")
case "$gid" in ''|*[!0-9]*|0) die 'Invalid relay service group.' ;; esac
mkdir -p -- "$CONFIG" "$BINARY_DIR"
chown "0:$gid" "$CONFIG"
chmod 750 "$CONFIG"
chown 0:0 /opt/wechat-p2p "$BINARY_DIR"
chmod 755 /opt/wechat-p2p "$BINARY_DIR"

run tar -tzf "$archive"
member=$(awk '$0!="." && $0!="./" {print}' "$stage/command.out")
case "$member" in iroh-relay|./iroh-relay) : ;; *) die 'Unexpected upstream release contents.' ;; esac
run tar -tvzf "$archive"
awk 'substr($0,1,1)=="d" {next} substr($0,1,1)=="-" {count++;next} {bad=1} END {exit bad || count!=1}' \
    "$stage/command.out" || die 'Expected one regular upstream relay binary.'
timeout 30 tar -xOzf "$archive" -- "$member" >"$stage/iroh-relay" 2>"$stage/archive.err" || die 'Cannot read upstream relay binary.'
[ "$(wc -c <"$stage/iroh-relay")" -le 67108864 ] || die 'Upstream relay binary is too large.'
[ "$(od -An -tx1 -N4 "$stage/iroh-relay" | tr -d ' \n')" = 7f454c46 ] || die 'Expected a Linux ELF executable.'
atomic_install "$stage/iroh-relay" "$BINARY_DIR/iroh-relay" 755 0
run "$BINARY_DIR/iroh-relay" --version
grep -F "$VERSION" "$stage/command.out" >/dev/null || die 'Unexpected relay version.'

if [ "$existing" = false ]; then
    # All new credentials are staged privately. Reruns never regenerate an existing deployment.
    # Host-wide OpenSSL defaults must not inject duplicate or unintended certificate extensions.
    printf '%s\n' '[req]' 'distinguished_name=dn' 'prompt=no' '[dn]' 'CN=WeChat Lite private relay CA' >"$stage/openssl.cnf"
    run openssl req -x509 -config "$stage/openssl.cnf" -newkey ec -pkeyopt ec_paramgen_curve:P-256 -nodes \
        -keyout "$stage/ca.key" -out "$stage/ca.crt" -days 3650 -subj '/CN=WeChat Lite private relay CA' \
        -addext 'basicConstraints=critical,CA:TRUE,pathlen:0' -addext 'keyUsage=critical,keyCertSign,cRLSign' \
        -addext 'subjectKeyIdentifier=hash'
    run openssl req -new -config "$stage/openssl.cnf" -newkey ec -pkeyopt ec_paramgen_curve:P-256 -nodes \
        -keyout "$stage/server.key" -out "$stage/server.csr" -subj "/CN=$public_ip"
    printf '%s\n' 'basicConstraints=critical,CA:FALSE' 'keyUsage=critical,digitalSignature' \
        'extendedKeyUsage=serverAuth' "subjectAltName=IP:$public_ip" >"$stage/server.ext"
    run openssl x509 -req -in "$stage/server.csr" -CA "$stage/ca.crt" -CAkey "$stage/ca.key" \
        -CAcreateserial -out "$stage/server.crt" -days 397 -sha256 -extfile "$stage/server.ext"
    run openssl rand -hex 32
    cp -- "$stage/command.out" "$stage/token"
    grep -Eq '^[0-9a-f]{64}$' "$stage/token" || die 'Cannot generate relay access token.'
    awk 'NR==FNR {token=$0;next} {count+=gsub(/REPLACE_WITH_A_RANDOM_64_CHARACTER_SECRET/,token);print} END {if(count!=1)exit 1}' \
        "$stage/token" "$assets/relay.toml.example" >"$stage/relay.toml" || die 'Relay template must contain exactly one access token.'
    jq -n --arg url "https://$public_ip:54443/" --rawfile token "$stage/token" --rawfile ca "$stage/ca.crt" \
        '{enabled:true,relay_url:$url,relay_token:($token|rtrimstr("\n")),relay_ca:$ca,relay_quic_port:54443}' >"$stage/endpoint.json"
    jq -n --arg version "$VERSION" --arg ip "$public_ip" --arg sha "$ARCHIVE_SHA256" \
        '{app:"wechat-lite-p2p",version:$version,public_ip:$ip,archive_sha256:$sha}' >"$stage/installed.json"
    for name in ca.key server.csr server.ext ca.srl; do atomic_install "$stage/$name" "$CONFIG/$name" 600 0; done
    for name in ca.crt server.key server.crt relay.toml endpoint.json; do atomic_install "$stage/$name" "$CONFIG/$name" 640 "$gid"; done
    atomic_install "$stage/installed.json" "$marker" 640 "$gid"
fi
for name in ca.key server.csr server.ext ca.srl; do
    if [ -f "$CONFIG/$name" ]; then chown 0:0 "$CONFIG/$name"; chmod 600 "$CONFIG/$name"; fi
done
for name in ca.crt server.key server.crt relay.toml endpoint.json installed.json; do
    chown "0:$gid" "$CONFIG/$name"
    chmod 640 "$CONFIG/$name"
done
run openssl verify -CAfile "$CONFIG/ca.crt" "$CONFIG/server.crt"
atomic_install "$assets/wechat-p2p-relay.service" "$UNIT" 644 0
run systemd-analyze verify "$UNIT"
run systemctl daemon-reload
run systemctl enable --now wechat-p2p-relay.service
run systemctl is-active wechat-p2p-relay.service
[ "$(cat "$stage/command.out")" = active ] || die 'Relay service did not become active.'
jq -n --arg version "$VERSION" --arg url "https://$public_ip:54443/" \
    '{installed:true,version:$version,relay_url:$url,tcp_port:54443,udp_port:54443,metrics_public:false,credentials_printed:false}'
