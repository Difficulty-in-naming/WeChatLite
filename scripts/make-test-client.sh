#!/bin/bash
# 复制一份「微信轻聊」app 作为测试版，与正式版完全隔离。
#
# 隔离三样东西：
#   1. Bundle 路径   -> 独立 .app，不和 /Applications 或 ~/Applications 冲突
#   2. Bundle ID     -> local.wechatlite.client.test，macOS 视为另一个应用
#                        （通知授权、LaunchServices 记录、TCC 都独立）
#   3. 运行环境      -> WECHAT_LITE_PROFILE 指向独立目录，配置不共用
#                        HOME/XDG 缓存指向独立目录，消息索引与媒体缓存不共用
#
# 不改动正式版任何文件；测试版删除即可彻底清理。
#
# 用法：
#   scripts/make-test-client.sh                       # 用已构建的 app
#   scripts/make-test-client.sh --build               # 先构建再复制
set -euo pipefail

cd "$(dirname "$0")/.."
ROOT=$PWD

SOURCE=${SOURCE:-"$ROOT/build-macos/client/wechat-lite.app"}
DEST=${DEST:-"$ROOT/build-macos/test-client/微信轻聊-测试版.app"}
PROFILE=${PROFILE:-"$ROOT/.state/test-client"}
TEST_ID=local.wechatlite.client.test
TEST_NAME="微信轻聊-测试版"

if [[ "${1:-}" == "--build" ]]; then
    echo "== 构建客户端 =="
    if [[ -z "${QT_PREFIX:-}" && -d "$ROOT/.qt/6.8.3/macos" ]]; then
        export QT_PREFIX="$ROOT/.qt/6.8.3/macos"
    fi
    cmake -S . -B build-macos -DCMAKE_PREFIX_PATH="${QT_PREFIX:?请设置 QT_PREFIX 或先安装 Qt}" >/dev/null
    cmake --build build-macos --config Release --target wechat-lite --parallel 4
fi

[[ -d "$SOURCE" ]] || { echo "找不到 $SOURCE，先构建或传 SOURCE=<path>" >&2; exit 1; }

echo "== 复制 app =="
rm -rf "$DEST"
mkdir -p "$(dirname "$DEST")"
ditto "$SOURCE" "$DEST"

# Demo mode runs the local relay. Ship it inside the test app so the test build works
# without a source checkout (the release client does not include it).
RELAY=${RELAY:-"$ROOT/target/release/wechat-relay"}
if [[ -x "$RELAY" ]]; then
    cp "$RELAY" "$DEST/Contents/MacOS/wechat-relay"
    echo "  已内置演示服务 wechat-relay"
else
    echo "  未找到 $RELAY，--demo 将不可用（按需先 cargo build --release -p wechat-relay）" >&2
fi

PLIST="$DEST/Contents/Info.plist"
[[ -f "$PLIST" ]] || { echo "缺少 Info.plist" >&2; exit 1; }

echo "== 改写 bundle 身份 =="
/usr/libexec/PlistBuddy -c "Set :CFBundleIdentifier $TEST_ID" "$PLIST"
/usr/libexec/PlistBuddy -c "Set :CFBundleName $TEST_NAME" "$PLIST" 2>/dev/null || true
/usr/libexec/PlistBuddy -c "Set :CFBundleDisplayName $TEST_NAME" "$PLIST" 2>/dev/null \
    || /usr/libexec/PlistBuddy -c "Add :CFBundleDisplayName string $TEST_NAME" "$PLIST"
# 版本号加后缀，设置页和崩溃日志里一眼能区分
/usr/libexec/PlistBuddy -c "Set :CFBundleShortVersionString $(/usr/libexec/PlistBuddy -c 'Print :CFBundleShortVersionString' "$PLIST")-test" "$PLIST" 2>/dev/null || true

# 改过 Info.plist 后原签名失效；ad-hoc 重签，本机运行足够。
# 正式版仍在它自己的路径下，不受影响。
if command -v codesign >/dev/null; then
    codesign --force --deep --sign - "$DEST" 2>/dev/null \
        && echo "  已 ad-hoc 重签" \
        || echo "  重签失败（可能仍可运行；若有问题用 xattr -dr com.apple.quarantine）" >&2
fi

mkdir -p "$PROFILE"/{config,cache,data,downloads}

cat > "$ROOT/build-macos/test-client/run.sh" <<RUN
#!/bin/bash
# 用隔离环境启动测试版客户端。
#   - 配置在 $PROFILE/config
#   - 缓存（消息索引、媒体）在 $PROFILE/cache
#   - 下载默认目录在 $PROFILE/downloads
# 正式版的配置与缓存完全不受影响。
set -euo pipefail
# macOS 上 Qt 的 QStandardPaths 只认 CFFIXED_USER_HOME；只设 HOME 仍会读写真实用户目录。
export CFFIXED_USER_HOME="$PROFILE/home"
export WECHAT_LITE_PROFILE="$PROFILE/config"
export HOME="$PROFILE/home"
export XDG_CACHE_HOME="$PROFILE/cache"
export XDG_DATA_HOME="$PROFILE/data"
mkdir -p "\$HOME" "\$XDG_CACHE_HOME" "\$XDG_DATA_HOME"
# 没有参数时直接进本机演示模式（800 条演示历史），用来试滚动最省事；
# 需要连自己的服务器就传 --demo 以外的参数并手工填连接信息。
if [[ \$# -eq 0 ]]; then
    exec "$DEST/Contents/MacOS/wechat-lite" --demo
fi
exec "$DEST/Contents/MacOS/wechat-lite" "\$@"
RUN
chmod +x "$ROOT/build-macos/test-client/run.sh"

echo
echo "== 完成 =="
echo "  测试版 app : $DEST"
echo "  启动脚本   : $ROOT/build-macos/test-client/run.sh"
echo "  隔离数据   : $PROFILE"
echo
echo "启动测试版（推荐用脚本，环境隔离完整）："
echo "  $ROOT/build-macos/test-client/run.sh"
echo
echo "直接用 open 启动（只隔离配置，缓存仍与正式版共用）："
echo "  open \"$DEST\""
echo
echo "清理测试版："
echo "  rm -rf \"$DEST\" \"$PROFILE\" \"$ROOT/build-macos/test-client/run.sh\""
echo
echo "正式版（$HOME/Applications/微信轻聊.app）全程未被修改。"
