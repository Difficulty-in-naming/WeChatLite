#!/usr/bin/env python3
"""A fake WeChat peer: accepts sends and answers 30 s later, on its own.

Two modes in one file.

Launcher (default)
    Creates an isolated relay config, starts the real `target/release/wechat-relay`
    with *this script* as its bridge program, selects group g1 for forwarding and
    prints the URL/token. Nothing touches WeChat, no real data directory is used.

Child (`--child`)
    Speaks the real NDJSON adapter protocol from adapters/PROTOCOL.md, so the
    relay runs its unmodified bridge/store/web code. For every `send` it:
      1. acknowledges after --ack-delay seconds (default 0.4), which models
         "WeChat wrote the row back into its local database";
      2. emits an inbound `message` --ack-delay + --reply-delay seconds after the
         send was accepted, i.e. ~30 s later by default;
      3. alternates that reply between text and image (--text-only disables).
    Inbound events go out on stdout exactly like the Windows adapter's, and the
    image is a real PNG written to the relay's data directory so the relay's
    media import, hashing and client download paths all run for real.

Why the timers are independent threads: this peer models *WeChat's* behaviour
(when a message becomes visible in the database), not the adapter's polling loop.
Adapter-side head-of-line blocking is what `[timing] inbox` in engine.rs measures.

Usage
    python3 tests/bench/fake_wechat_peer.py --data "$TMPDIR/peer" --reply-delay 30

Then in another shell:
    curl -s -H "Authorization: Bearer <token>" http://127.0.0.1:<port>/api/history?group=g1&before=0
Ctrl-C stops the relay.
"""
import argparse
import hashlib
import json
import os
import struct
import subprocess
import sys
import tempfile
import threading
import time
import urllib.request
import uuid
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
RELAY = os.path.join(ROOT, "target/release/wechat-relay")

GROUPS = [{"id": "g1", "name": "测试群一", "count": 4, "avatar": ""}]
MEMBERS = [
    {"id": "member-a", "name": "成员甲", "remark": "", "avatar": ""},
    {"id": "member-b", "name": "成员乙", "remark": "", "avatar": ""},
]

lock = threading.Lock()
T0 = time.time()
counter = {"send": 0, "reply": 0}


# ---------------------------------------------------------------- child mode


def emit(value):
    with lock:
        sys.stdout.write(json.dumps(value, ensure_ascii=False) + "\n")
        sys.stdout.flush()


def log(text):
    with lock:
        sys.stderr.write("[peer %.2fs] %s\n" % (time.time() - T0, text))
        sys.stderr.flush()


def png_bytes(size=48):
    """A genuinely decodable RGB PNG, so the relay's media checks really run."""

    def chunk(tag, data):
        body = tag + data
        return (
            struct.pack(">I", len(data))
            + body
            + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)
        )

    rows = b""
    for y in range(size):
        rows += b"\x00"
        for x in range(size):
            rows += bytes(((x * 5) % 256, (y * 5) % 256, 140))
    return (
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 2, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress(rows, 6))
        + chunk(b"IEND", b"")
    )


def sha256_file(path):
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for block in iter(lambda: handle.read(65536), b""):
            digest.update(block)
    return digest.hexdigest()


