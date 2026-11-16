#!/usr/bin/env python3
"""End-to-end smoke test: is the relay usable on this machine?

Starts an isolated relay with the local test adapter and exercises the real
HTTP surface: status, forwarding selection, text send, image send, queue
drain, history. Prints PASS/FAIL per step. Never touches WeChat.
"""
import json
import os
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request
import uuid
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
RELAY = os.path.join(ROOT, "target/release/wechat-relay")
ADAPTER = os.path.join(HERE, "fake_adapter.py")

results = []


def check(name, ok, detail=""):
    results.append((name, ok, detail))
    print("%s  %-42s %s" % ("PASS" if ok else "FAIL", name, detail))
    return ok


def png_bytes():
    def chunk(tag, data):
        body = tag + data
        return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)
    return (b"\x89PNG\r\n\x1a\n"
            + chunk(b"IHDR", struct.pack(">IIBBBBB", 2, 2, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(b"\x00\xff\x00\x00\x00\xff\x00\x00\x00\xff\x00"))
            + chunk(b"IEND", b""))


def free_port():
    sock = socket.socket()
    sock.bind(("127.0.0.1", 0))
    value = sock.getsockname()[1]
    sock.close()
    return value


class Client:
    def __init__(self, port, token):
        self.base = "http://127.0.0.1:%d" % port
        self.token = token

    def call(self, method, path, body=None, raw=None, headers=None):
        request = urllib.request.Request(
            self.base + path,
            data=raw if raw is not None else (json.dumps(body).encode() if body is not None else None),
            method=method)
        request.add_header("Authorization", "Bearer " + self.token)
        for key, value in (headers or {}).items():
            request.add_header(key, value)
        if body is not None and raw is None:
            request.add_header("Content-Type", "application/json")
        try:
            with urllib.request.urlopen(request, timeout=30) as response:
                payload = response.read()
                return json.loads(payload) if payload else None
        except urllib.error.HTTPError as error:
            return {"__error__": error.code, "body": error.read().decode(errors="replace")[:200]}


def main():
    if not os.path.isfile(RELAY):
        raise SystemExit("relay binary missing: " + RELAY)

    data = tempfile.mkdtemp(prefix="wclite-smoke-")
    port = free_port()
    token = "smoke-" + uuid.uuid4().hex
    config = os.path.join(data, "server.local.json")
    with open(config, "w") as handle:
        json.dump({
            "listen": "127.0.0.1:%d" % port,
            "token": token,
            "data": os.path.join(data, "state"),
            "bridge_program": sys.executable,
            "bridge_args": [ADAPTER, "--image-delay", "3"],
            "tls_cert": "",
            "tls_key": "",
        }, handle)

    log_path = os.path.join(data, "relay.log")
    log = open(log_path, "w")
    process = subprocess.Popen([RELAY, "--config", config], stdout=log, stderr=subprocess.STDOUT, text=True)
    client = Client(port, token)

    try:
        # 1. Service comes up and reports a real HTTP response.
        ready = False
        deadline = time.time() + 25
        while time.time() < deadline:
            try:
                status = client.call("GET", "/api/status")
                if status.get("ready") is True:
                    ready = True
                    break
            except Exception:
                pass
            time.sleep(0.2)
        if not check("relay starts and reports ready", ready):
            return
        check("adapter connected", status.get("text", "").startswith("本地测试适配器"),
              status.get("text", "")[:40])

        # 2. Forwarding selection round-trips.
        selected = client.call("POST", "/api/local/forwarding", {"selected": ["g1", "g2"]})
        check("forwarding groups selectable",
              selected.get("selected") == ["g1", "g2"],
              str(selected.get("selected")))

        groups = client.call("GET", "/api/groups")
        check("group list served", isinstance(groups, list) and len(groups) == 2,
              "%d groups" % (len(groups) if isinstance(groups, list) else -1))

        # 3. Text message send.
        text_id = str(uuid.uuid4())
        sent = client.call("POST", "/api/messages", {
            "id": text_id, "group": "g1", "client_id": "smoke", "sender": "我",
            "text": "smoke test", "media": [], "mentions": []})
        check("text message accepted", sent.get("status") == "queued", str(sent.get("status")))

        deadline = time.time() + 20
        final = None
        while time.time() < deadline:
            page = client.call("GET", "/api/history?group=g1&before=0")
            for message in page.get("messages", []):
                if message["id"] == text_id:
                    final = message["status"]
            if final in ("sent", "failed", "uncertain"):
                break
            time.sleep(0.3)
        check("text message confirmed", final == "sent", str(final))

        # 4. Image upload and send.
        uploaded = client.call("POST", "/api/media?name=test.png&group=g1", raw=png_bytes(),
                               headers={"Content-Type": "application/octet-stream"})
        check("image upload accepted",
              bool(uploaded.get("id")) and uploaded.get("kind") == "image",
              str(uploaded.get("kind")))

        image_id = str(uuid.uuid4())
        queued = client.call("POST", "/api/messages", {
            "id": image_id, "group": "g1", "client_id": "smoke", "sender": "我",
            "text": "", "media": [uploaded["id"]], "mentions": []})
        check("image message accepted", queued.get("status") == "queued", str(queued.get("status")))

        deadline = time.time() + 30
        final = None
        while time.time() < deadline:
            page = client.call("GET", "/api/history?group=g1&before=0")
            for message in page.get("messages", []):
                if message["id"] == image_id:
                    final = message["status"]
            if final in ("sent", "failed", "uncertain"):
                break
            time.sleep(0.4)
        check("image message confirmed", final == "sent", str(final))

        # 5. History still readable afterwards.
        page = client.call("GET", "/api/history?group=g1&before=0")
        check("history readable", isinstance(page, dict) and "messages" in page,
              "%d rows" % len(page.get("messages", [])) if isinstance(page, dict) else "?")

    finally:
        process.terminate()
        try:
            process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            process.kill()
        log.close()

    passed = sum(1 for _, ok, _ in results if ok)
    print("\n%d/%d checks passed" % (passed, len(results)))
    if passed != len(results):
        print("\n--- relay log ---")
        print(open(log_path).read()[-2000:])
    shutil.rmtree(data, ignore_errors=True)
    sys.exit(0 if passed == len(results) else 1)


if __name__ == "__main__":
    main()
