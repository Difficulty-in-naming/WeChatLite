#!/usr/bin/env python3
"""Stage the packaged Mac test client without reading configuration or user data."""

import argparse
from datetime import datetime
import hashlib
import json
from pathlib import Path
import plistlib
import shutil
import subprocess
import tempfile
import uuid


ROOT = Path(__file__).resolve().parents[1]
DIRECTORY = Path("/Volumes/MacData/cal_data/Downloads/WeChatLite-构建产物")
APP = "微信轻聊-测试版.app"
MANIFEST = "测试客户端构建.json"
FEATURE = "media-wait=delayed-clear-v1"


def digest(data):
    return hashlib.sha256(data).hexdigest()


def snapshot(app):
    app = app.resolve(strict=True)
    records = []
    for path in sorted(app.rglob("*")):
        name = str(path.relative_to(app))
        if path.is_symlink():
            if not path.resolve(strict=True).is_relative_to(app):
                raise RuntimeError(f"客户端包含外部链接：{name}")
            records.append({"path": name, "link": str(path.readlink())})
        elif path.is_file():
            records.append({"path": name, "sha256": digest(path.read_bytes())})
    return records


def package():
    folder = ROOT / "build-macos/package-staging"
    folder.mkdir(parents=True, exist_ok=True)
    app = Path(tempfile.mkdtemp(prefix="media-warning-", dir=folder)) / APP
    subprocess.run(["ditto", str(ROOT / "build-macos/client/wechat-lite.app"), str(app)], check=True)
    helpers = [ROOT / "target/release/wechat-link"]
    demo = ROOT / "target/release/wechat-relay"
    if demo.is_file():
        helpers.append(demo)
    for helper in helpers:
        shutil.copy2(helper, app / "Contents/MacOS" / helper.name)
    resources = app / "Contents/Resources"
    resources.mkdir(exist_ok=True)
    shutil.copy2(ROOT / "docs/CLIENT.md", resources / "使用说明.md")
    shutil.copy2(ROOT / "THIRD_PARTY_NOTICES.md", resources / "THIRD_PARTY_NOTICES.md")
    shutil.copytree(ROOT / "network/licenses", resources / "licenses", dirs_exist_ok=True)
    plist = app / "Contents/Info.plist"
    info = plistlib.loads(plist.read_bytes())
    info.update(CFBundleIdentifier="local.wechatlite.client.test", CFBundleName="微信轻聊-测试版",
                CFBundleDisplayName="微信轻聊-测试版", WeChatLiteMediaWait=FEATURE)
    info["CFBundleShortVersionString"] += "-test"
    plist.write_bytes(plistlib.dumps(info))
    deploy = ROOT / ".qt/6.8.3/macos/bin/macdeployqt"
    subprocess.run([str(deploy), str(app), "-always-overwrite", "-codesign=-"] +
                   [f"-executable={app / 'Contents/MacOS' / helper.name}" for helper in helpers], check=True)
    for helper in helpers:
        subprocess.run(["codesign", "--force", "--sign", "-", str(app / "Contents/MacOS" / helper.name)], check=True)
    subprocess.run(["codesign", "--force", "--sign", "-", str(app)], check=True)
    subprocess.run(["codesign", "--verify", "--deep", "--strict", str(app)], check=True)
    subprocess.run(["cmake", f"-DAPP={app}", "-DAPP_BINARY=wechat-lite", "-DHELPER_BINARY=wechat-link",
                    "-P", str(ROOT / "scripts/verify-macos-bundle.cmake")], check=True)
    return app


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--app", type=Path, help="Already packaged and signed test app")
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--package-only", action="store_true", help="Package inside the repository without staging")
    args = parser.parse_args()
    if args.package_only:
        print("客户端封装完成：", package())
        return
    directory = DIRECTORY.resolve(strict=True)
    target = directory / APP
    manifest_path = directory / MANIFEST
    if target.is_symlink() or manifest_path.is_symlink():
        raise RuntimeError("拒绝替换链接")
    built = ROOT / "build-macos/client/wechat-lite.app/Contents/MacOS/wechat-lite"
    if args.check:
        manifest = json.loads(manifest_path.read_text())
        assert manifest["feature"] == FEATURE, "客户端功能标记不匹配"
        assert manifest["source_executable_sha256"] == digest(built.read_bytes()), "客户端构建已变化，需重新交付"
        assert manifest["files"] == snapshot(target), "客户端交付文件不一致"
    else:
        source = (args.app or package()).resolve(strict=True)
        if not source.is_relative_to(ROOT / "build-macos/package-staging"):
            parser.error("来源必须在仓库 build-macos/package-staging 内")
        info = plistlib.loads((source / "Contents/Info.plist").read_bytes())
        assert info["CFBundleIdentifier"] == "local.wechatlite.client.test"
        assert info["WeChatLiteMediaWait"] == FEATURE
        subprocess.run(["codesign", "--verify", "--deep", "--strict", str(source)], check=True)
        manifest = {"feature": FEATURE, "source_executable_sha256": digest(built.read_bytes()),
                    "files": snapshot(source)}
        data = (json.dumps(manifest, ensure_ascii=False, indent=2) + "\n").encode()
        if (not target.exists() or snapshot(target) != manifest["files"]
                or not manifest_path.exists() or manifest_path.read_bytes() != data):
            staged = directory / f".stage-client-{uuid.uuid4().hex}.app"
            subprocess.run(["ditto", str(source), str(staged)], check=True)
            assert snapshot(staged) == manifest["files"], "客户端复制校验失败"
            backup = directory / "测试文件备份" / f"{datetime.now():%Y%m%d-%H%M%S}-client-{uuid.uuid4().hex[:8]}"
            backup.mkdir(parents=True)
            (backup / "归档清单.json").write_text(json.dumps({
                "original_directory": str(directory),
                "files": [name for name in [APP, MANIFEST] if (directory / name).exists()]
            }, ensure_ascii=False, indent=2) + "\n")
            if target.exists():
                target.rename(backup / APP)
            try:
                staged.rename(target)
            except OSError:
                if (backup / APP).exists() and not target.exists():
                    (backup / APP).rename(target)
                raise
            if manifest_path.exists():
                manifest_path.rename(backup / MANIFEST)
            print("旧客户端已归档：", backup)
        manifest_path.write_bytes(data)
    assert snapshot(target) == manifest["files"]
    print("测试客户端已校验：", target)
    print("客户端 SHA-256：", digest((target / "Contents/MacOS/wechat-lite").read_bytes()))
    print("功能标记：", FEATURE)
    print("原 启动测试版客户端.command 保持不变；本脚本不启动客户端或服务。")


if __name__ == "__main__":
    main()
