#!/usr/bin/env python3
"""Run the relay locally on macOS against a configurable fake adapter.

Purpose: let the queue behaviour be reproduced on this machine. The real
Windows adapter cannot run here (adapters/windows-rs/src/main.rs requires
cfg!(windows) and the WeChat process), so this substitutes a stand-in that
speaks the same NDJSON protocol. The relay code under test is the real binary.

Everything lives under a caller-supplied data directory. It never touches
WeChat, and it refuses to run against a directory that already has data unless
--force is given.
"""
import argparse
import json
import os
import shutil
import socket
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))


def free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--data", required=True, help="isolated data directory")
    parser.add_argument("--port", type=int, default=0)
    parser.add_argument("--relay", default=os.path.join(ROOT, "target/release/wechat-relay"))
    parser.add_argument("--image-delay", type=float, default=120.0,
                        help="seconds before an image is acknowledged (engine.rs:877 uses 120)")
    parser.add_argument("--force", action="store_true")
    args = parser.parse_args()

    data = os.path.abspath(args.data)
    if os.path.exists(os.path.join(data, "state", "relay.sqlite")) and not args.force:
        raise SystemExit("data directory already has a relay database; pass --force to reuse it")
    os.makedirs(data, exist_ok=True)
    port = args.port or free_port()
    token = "local-" + os.urandom(16).hex()
    config_path = os.path.join(data, "server.local.json")

    config = {
        "listen": "127.0.0.1:%d" % port,
        "token": token,
        "data": os.path.join(data, "state"),
        "bridge_program": sys.executable,
        "bridge_args": [
            os.path.join(HERE, "fake_adapter.py"),
            "--image-delay", str(args.image_delay),
        ],
        "tls_cert": "",
        "tls_key": "",
    }
    with open(config_path, "w") as handle:
        json.dump(config, handle, indent=2)

    print("relay   : %s" % args.relay)
    print("config  : %s" % config_path)
    print("url     : http://127.0.0.1:%d" % port)
    print("token   : %s" % token)
    print("adapter : fake (image ack delay %.1fs)" % args.image_delay)
    print()
    print("Try:")
    print("  curl -s -H 'Authorization: Bearer %s' http://127.0.0.1:%d/api/status" % (token, port))
    print()
    sys.stdout.flush()

    process = subprocess.Popen([args.relay, "--config", config_path])
    try:
        process.wait()
    except KeyboardInterrupt:
        process.terminate()
        process.wait(timeout=10)


if __name__ == "__main__":
    main()
