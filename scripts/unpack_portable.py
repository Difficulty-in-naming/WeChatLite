#!/usr/bin/env python3
"""解包 / 重打包 WeChatLite 的 Windows 便携单文件 EXE。

便携包的格式（见 packaging/windows-portable/FORMAT.md）：

    [启动器 PE] [WLPKG01\\n] [u64 清单长度] [JSON 清单] [各文件字节...] [WLPEND1\\n] [u64 offset] [u64 size]

启动器把「整个 payload 的 SHA256」编译进自己，启动时校验。所以改包内文件后
必须重新生成 payload 并重编启动器 —— 不能只替换字节。

用法：
    unpack_portable.py list   <portable.exe>
    unpack_portable.py unpack <portable.exe> <目标目录>
    unpack_portable.py pack   <源目录> <输出.exe> <启动器模板.exe>

pack 时会从模板 EXE 里复用启动器那段 PE，只替换后面的 payload；
但复用启动器意味着旧哈希仍在里面，因此 pack 需要一个「哈希已更新」的启动器。
见 rebuild_portable_server.sh：它会重新编译启动器再调用这里。
"""
import hashlib
import json
import os
import struct
import sys

MAGIC = b"WLPKG01\n"
FOOTER = b"WLPEND1\n"


def read_package(path):
    with open(path, "rb") as handle:
        data = handle.read()
    if not data.endswith(b"\x00" * 0) and len(data) < 24:
        raise SystemExit("文件太小，不是便携包")
    footer = data[-24:]
    if footer[:8] != FOOTER:
        raise SystemExit("找不到便携包 footer（WLPEND1）；这不是单文件便携包")
    offset, size = struct.unpack("<QQ", footer[8:24])
    if offset + size != len(data) - 24:
        raise SystemExit("footer 偏移与文件长度不符，包已损坏")
    payload = data[offset : offset + size]
    if payload[:8] != MAGIC:
        raise SystemExit("payload 缺少 WLPKG01 头")
    manifest_length = struct.unpack("<Q", payload[8:16])[0]
    manifest = json.loads(payload[16 : 16 + manifest_length])
    body = payload[16 + manifest_length :]
    entries = []
    position = 0
    for entry in manifest["files"]:
        chunk = body[position : position + entry["size"]]
        if len(chunk) != entry["size"]:
            raise SystemExit("payload 被截断：%s" % entry["path"])
        digest = hashlib.sha256(chunk).hexdigest()
        if digest != entry["sha256"]:
            raise SystemExit("清单哈希不匹配：%s" % entry["path"])
        entries.append((entry["path"], chunk))
        position += entry["size"]
    if position != len(body):
        raise SystemExit("payload 尾部有多余数据")
    return {
        "launcher": data[:offset],
        "manifest": manifest,
        "entries": entries,
        "payload_sha256": hashlib.sha256(payload).hexdigest(),
        "exe_sha256": hashlib.sha256(data).hexdigest(),
    }


def unpack(source, destination):
    package = read_package(source)
    print("角色      : %s" % package["manifest"]["role"])
    print("入口      : %s" % package["manifest"]["entry"])
    print("文件数    : %d" % len(package["entries"]))
    print("payload   : %s" % package["payload_sha256"])
    print()
    for relative, chunk in package["entries"]:
        target = os.path.join(destination, *relative.split("/"))
        os.makedirs(os.path.dirname(target), exist_ok=True)
        with open(target, "wb") as handle:
            handle.write(chunk)
        print("  %10d  %s" % (len(chunk), relative))
    print("\n已解包到 %s" % destination)


def pack(directory, output, launcher_source):
    """用 launcher_source 的启动器段 + 重新计算的 payload 生成新 EXE。

    注意：启动器里的 PORTABLE_SHA256 必须已经等于新 payload 的哈希，
    否则新 EXE 启动时会报“complete package SHA256 mismatch”。
    先用 rebuild_portable_server.sh 重编启动器。
    """
    base = read_package(launcher_source)
    launcher = base["launcher"]

    manifest = base["manifest"]
    files = []
    body = bytearray()
    for entry in manifest["files"]:
        relative = entry["path"]
        target = os.path.join(directory, *relative.split("/"))
        if not os.path.isfile(target):
            raise SystemExit("缺少文件：%s" % relative)
        with open(target, "rb") as handle:
            chunk = handle.read()
        files.append(
            {"path": relative, "size": len(chunk), "sha256": hashlib.sha256(chunk).hexdigest()}
        )
        body += chunk
    manifest["files"] = files
    manifest_bytes = json.dumps(manifest, separators=(",", ":")).encode()
    payload = MAGIC + struct.pack("<Q", len(manifest_bytes)) + manifest_bytes + bytes(body)
    payload_hash = hashlib.sha256(payload).hexdigest()

    with open(output, "wb") as handle:
        handle.write(launcher)
        handle.write(payload)
        handle.write(FOOTER)
        handle.write(struct.pack("<QQ", len(launcher), len(payload)))

    print("payload SHA256 : %s" % payload_hash)
    print("EXE     SHA256 : %s" % hashlib.sha256(open(output, "rb").read()).hexdigest())
    print("输出           : %s" % output)
    print()
    print("启动器内嵌的哈希必须等于上面的 payload SHA256。")
    print("如果不一致，先在 Windows 侧或重编启动器时把 WECHAT_PORTABLE_PAYLOAD 指向这个 payload。")


def main():
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    command = sys.argv[1]
    if command == "list":
        package = read_package(sys.argv[2])
        print("角色    : %s" % package["manifest"]["role"])
        print("入口    : %s" % package["manifest"]["entry"])
        print("文件数  : %d" % len(package["entries"]))
        for relative, chunk in package["entries"]:
            print("  %10d  %s" % (len(chunk), relative))
    elif command == "unpack":
        if len(sys.argv) != 4:
            raise SystemExit("用法: unpack_portable.py unpack <portable.exe> <目标目录>")
        unpack(sys.argv[2], sys.argv[3])
    elif command == "pack":
        if len(sys.argv) != 5:
            raise SystemExit("用法: unpack_portable.py pack <源目录> <输出.exe> <启动器模板.exe>")
        pack(sys.argv[2], sys.argv[3], sys.argv[4])
    else:
        raise SystemExit(__doc__)


if __name__ == "__main__":
    main()
