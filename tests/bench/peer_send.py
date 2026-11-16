#!/usr/bin/env python3
"""Send text and/or images into the isolated peer relay, then watch both directions.

Pairs with tests/bench/fake_wechat_peer.py, which prints the --url and --token.
Nothing here reaches WeChat.

    python3 tests/bench/peer_send.py --url http://127.0.0.1:PORT --token TOKEN \
        --text "你好" --images 2

It prints a timeline of
  * your message statuses (queued/sending/sent)
  * every inbound message the peer replied with, and how late it arrived,
so "sending an image also delays receiving" becomes a number instead of a feeling.
"""
import argparse
import json
import struct
import sys
import time
import urllib.error
import urllib.request
import uuid
import zlib


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


class Client:
    def __init__(self, url, token):
        self.base = url.rstrip("/")
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

    def upload(self, group="g1", name="out.png"):
        return self.call("POST", "/api/media?name=%s&group=%s" % (name, group),
                         raw=png_bytes(),
                         headers={"Content-Type": "application/octet-stream"})


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--url", required=True)
    parser.add_argument("--token", required=True)
    parser.add_argument("--group", default="g1")
    parser.add_argument("--text", default="你好，这是客户端发的文字")
    parser.add_argument("--images", type=int, default=1,
                        help="how many images to attach to the message (0 for text only)")
    parser.add_argument("--pace", type=float, default=1.5,
                        help="seconds between two separate messages")
    parser.add_argument("--split", action="store_true",
                        help="send image message, then text, then a 2-image message (user's report)")
    parser.add_argument("--watch", type=float, default=120,
                        help="seconds to keep polling after sending")
    args = parser.parse_args()
    # This script POSTs messages. The repository forbids doing that to any real
    # relay, so refuse anything that is not an explicit loopback address.
    from urllib.parse import urlparse
    if urlparse(args.url).hostname not in {"127.0.0.1", "localhost", "::1"}:
        raise SystemExit("peer_send only talks to an isolated loopback relay")

    client = Client(args.url, args.token)
    status = client.call("GET", "/api/status")
    if not isinstance(status, dict) or status.get("__error__"):
        raise SystemExit("relay not reachable: %s" % status)
    print("relay ready=%s %s" % (status.get("ready"), status.get("text", "")))

    outbound = []  # (label, message id)
    started = time.time()

    def submit(label, text, image_count):
        body = {"id": str(uuid.uuid4()), "group": args.group,
                "client_id": "peer_send", "sender": "我", "mentions": []}
        media = []
        if image_count:
            uploaded = [client.upload(args.group, "out_%d_%d.png" % (len(outbound), i))
                        for i in range(image_count)]
            media = [item["id"] for item in uploaded if item and item.get("id")]
        body["text"] = text
        body["media"] = media
        reply = client.call("POST", "/api/messages", body)
        state = reply.get("status") if isinstance(reply, dict) else reply
        print("%6.1fs  POST %-10s -> %s" % (time.time() - started, label, state))
        outbound.append((label, body["id"]))

    if args.split:
        submit("图片", "", 1)
        time.sleep(args.pace)
        submit("文字", args.text, 0)
        time.sleep(args.pace)
        submit("两张图", "", 2)
    else:
        submit("单条", args.text, args.images)

    print("\n  t     outbound                         inbound (peer replies)")
    seen_inbound = {}
    last_line = None
    deadline = time.time() + args.watch
    while time.time() < deadline:
        page = client.call("GET", "/api/history?group=%s&before=0" % args.group)
        rows = page.get("messages", []) if isinstance(page, dict) else []
        by_id = {row["id"]: row for row in rows}
        out = " ".join(
            "%s=%s" % (label, by_id.get(mid, {}).get("status", "-") if mid in by_id else "-")
            for label, mid in outbound)
        arrivals = []
        for row in rows:
            if row.get("outgoing"):
                continue
            if row["id"] in seen_inbound:
                continue
            seen_inbound[row["id"]] = row
            arrivals.append(row)
        for row in arrivals:
            delay = time.time() - started
            peak = "图片" if row.get("media") else "文字"
            print("%6.1fs  %-31s ← %s 收到（发送后 %.1fs）" % (
                delay, out, peak, delay))
            last_line = None
        line = "%6.1fs  %s" % (time.time() - started, out)
        if line != last_line:
            print(line)
            last_line = line
        if all(by_id.get(mid, {}).get("status") in ("sent", "failed", "uncertain")
               for _, mid in outbound) and time.time() - started > 5:
            # Keep watching so inbound replies are still observed.
            pass
        time.sleep(1.0)
    print("\ninbound received: %d" % len(seen_inbound))


if __name__ == "__main__":
    main()