class Child:
    def __init__(self, args):
        self.ack_delay = args.ack_delay
        self.reply_delay = args.reply_delay
        self.reply_group = args.reply_group
        self.text_only = args.text_only
        self.media_dir = args.media_dir
        os.makedirs(self.media_dir, exist_ok=True)
        self.threads = []

    def write_png(self, index):
        path = os.path.join(self.media_dir, "peer_reply_%d.png" % index)
        with open(path, "wb") as handle:
            handle.write(png_bytes())
        return path

    # -- outbound confirmation ------------------------------------------------

    def confirm(self, message, started):
        time.sleep(self.ack_delay)
        group = message["group"]
        index = counter["send"]
        source = "wx:%s:peer:peer:%d" % (group, index)
        emit({
            "type": "ack",
            "id": message["id"],
            "status": "sent",
            "source_id": source,
            "source_ids": [source],
            "timestamp": int(time.time() * 1000),
        })
        log("ack #%d after %.1fs" % (index, time.time() - started))

    # -- inbound message ------------------------------------------------------

    def reply(self, group, send_index, send_started):
        # Reply at a FIXED offset from the send, independent of how long the ack
        # takes. Otherwise the harness itself delays the reply and any measured
        # "inbound is slow" would be an artifact of this formula, not of the relay.
        delay = max(0.0, self.reply_delay - (time.time() - send_started))
        time.sleep(delay)
        counter["reply"] += 1
        index = counter["reply"]
        with_image = not self.text_only and index % 2 == 0
        source = "wx:%s:peer:reply:%d" % (group, index)
        media = []
        text = "对方回复 #%d：收到你的消息了" % index
        if with_image:
            path = self.write_png(index)
            media = [{
                "id": "",
                "kind": "image",
                "name": os.path.basename(path),
                "size": os.path.getsize(path),
                "sha256": sha256_file(path),
                "path": path,
            }]
            text = ""
        emit({
            "type": "message",
            "historical": False,
            "message": {
                "id": str(uuid.uuid4()),
                "group": group,
                "sender": "成员甲",
                "sender_id": "member-a",
                "avatar": "",
                "text": text,
                "media": media,
                "mentions": [],
                "timestamp": int(time.time() * 1000),
                "status": "received",
                "source_id": source,
                "outgoing": False,
            },
        })
        log("reply #%d (%s) after %.1fs" % (
            index, "图片" if with_image else "文字",
            time.time() - send_started))

    def on_send(self, message):
        counter["send"] += 1
        started = time.time()
        log("send #%d received (%s)" % (
            counter["send"], "附件" if message.get("media") else "文字"))
        for target in (self.confirm, lambda m, s: self.reply(m["group"], counter["send"], s)):
            thread = threading.Thread(target=target, args=(message, started), daemon=True)
            thread.start()
            self.threads.append(thread)

    # -- main loop ------------------------------------------------------------

    def run(self):
        emit({"type": "groups", "groups": GROUPS})
        emit({
            "type": "status",
            "ready": True,
            "text": "模拟微信（本地，不连接真实微信）",
            "self_id": "wxid_peer",
            "recall_supported": False,
            "recall_reason": "模拟微信不支持撤回",
        })
        for line in sys.stdin:
            line = line.strip()
            if not line:
                continue
            try:
                command = json.loads(line)
            except ValueError:
                continue
            op = command.get("op")
            if op == "quit":
                break
            if op == "configure":
                log("configure %s" % (command.get("groups"),))
            elif op == "send":
                self.on_send(command["message"])
            elif op == "activate":
                emit({"type": "members",
                      "group": command.get("group"), "members": MEMBERS})
            elif op == "history":
                emit({
                    "type": "history_done",
                    "group": command.get("group"),
                    "request_id": command.get("request_id"),
                    "before_source": command.get("before_source", ""),
                    "sources": [],
                    "has_more": False,
                })
            elif op == "announcement":
                emit({
                    "type": "announcement",
                    "group": command.get("group"),
                    "request_id": command.get("request_id"),
                    "available": False, "set": False,
                    "author_id": "", "author": "", "avatar": "",
                    "updated_at": None, "text": "", "error": "unavailable",
                })
        log("stdin closed; %d background threads" % len(self.threads))
        for thread in self.threads:
            thread.join(timeout=2)


# ------------------------------------------------------------- launcher mode


def free_port():
    import socket
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


