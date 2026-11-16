#!/usr/bin/env python3
"""Stage the built adapter and relay in the user's Windows VM shared test folder.

Only program files and their instructions are replaced. Previous versions are
moved to a dated backup; configuration, data and running services are untouched.
"""

import argparse
from datetime import datetime, timezone, timedelta
import hashlib
import json
import os
from pathlib import Path
import re
import tempfile
import uuid


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_DIRECTORY = Path(
    "/Volumes/MacData/cal_data/Downloads/WeChatLite-构建产物/WeChatLite-Server-测试版"
)
MARKER = "image-preview=cdn-footer-v3 media-wait=delayed-clear-v1"
ADAPTER_MARKER = MARKER + " inbox-poll=snapshot-v1 native-health=split-v1"
MANIFEST = "测试构建.json"


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--directory", type=Path, default=DEFAULT_DIRECTORY)
    parser.add_argument("--check", action="store_true", help="Verify without writing anything")
    args = parser.parse_args()
    directory = args.directory.resolve(strict=True)
    if not (directory / "wechat-server.exe").is_file():
        parser.error("目标必须是已有的测试服务端目录，缺少 wechat-server.exe")
    adapter = ROOT / "target/x86_64-pc-windows-gnu/release/wechat-adapter.exe"
    binary = adapter.read_bytes()
    if not binary.startswith(b"MZ") or ADAPTER_MARKER.encode() not in binary:
        parser.error("先构建 Windows 适配器；当前产物缺少本次功能标记")
    media_hashes = set(re.findall(rb"native-media-sha256=([a-f0-9]{64})", binary))
    if len(media_hashes) != 1:
        parser.error("适配器必须包含唯一的内嵌后台下载 DLL 指纹")
    media_hash = media_hashes.pop().decode("ascii")
    relay = (ROOT / "target/x86_64-pc-windows-gnu/release/wechat-relay.exe").read_bytes()
    if not relay.startswith(b"MZ") or MARKER.encode() not in relay:
        parser.error("先构建 Windows relay；缩略图升级需要适配器和 relay 同时更新")
    files = {
        "adapter/wechat-adapter.exe": binary,
        "wechat-relay.exe": relay,
        "启动测试版.bat": (ROOT / "scripts/windows-test-launcher.bat")
            .read_bytes().replace(b"\r\n", b"\n").replace(b"\n", b"\r\n"),
        "使用说明.md": (ROOT / "docs/TEST-SERVER-STEPS.md").read_bytes(),
    }
    manifest = {
        "feature": ADAPTER_MARKER,
        "native_media_sha256": media_hash,
        "files": [{"path": name, "sha256": sha256(data)} for name, data in files.items()],
    }
    files[MANIFEST] = (json.dumps(manifest, ensure_ascii=False, indent=2) + "\n").encode()
    changed = {
        name: data for name, data in files.items()
        if not (directory / name).is_file() or (directory / name).read_bytes() != data
    }
    if args.check and changed:
        parser.error("测试目录未同步：" + ", ".join(changed))
    if changed:
        stamp = datetime.now(timezone(timedelta(hours=8))).strftime("%Y%m%d-%H%M%S")
        backup = directory / "测试文件备份" / f"{stamp}-{uuid.uuid4().hex[:8]}"
        backup.mkdir(parents=True)
        records = []
        for name in changed:
            destination = directory / name
            if destination.is_symlink():
                parser.error(f"拒绝替换链接：{destination}")
            if destination.exists():
                records.append({"path": name, "sha256": sha256(destination.read_bytes())})
        (backup / "归档清单.json").write_text(
            json.dumps({"original_directory": str(directory), "files": records},
                       ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
        )
        for name, data in changed.items():
            destination = directory / name
            destination.parent.mkdir(parents=True, exist_ok=True)
            with tempfile.NamedTemporaryFile(dir=destination.parent, prefix=".stage-", delete=False) as staged:
                staged.write(data)
                temporary = Path(staged.name)
            previous = backup / name
            if destination.exists():
                previous.parent.mkdir(parents=True, exist_ok=True)
                destination.rename(previous)
            try:
                os.replace(temporary, destination)
            except OSError:
                if previous.exists() and not destination.exists():
                    previous.rename(destination)
                raise
        print("旧文件已归档：", backup)
    for name, data in files.items():
        if (directory / name).read_bytes() != data:
            raise RuntimeError(f"交付校验失败：{name}")
    print("测试目录已校验：", directory)
    print("适配器 SHA-256：", sha256(binary))
    print("Relay SHA-256：", sha256(relay))
    print("适配器启动标记：", ADAPTER_MARKER)
    print("内嵌后台下载 DLL SHA-256：", media_hash)
    print("用户在 Windows 里双击该目录的 启动测试版.bat；本脚本不启动或停止服务。")


if __name__ == "__main__":
    main()
