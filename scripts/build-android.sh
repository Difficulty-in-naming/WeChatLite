#!/usr/bin/env bash
# Build the Android client. The toolchain is taken from the Unity editors installed on
# this machine: Unity 6000.0.74f1 ships the JDK/SDK/NDK, Unity 6000.7.0b5 ships Gradle.
# Override any path through the environment when using another machine.
set -euo pipefail
cd "$(dirname "$0")/.."

UNITY_EDITOR_ROOT="${UNITY_EDITOR_ROOT:-/Volumes/MacData/Applications/UnityEditor}"
SDK_EDITOR="${SDK_EDITOR:-$UNITY_EDITOR_ROOT/6000.0.74f1}"
GRADLE_EDITOR="${GRADLE_EDITOR:-$UNITY_EDITOR_ROOT/6000.7.0b5}"

ANDROID_SDK="${ANDROID_SDK:-$SDK_EDITOR/PlaybackEngines/AndroidPlayer/SDK}"
GRADLE_HOME="${GRADLE_HOME:-$GRADLE_EDITOR/PlaybackEngines/AndroidPlayer/Tools/gradle}"
GRADLE_BIN="${GRADLE_BIN:-$GRADLE_HOME/bin/gradle}"

# The JDK bundled with Unity 6000.0.74f1 is x86_64; Gradle's native platform library then has to
# run under Rosetta. Prefer an arm64 JDK when one is installed so Gradle starts natively.
if [[ -z "${JAVA_HOME:-}" ]]; then
  for candidate in \
    /opt/homebrew/opt/openjdk@21/libexec/openjdk.jdk/Contents/Home \
    /opt/homebrew/opt/openjdk/libexec/openjdk.jdk/Contents/Home \
    "$SDK_EDITOR/PlaybackEngines/AndroidPlayer/OpenJDK"; do
    if [[ -x "$candidate/bin/java" ]]; then
      JAVA_HOME="$candidate"
      break
    fi
  done
fi
if [[ -z "${JAVA_HOME:-}" ]]; then
  echo 'No JDK found. Set JAVA_HOME.' >&2
  exit 1
fi

# Gradle caches a native library per architecture and this machine's default cache only holds the
# x86_64 build, so keep an independent cache unless the caller already points one somewhere writable.
export GRADLE_USER_HOME="${GRADLE_USER_HOME:-$PWD/android/.gradle-user-home}"
# AGP writes the debug keystore and its analytics under the Android user home.
export ANDROID_USER_HOME="${ANDROID_USER_HOME:-$PWD/android/.android-user-home}"
mkdir -p "$GRADLE_USER_HOME" "$ANDROID_USER_HOME"

export JAVA_HOME
export ANDROID_HOME="$ANDROID_SDK"
export ANDROID_SDK_ROOT="$ANDROID_SDK"

printf 'sdk.dir=%s\n' "$ANDROID_SDK" > android/local.properties

# --- P2P tunnel helper -------------------------------------------------------
# The Android client loads the tunnel in-process through JNI. A bundled ELF cannot be executed on
# Android: `untrusted_app` may not run files under `app_data_file`, and a PIE executable placed
# under `lib/` is treated as a shared library and aborts with
# "android context was not initialized". So the crate is built as a cdylib and loaded with
# System.loadLibrary; see network/src/jni.rs.
ANDROID_ABI="${ANDROID_ABI:-arm64-v8a}"
NDK_ROOT="${NDK_ROOT:-$SDK_EDITOR/PlaybackEngines/AndroidPlayer/NDK}"
NDK_HOST_TAG="${NDK_HOST_TAG:-darwin-x86_64}"
NDK_TOOLCHAIN="$NDK_ROOT/toolchains/llvm/prebuilt/$NDK_HOST_TAG/bin"

if [[ "$ANDROID_ABI" != "arm64-v8a" ]]; then
  echo "Only arm64-v8a is packaged today (got $ANDROID_ABI)." >&2
  exit 1
fi

RUST_TARGET="aarch64-linux-android"
RUST_API_LEVEL="${RUST_API_LEVEL:-26}"

# Prefer the project-local toolchain (Rust 1.95). The Homebrew rustc on this machine is 1.88 and
# cannot build iroh, so falling back to `cargo` on PATH would fail with a version error.
if [[ -x "$PWD/.tools/cargo/bin/cargo" ]]; then
  export RUSTUP_HOME="${RUSTUP_HOME:-$PWD/.tools/rustup}"
  export CARGO_HOME="${CARGO_HOME:-$PWD/.tools/cargo}"
  CARGO="${CARGO:-$PWD/.tools/cargo/bin/cargo}"
else
  CARGO="${CARGO:-cargo}"
fi
if ! "$CARGO" --version >/dev/null 2>&1; then
  echo 'cargo was not found or is not runnable; install Rust 1.95 or add it to PATH.' >&2
  exit 1
fi

helper_source="$PWD/target/$RUST_TARGET/release/libwechat_p2p.so"
helper_destination="$PWD/android/app/src/main/jniLibs/$ANDROID_ABI/libwechat_p2p.so"

# Rebuild when the helper sources are newer than the packaged library, so a source change never
# ships a stale tunnel. Set SKIP_HELPER=1 to reuse the existing library.
if [[ "${SKIP_HELPER:-0}" != "1" ]]; then
  echo "Building $RUST_TARGET libwechat_p2p with the NDK toolchain…"
  export CARGO_TARGET_AARCH64_LINUX_ANDROID_LINKER="$NDK_TOOLCHAIN/aarch64-linux-android$RUST_API_LEVEL-clang"
  export CC_aarch64_linux_android="$CARGO_TARGET_AARCH64_LINUX_ANDROID_LINKER"
  export AR_aarch64_linux_android="$NDK_TOOLCHAIN/llvm-ar"
  "$CARGO" build --release --locked -p wechat-p2p --target "$RUST_TARGET"
fi

if [[ ! -f "$helper_source" ]]; then
  echo "Missing $helper_source. Run the helper build first." >&2
  exit 1
fi

mkdir -p "$(dirname "$helper_destination")"
cp "$helper_source" "$helper_destination"
echo "Packaged tunnel library: $(basename "$helper_destination") ($(du -h "$helper_destination" | cut -f1))"

task="${1:-assembleDebug}"
shift || true

# The Gradle build lives in android/ while the helper build runs from the repository root.
GRADLE_BUILD="$PWD/android"

# Some Unity installs ship only Gradle's libs, without the `bin/gradle` launcher script. Running the
# launcher jar through the JDK behaves the same and keeps the script usable on those machines.
if [[ -x "$GRADLE_BIN" ]]; then
  exec "$GRADLE_BIN" --project-dir "$GRADLE_BUILD" ":app:$task" --no-daemon --console=plain "$@"
fi
launcher_jar=$(ls "$GRADLE_HOME"/lib/gradle-launcher-*.jar 2>/dev/null | sort -V | tail -1 || true)
if [[ -z "$launcher_jar" ]]; then
  echo "Gradle launcher not found under $GRADLE_HOME." >&2
  exit 1
fi
exec "$JAVA_HOME/bin/java" -cp "$launcher_jar" org.gradle.launcher.GradleMain \
  --project-dir "$GRADLE_BUILD" ":app:$task" --no-daemon --console=plain "$@"