class Client:
    def __init__(self, port, token):
        self.base = "http://127.0.0.1:%d" % port
        self.token = token

    def call(self, method, path, body=None):
        data = json.dumps(body).encode() if body is not None else None
        request = urllib.request.Request(self.base + path, data=data, method=method)
        request.add_header("Authorization", "Bearer " + self.token)
        if body is not None:
            request.add_header("Content-Type", "application/json")
        try:
            with urllib.request.urlopen(request, timeout=10) as response:
                payload = response.read()
                return json.loads(payload) if payload else None
        except Exception:
            return None


def launch(args):
    data = os.path.abspath(args.data) if args.data else tempfile.mkdtemp(prefix="wclite-peer-")
    os.makedirs(data, exist_ok=True)
    port = args.port or free_port()
    token = "peer-" + uuid.uuid4().hex
    config = os.path.join(data, "server.local.json")
    child_args = [
        os.path.abspath(__file__), "--child",
        "--ack-delay", str(args.ack_delay),
        "--reply-delay", str(args.reply_delay),
        "--reply-group", args.reply_group,
        "--media-dir", os.path.join(data, "inbound"),
    ]
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

    print("relay   : %s" % args.relay)
    print("data    : %s" % data)
    print("url     : http://127.0.0.1:%d" % port)
    print("token   : %s" % token)
    print("reply   : about %.0fs after each accepted send" % args.reply_delay)
    print()
    print("Watch it:")
    print("  curl -s -H 'Authorization: Bearer %s' '%s/api/history?group=g1&before=0'" % (token, "http://127.0.0.1:%d" % port))
    print("  curl -s -H 'Authorization: Bearer %s' '%s/api/status'" % (token, "http://127.0.0.1:%d" % port))
    print()
    print("Send something as a client (image upload then message) with")
    print("  tests/bench/peer_send.py --url %s --token %s" % ("http://127.0.0.1:%d" % port, token))
    print()
    sys.stdout.flush()

    process = subprocess.Popen([args.relay, "--config", config])
    client = Client(port, token)
    try:
        # The adapter catalog arrives asynchronously; selecting before it lands
        # yields an empty selection and every send is then refused.
        deadline = time.time() + 30
        selected = None
        attempts = 0
        while time.time() < deadline and process.poll() is None:
            # /api/groups only lists already-selected groups; the catalog lives here.
            catalogue = client.call("GET", "/api/local/forwarding")
            groups = (catalogue or {}).get("groups") or []
            if any(g.get("id") == args.reply_group for g in groups):
                attempts += 1
                # A POST during adapter start-up can still land on an empty
                # catalog, so keep retrying until the relay echoes the selection.
                selected = client.call("POST", "/api/local/forwarding",
                                       {"selected": [args.reply_group]})
                if isinstance(selected, dict) and args.reply_group in (selected.get("selected") or []):
                    break
            time.sleep(0.3)
        if attempts > 1:
            print("forwarding: needed %d attempts" % attempts)
        if not selected or args.reply_group not in (selected.get("selected") or []):
            print("FAILED to select forwarding group %s: %s" % (args.reply_group, selected))
        else:
            print("forwarding: %s selected" % (selected.get("selected"),))
        sys.stdout.flush()
        process.wait()
    except KeyboardInterrupt:
        process.terminate()
        try:
            process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            process.kill()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--child", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("--data", help="isolated data directory (temp dir by default)")
    parser.add_argument("--port", type=int, default=0)
    parser.add_argument("--relay", default=RELAY)
    parser.add_argument("--ack-delay", type=float, default=0.4,
                        help="seconds until an outbound message is confirmed")
    parser.add_argument("--reply-delay", type=float, default=30.0,
                        help="seconds after an accepted send before the peer answers")
    parser.add_argument("--reply-group", default="g1")
    parser.add_argument("--media-dir", default="", help=argparse.SUPPRESS)
    parser.add_argument("--text-only", action="store_true",
                        help="reply with text only, never an image")
    args = parser.parse_args()
    if args.child:
        Child(args).run()
    else:
        launch(args)


if __name__ == "__main__":
    main()
