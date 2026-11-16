#!/usr/bin/env python3
"""One-command demo: I send image+text, the fake peer answers ~30 s later.

Starts an isolated relay (temp data dir, random loopback port) whose adapter is
tests/bench/fake_wechat_peer.py --child. Then it plays *both* sides:

  * sends like your desktop client:  image -> text -> two images
  * receives like your phone:      the peer replies 30 s after each send,
                                   alternating text and image

Every action is printed on one timeline, so you can see whether an inbound reply
is really late, and by how much. Nothing here touches real WeChat.

Usage
    python3 tests/bench/peer_demo.py                 # 30 s replies, user's sequence
    python3 tests/bench/peer_demo.py --reply-delay 10
    python3 tests/bench/peer_demo.py --single        # one message: text + 2 images
    python3 tests/bench/peer_demo.py --watch 180     # observe longer
"""
import argparse
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
import urllib.parse
import urllib.request
import uuid
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
RELAY = os.path.join(ROOT, "target/release/wechat-relay")
PEER = os.path.join(HERE, "fake_wechat_peer.py")


def png_bytes(size=48):
    def chunk(tag, data):
        body = tag + data
        return (struct.pack(">I", len(data)) + body
                + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF))

    rows = b""
    for y in range(size):
        rows += b"\x00"
        for x in range(size):
            rows += bytes(((x * 5) % 256, (y * 5) % 256, 140))
    return (b"\x89PNG\r\n\x1a\n"
            + chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(rows, 6))
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
        data = raw if raw is not None else (
            json.dumps(body).encode() if body is not None else None)
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
            return {"__error__": error.code,
                    "body": error.read().decode(errors="replace")[:300]}
        except urllib.error.URLError:
            # Relay socket not listening yet; the readiness loop retries.
            return None


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--reply-delay", type=float, default=30.0)
    parser.add_argument("--ack-delay", type=float, default=0.4,
                        help="seconds until a send is confirmed (models WeChat write-back)")
    parser.add_argument("--pace", type=float, default=1.5,
                        help="seconds between the three sends")
    parser.add_argument("--watch", type=float, default=150,
                        help="seconds to keep watching after the last send")
    parser.add_argument("--single", action="store_true",
                        help="send one message (text + 2 images) instead of three")
    parser.add_argument("--text-only", action="store_true",
                        help="peer replies with text only")
    args = parser.parse_args()

    data = tempfile.mkdtemp(prefix="wclite-demo-")
    port = free_port()
    token = "demo-" + uuid.uuid4().hex
    config = os.path.join(data, "server.local.json")
    # bridge_program already runs sys.executable; args start with the script.
    child_args = [PEER, "--child",
                  "--ack-delay", str(args.ack_delay),
                  "--reply-delay", str(args.reply_delay),
                  "--media-dir", os.path.join(data, "inbound")]
    if args.text_only:
        child_args.append("--text-only")
    with open(config, "w") as handle:
        json.dump({
            "listen": "127.0.0.1:%d" % port,
            "token": token,
            "data": os.path.join(data, "state"),
            "bridge_program": sys.executable,
            "bridge_args": child_args,
            "tls_cert": "",
            "tls_key": "",
        }, handle, indent=2)

    log_path = os.path.join(data, "relay.log")
    log = open(log_path, "w")
    process = subprocess.Popen([RELAY, "--config", config],
                               stdout=log, stderr=subprocess.STDOUT, text=True)
    client = Client(port, token)
    print("隔离演示实例：%s（临时目录 %s，不连接真实微信）" % (client.base, data))
    print("模拟对方：每条消息被接受后约 %.0f 秒回复，文字/图片交替\n" % args.reply_delay)

    outbound = []
    started = time.time()  # the timeline starts when the demo starts

    def submit(label, text, image_count):
        body = {"id": str(uuid.uuid4()), "group": "g1", "client_id": "demo",
                "sender": "我", "mentions": []}
        media = []
        for index in range(image_count):
            uploaded = client.call(
                "POST", "/api/media?name=%s_%d.png&group=g1"
                % (urllib.parse.quote(label), index),
                raw=png_bytes(),
                headers={"Content-Type": "application/octet-stream"})
            if isinstance(uploaded, dict) and uploaded.get("id"):
                media.append(uploaded["id"])
        body["text"] = text
        body["media"] = media
        reply = client.call("POST", "/api/messages", body)
        state = reply.get("status") if isinstance(reply, dict) else reply
        print("%6.1fs  我发出 %-6s（%d 张图）→ 服务端 %s" % (
            time.time() - started, label, image_count, state))
        outbound.append((label, body["id"]))

    try:
        deadline = time.time() + 30
        while time.time() < deadline:
            status = client.call("GET", "/api/status")
            if isinstance(status, dict) and status.get("ready") is True:
                break
            time.sleep(0.2)
        else:
            raise SystemExit("relay never became ready")

        # /api/groups only lists already-selected groups, so the discovered
        # adapter catalog has to come from the local forwarding endpoint.
        chosen = None
        for _ in range(60):
            catalogue = client.call("GET", "/api/local/forwarding")
            available = (catalogue or {}).get("groups") or []
            if available:
                chosen = client.call("POST", "/api/local/forwarding",
                                     {"selected": [g["id"] for g in available]})
                if isinstance(chosen, dict) and chosen.get("selected"):
                    break
            time.sleep(0.5)
        if not (isinstance(chosen, dict) and chosen.get("selected")):
            raise SystemExit(
                "could not select a forwarding group; last reply=%r status=%r"
                % (chosen, client.call("GET", "/api/status")))
        print("转发群：%s\n" % (chosen.get("selected"),))

        started = time.time()
        if args.single:
            submit("单条", "这条消息带两张图", 2)
        else:
            submit("图片", "", 1)
            time.sleep(args.pace)
            submit("文字", "这段文字排在图片后面", 0)
            time.sleep(args.pace)
            submit("两图", "", 2)

        print("\n  t     我发出的状态                        对方回复")
        seen = {}
        last_line = None
        deadline = time.time() + args.watch
        while time.time() < deadline:
            page = client.call("GET", "/api/history?group=g1&before=0")
            rows = page.get("messages", []) if isinstance(page, dict) else []
            by_id = {row["id"]: row for row in rows}
            mine = "  ".join(
                "%s=%s" % (label, by_id.get(mid, {}).get("status", "-"))
                for label, mid in outbound)
            arrivals = []
            for row in rows:
                if row.get("outgoing") or row["id"] in seen:
                    continue
                seen[row["id"]] = row
                arrivals.append(row)
            for row in arrivals:
                kind = "图片" if row.get("media") else "文字"
                print("%6.1fs  %-34s  ← 收到对方%s" % (
                    time.time() - started, mine, kind))
                last_line = None
            line = "%6.1fs  %s" % (time.time() - started, mine)
            if line != last_line:
                print(line)
                last_line = line
            time.sleep(1.0)

        print("\n小结：我发出 %d 条，收到对方 %d 条" % (len(outbound), len(seen)))
        timing = [l for l in open(log_path, errors="replace").read().splitlines()
                  if l.startswith("[timing]")]
        if timing:
            print("\n--- timing（服务端/适配器诊断）---")
            for line in timing[-20:]:
                print(line)
    finally:
        process.terminate()
        try:
            process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            process.kill()
        log.close()
        shutil.rmtree(data, ignore_errors=True)


if __name__ == "__main__":
    main()
