#!/usr/bin/env python3
"""One-shot reproduction: does an unacknowledged image block the send queue?

Starts an isolated relay with the fake adapter, submits one image followed by
several texts, and samples every message's status over time. Nothing here
touches WeChat; the relay binary is the real, unmodified one.
"""
import json
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request
import uuid
import zlib
import struct

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
RELAY = os.path.join(ROOT, "target/release/wechat-relay")
ADAPTER = os.path.join(HERE, "fake_adapter.py")


def png_bytes():
    def chunk(tag, data):
        body = tag + data
        return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)

    raw = b"\x00\xff\x00\x00\x00\xff\x00" + b"\x00\x00\xff"
    return (b"\x89PNG\r\n\x1a\n"
            + chunk(b"IHDR", struct.pack(">IIBBBBB", 2, 2, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw))
            + chunk(b"IEND", b""))


def free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


class Client:
    def __init__(self, port, token):
        self.base = "http://127.0.0.1:%d" % port
        self.token = token

    def call(self, method, path, body=None, raw=None, headers=None):
        data = raw if raw is not None else (json.dumps(body).encode() if body is not None else None)
        request = urllib.request.Request(self.base + path, data=data, method=method)
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
            return {"__error__": error.code, "body": error.read().decode(errors="replace")}


def main():
    image_delay = float(sys.argv[1]) if len(sys.argv) > 1 else 20.0
    data = tempfile.mkdtemp(prefix="wclite-repro-")
    port = free_port()
    token = "repro-" + uuid.uuid4().hex
    config = os.path.join(data, "server.local.json")
    with open(config, "w") as handle:
        json.dump({
            "listen": "127.0.0.1:%d" % port,
            "token": token,
            "data": os.path.join(data, "state"),
            "bridge_program": sys.executable,
            "bridge_args": [ADAPTER, "--image-delay", str(image_delay)],
            "tls_cert": "",
            "tls_key": "",
        }, handle)

    log = open(os.path.join(data, "output.log"), "w")
    process = subprocess.Popen([RELAY, "--config", config], stdout=log, stderr=subprocess.STDOUT, text=True)
    client = Client(port, token)
    try:
        deadline = time.time() + 25
        while time.time() < deadline:
            try:
                if client.call("GET", "/api/status").get("ready") is True:
                    break
            except Exception:
                pass
            time.sleep(0.2)
        else:
            raise SystemExit("relay never became ready")

        client.call("POST", "/api/local/forwarding", {"selected": ["g1"]})
        time.sleep(0.5)

        uploaded = client.call(
            "POST", "/api/media?name=test.png&group=g1",
            raw=png_bytes(), headers={"Content-Type": "application/octet-stream"})
        media_id = uploaded.get("id")
        print("uploaded image -> %s" % media_id)

        image_id = str(uuid.uuid4())
        client.call("POST", "/api/messages", {
            "id": image_id, "group": "g1", "client_id": "repro", "sender": "我",
            "text": "", "media": [media_id], "mentions": []})
        text_ids = []
        for index in range(4):
            mid = str(uuid.uuid4())
            text_ids.append(mid)
            client.call("POST", "/api/messages", {
                "id": mid, "group": "g1", "client_id": "repro", "sender": "我",
                "text": "图片之后的文本 %d" % (index + 1), "media": [], "mentions": []})

        order = [image_id] + text_ids
        started = time.time()
        seen = {}
        print("\n  t    图片        文本1       文本2       文本3       文本4")
        while time.time() - started < image_delay + 25:
            page = client.call("GET", "/api/history?group=g1&before=0")
            status = {m["id"]: m["status"] for m in page.get("messages", []) if m["id"] in order}
            row = " ".join((status.get(mid, "-") or "-")[:10].ljust(10) for mid in order)
            print("%5.1fs %s" % (time.time() - started, row))
            if all(status.get(mid) in ("sent", "failed", "uncertain") for mid in order):
                break
            time.sleep(1.0)

        final = client.call("GET", "/api/history?group=g1&before=0")
        status = {m["id"]: m["status"] for m in final.get("messages", []) if m["id"] in order}
        print("\nfinal: image=%s texts=%s" % (
            status.get(image_id), [status.get(m) for m in text_ids]))
        blocked = sum(1 for m in text_ids if status.get(m) == "queued")
        print("texts still queued after %.0fs: %d/%d" % (
            time.time() - started, blocked, len(text_ids)))
    finally:
        process.terminate()
        try:
            process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            process.kill()
        log.close()
        # Print the diagnostics the relay/adapter emitted; the data dir is
        # temporary and would otherwise take them with it.
        try:
            output = open(os.path.join(data, "output.log")).read()
        except OSError:
            output = ""
        timing = [line for line in output.splitlines() if line.startswith("[timing]")]
        if timing:
            print("\n--- timing ---")
            for line in timing:
                print(line)
        else:
            print("\n(no [timing] lines; is the instrumented binary in use?)")
        shutil.rmtree(data, ignore_errors=True)


if __name__ == "__main__":
    main()
