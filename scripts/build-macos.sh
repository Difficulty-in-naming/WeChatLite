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

# Signing identity. Ad-hoc keeps the build working on any checkout and in CI, but a stable
# "Apple Development" identity is preferable locally: macOS keys each app's notification
# permission to its signing identity, so ad-hoc signing makes every rebuild look like a brand
# new app and the permission has to be granted again. Override with CODESIGN_IDENTITY, or set
# it to an empty string to force ad-hoc.
if [[ -z "${CODESIGN_IDENTITY+set}" ]]; then
  CODESIGN_IDENTITY='-'
  if command -v security >/dev/null 2>&1; then
    if detected="$(security find-identity -v -p codesigning 2>/dev/null |
      sed -n 's/.*"\(Apple Development: .*\)"/\1/p' | head -1)" && [[ -n "$detected" ]]; then
      CODESIGN_IDENTITY="$detected"
    fi
  fi
fi
: "${CODESIGN_IDENTITY:=-}"
if [[ "$CODESIGN_IDENTITY" == '-' ]]; then
  echo 'Signing ad-hoc. Set CODESIGN_IDENTITY to a Developer ID to keep macOS permissions stable.'
else
  echo "Signing with: $CODESIGN_IDENTITY"
fi
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
  # macdeployqt must sign with the same identity: it is the step that signs the Qt frameworks
  # nested in Contents/Frameworks, and codesign does not re-sign nested code when signing a bundle
  # without the deprecated --deep. Everything is then re-signed below in its final location.
  "$QT_PREFIX/bin/macdeployqt" "$app" \
    -executable="$app/Contents/MacOS/$helper" -always-overwrite -codesign="$CODESIGN_IDENTITY"
  # Sign after resources and both executables are in their final locations. The helper is signed
  # first because signing the bundle does not re-sign nested executables.
  codesign --force --sign "$CODESIGN_IDENTITY" "$app/Contents/MacOS/$helper"
  codesign --force --sign "$CODESIGN_IDENTITY" "$app"
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
echo "Signed with: $CODESIGN_IDENTITY. Developer ID signing/notarization require the owner certificate."
