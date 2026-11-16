#!/usr/bin/env python3
"""Stand-in for the Windows WeChat adapter. Never touches WeChat.

Speaks the NDJSON protocol from adapters/PROTOCOL.md so the real relay binary
runs its unmodified bridge/store/web code. This exists because the real adapter
requires Windows and a logged-in WeChat process.
"""
import argparse
import json
import sys
import threading
import time

GROUPS = [
    {"id": "g1", "name": "测试群一", "count": 3, "avatar": ""},
    {"id": "g2", "name": "测试群二", "count": 3, "avatar": ""},
]
MEMBERS = [
    {"id": "member-a", "name": "成员甲", "remark": "", "avatar": ""},
    {"id": "member-b", "name": "成员乙", "remark": "", "avatar": ""},
]

lock = threading.Lock()
T0 = time.time()
counter = {"n": 0}


def emit(value):
    with lock:
        sys.stdout.write(json.dumps(value) + "\n")
        sys.stdout.flush()


def log(text):
    with lock:
        sys.stderr.write("[fake-adapter %.2fs] %s\n" % (time.time()-T0, text))
        sys.stderr.flush()


def is_image(message):
    return bool(message.get("media"))


def confirm(index, message, image_delay, per_media=False):
    media = message.get("media") or []
    if per_media and media:
        # The real adapter confirms one step per attachment, each waiting for its
        # own native server_id, so a 2-image message costs two image delays.
        delay = image_delay * len(media) + (0.3 if message.get("text", "").strip() else 0.0)
    else:
        delay = image_delay if media else 0.3
    started = time.time()
    time.sleep(delay)
    group = message["group"]
    source = "wx:%s:fake:fake:%d" % (group, index)
    emit({
        "type": "ack",
        "id": message["id"],
        "status": "sent",
        "source_id": source,
        "source_ids": [source],
        "timestamp": int(time.time() * 1000),
    })
    log("ack %s (%s) after %.1fs" % (
        message["id"][:8], "image" if is_image(message) else "text", time.time() - started))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--image-delay", type=float, default=120.0)
    parser.add_argument("--per-media", action="store_true",
                        help="charge image_delay once per attachment (serial per-step confirmation)")
    args = parser.parse_args()

    log("pid=%d image_delay=%.1fs per_media=%s" % (
        os.getpid() if hasattr(os, "getpid") else 0, args.image_delay, args.per_media))
    emit({"type": "groups", "groups": GROUPS})
    emit({
        "type": "status",
        "ready": True,
        "text": "本地测试适配器（不连接微信）",
        "self_id": "wxid_fake",
        "recall_supported": False,
        "recall_reason": "本地测试模式不支持撤回",
    })

    threads = []
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
            log("configure %s" % command.get("groups"))
        elif op == "send":
            counter["n"] += 1
            message = command["message"]
            log("send %s kind=%s" % (message["id"][:8], "image" if is_image(message) else "text"))
            thread = threading.Thread(
                target=confirm, args=(counter["n"], message, args.image_delay, args.per_media), daemon=True)
            thread.start()
            threads.append(thread)
        elif op == "activate":
            emit({"type": "members", "group": command.get("group"), "members": MEMBERS})
        elif op == "history":
            # history.finish() requires before_source; omitting it makes the
            # relay treat the reply as a protocol error and tear down the pipe.
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
                "available": False, "set": False, "author_id": "", "author": "",
                "avatar": "", "updated_at": None, "text": "", "error": "unavailable",
            })
    log("STDIN EOF - exiting (threads=%d)" % len(threads))
    for thread in threads:
        thread.join(timeout=5)


if __name__ == "__main__":
    import os
    main()
