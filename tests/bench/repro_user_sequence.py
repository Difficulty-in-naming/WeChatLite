#!/usr/bin/env python3
"""Reproduce the user-reported sequence: image -> text -> two images.

Everything runs against an isolated relay (temporary data dir, random loopback
port) whose bridge is fake_adapter.py. No WeChat process is involved and no
real relay instance is touched.

The fake adapter charges the confirmation delay once per attachment step,
because the real adapter confirms a ChatMessage step by step: one step for the
text, then one step per attachment, each waiting for its own native server_id
(adapters/windows-rs/src/engine.rs: Job::advance / check_step).

Usage:
    python3 tests/bench/repro_user_sequence.py [per_image_seconds] [--burst]

    per_image_seconds  confirmation cost of one image (default 60)
    --burst            post all three messages immediately instead of pacing
                       them like a human typing (default: 1.5 s apart)
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
            return {"__error__": error.code, "body": error.read().decode(errors="replace")[:300]}


def upload(client, name="test.png"):
    result = client.call("POST", "/api/media?name=%s&group=g1" % name, raw=png_bytes(),
                         headers={"Content-Type": "application/octet-stream"})
    if not result.get("id"):
        raise SystemExit("upload failed: %s" % result)
    return result["id"]


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    burst = "--burst" in sys.argv[1:]
    per_image = float(args[0]) if args else 60.0

    data = tempfile.mkdtemp(prefix="wclite-userseq-")
    port = free_port()
    token = "userseq-" + uuid.uuid4().hex
    config = os.path.join(data, "server.local.json")
    with open(config, "w") as handle:
        json.dump({
            "listen": "127.0.0.1:%d" % port,
            "token": token,
            "data": os.path.join(data, "state"),
            "bridge_program": sys.executable,
            "bridge_args": [ADAPTER, "--image-delay", str(per_image), "--per-media"],
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
        time.sleep(0.3)

        # 电脑客户端：一张图 -> 一段文字 -> 两张图（同一消息，两个附件）。
        one, two, three = upload(client, "one.png"), upload(client, "two.png"), upload(client, "three.png")
        steps = [
            ("图片", {"text": "", "media": [one]}),
            ("文字", {"text": "这段文字要等前面的图片排队", "media": []}),
            ("两张图", {"text": "", "media": [two, three]}),
        ]
        ids, labels = [], []
        started = time.time()
        print("按用户描述依次发送：图片 -> 文字 -> 两张图（单张图片确认耗时假设 %.0fs）\n" % per_image)
        for label, payload in steps:
            mid = str(uuid.uuid4())
            body = {"id": mid, "group": "g1", "client_id": "userseq", "sender": "我",
                    "mentions": []}
            body.update(payload)
            reply = client.call("POST", "/api/messages", body)
            if reply.get("status") != "queued":
                raise SystemExit("send rejected: %s" % reply)
            ids.append(mid)
            labels.append(label)
            print("%5.1fs  ← 客户端提交 %s（服务端 %s）" % (time.time() - started, label, reply["status"]))
            if not burst:
                time.sleep(1.5)

        header = "  t   " + "".join(label.ljust(12) for label in labels)
        print("\n" + header)
        last_row = None
        while True:
            page = client.call("GET", "/api/history?group=g1&before=0")
            status = {m["id"]: m["status"] for m in page.get("messages", []) if m["id"] in ids}
            row = [status.get(mid, "-") or "-" for mid in ids]
            if row != last_row:
                print("%5.1fs " % (time.time() - started)
                      + "".join(value[:11].ljust(12) for value in row))
                last_row = row
            if all(value in ("sent", "failed", "uncertain") for value in row):
                break
            if time.time() - started > per_image * 6 + 60:
                print("(超时，仍在等待)")
                break
            time.sleep(1.0)
        elapsed = time.time() - started
        print("\n全部结束用时 %.1fs（%.1f 分钟）" % (elapsed, elapsed / 60.0))
    finally:
        process.terminate()
        try:
            process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            process.kill()
        log.close()
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
