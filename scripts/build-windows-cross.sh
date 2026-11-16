#!/bin/bash
# 在 macOS 上交叉编译出可在 Windows 运行的两个 Rust 可执行文件。
#
# 只编 wechat-relay / wechat-adapter 两个纯 Rust crate；不编 Qt 客户端、
# 不编管理界面、不编 adapters/native 的 C++ DLL（那些需要 Windows + MSVC）。
#
# 前置依赖（一次性）：
#   brew install mingw-w64
#   rustup target add x86_64-pc-windows-gnu
#
# 产物：target/x86_64-pc-windows-gnu/release/*.exe
set -euo pipefail

cd "$(dirname "$0")/.."
ROOT=$PWD

# 工作区自带的 Rust 1.95（Homebrew 的 1.88 编不了 edition 2024）
export RUSTUP_HOME="$ROOT/.tools/rustup"
export CARGO_HOME="$ROOT/.tools/cargo"
export PATH="$ROOT/.tools/cargo/bin:$PATH"

TARGET=x86_64-pc-windows-gnu

command -v x86_64-w64-mingw32-gcc >/dev/null || {
    echo "缺少 mingw-w64，请先运行：brew install mingw-w64" >&2
    exit 1
}
rustup target list --installed | grep -qx "$TARGET" || {
    echo "缺少 Rust target，请先运行：rustup target add $TARGET" >&2
    exit 1
}

echo "== 编译 wechat-relay / wechat-adapter ($TARGET) =="
cargo build --release --locked \
    -p wechat-relay -p wechat-adapter \
    --target "$TARGET"

OUT="$ROOT/target/$TARGET/release"
echo
echo "== 产物 =="
ls -lh "$OUT/wechat-relay.exe" "$OUT/wechat-adapter.exe"
echo
echo "== 平台校验 =="
file "$OUT/wechat-relay.exe" "$OUT/wechat-adapter.exe" | sed 's/^/  /'
echo
echo "== 非系统 DLL 依赖（应为空）=="
for exe in "$OUT/wechat-relay.exe" "$OUT/wechat-adapter.exe"; do
    bad=$(x86_64-w64-mingw32-objdump -p "$exe" 2>/dev/null \
        | awk '/DLL Name:/{print $3}' \
        | grep -viE '^(api-ms-win-|ext-ms-)|^(kernel32|ntdll|advapi32|bcrypt|bcryptprimitives|ws2_32|wsock32|combase|crypt32|iphlpapi|ole32|oleaut32|user32|shell32|shlwapi|secur32|wintrust|userenv|version|psapi|dbghelp|powrprof)\.dll$' \
        | sort -u || true)
    if [ -n "$bad" ]; then
        echo "  $(basename "$exe") 依赖了非系统库："
        echo "$bad" | sed 's/^/    /'
        echo "  -> 需要把对应 DLL 一起复制到 Windows 目标目录" >&2
    else
        echo "  $(basename "$exe") OK"
    fi
done

echo
echo "部署到 Windows 服务端目录："
echo "  cp $OUT/wechat-relay.exe   <服务端目录>/"
echo "  cp $OUT/wechat-adapter.exe <服务端目录>/adapter/"
echo
echo "注意：adapter/ 下的 WeChatActions.dll / WeChatQuote.dll / ffmpeg.exe 不要动。"
echo "注意：wechat-server.exe（管理界面）本次未重新编译，日志落盘仍由旧版本负责。"
