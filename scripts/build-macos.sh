#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."

if [[ "$(uname -s)" != Darwin || "$(uname -m)" != arm64 ]]; then
  echo 'Build the Apple Silicon packages on an arm64 Mac.' >&2
  exit 1
fi
QT_PREFIX="${QT_PREFIX:-$(brew --prefix qt)}"
jobs="${BUILD_JOBS:-3}"
export CARGO_BUILD_JOBS="$jobs"
cargo test --locked -p wechat-relay -p wechat-p2p
cargo build --release --locked -p wechat-relay -p wechat-p2p
WECHAT_RELAY_BINARY="$PWD/target/release/wechat-relay" \
  cargo test --locked -p wechat-p2p --test p2p_e2e -- --ignored --test-threads=1
cmake -S . -B build-macos -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="$QT_PREFIX" -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=13.0
cmake --build build-macos --parallel "$jobs"
ctest --test-dir build-macos --output-on-failure --timeout 180

stamp="$(date +%Y%m%d-%H%M%S)-$$"
stage="$PWD/build-macos/package-staging/$stamp"
mkdir -p "$stage/client" "$stage/server" dist build-macos/package-history
for role in client server; do
  if [[ "$role" == client ]]; then
    name='微信轻聊'
    binary='wechat-lite'
    helper='wechat-link'
    origin='client'
    guide='CLIENT'
    asset='Client'
  else
    name='微信轻聊服务端'
    binary='wechat-server'
    helper='wechat-relay'
    origin='server_gui'
    guide='SERVER'
    asset='Server'
  fi
  app="$stage/$role/$name.app"
  ditto "build-macos/$origin/$binary.app" "$app"
  cp "target/release/$helper" "$app/Contents/MacOS/$helper"
  resources="$app/Contents/Resources"
  mkdir -p "$resources/licenses"
  cp "docs/$guide.md" "$resources/使用说明.md"
  cp THIRD_PARTY_NOTICES.md "$resources/THIRD_PARTY_NOTICES.md"
  cp network/licenses/*.txt "$resources/licenses/"
  "$QT_PREFIX/bin/macdeployqt" "$app" \
    -executable="$app/Contents/MacOS/$helper" -always-overwrite -codesign=-
  # Sign after resources and both executables are in their final locations.
  codesign --force --sign - "$app/Contents/MacOS/$helper"
  codesign --force --sign - "$app"
  codesign --verify --deep --strict --verbose=2 "$app"
  cmake "-DAPP=$app" "-DAPP_BINARY=$binary" "-DHELPER_BINARY=$helper" \
    -P scripts/verify-macos-bundle.cmake
  archive="$stage/WeChatLite-$asset-macOS-arm64.dmg"
  # The runner's disk image service can briefly return "Resource busy".
  for attempt in 1 2 3; do
    if hdiutil create -ov -volname "$name" -srcfolder "$stage/$role" -format UDZO "$archive"; then
      break
    fi
    if [[ "$attempt" -eq 3 ]]; then
      echo "Unable to create $asset DMG after three attempts." >&2
      exit 1
    fi
    sleep "$((attempt * 2))"
  done
  hdiutil verify "$archive"
  destination="$PWD/dist/$role-macos-arm64"
  if [[ -e "$destination" ]]; then
    if [[ -L "$destination" || -L "$PWD/dist" || -L "$PWD/build-macos/package-history" ]]; then
      echo 'Refusing to move a package through a symbolic link.' >&2
      exit 1
    fi
    mv "$destination" "$PWD/build-macos/package-history/$stamp-$role"
  fi
  mv "$stage/$role" "$destination"
  mv "$archive" "$PWD/dist/"
done
shasum -a 256 dist/WeChatLite-*-macOS-arm64.dmg > dist/MACOS_SHA256SUMS.txt
echo 'Output: two DMGs, each containing one self-contained .app.'
echo 'The Mac server has no native Mac WeChat adapter. It cannot forward local Mac WeChat yet.'
echo 'Ad-hoc signed for local testing. Developer ID signing/notarization require the owner certificate.'
