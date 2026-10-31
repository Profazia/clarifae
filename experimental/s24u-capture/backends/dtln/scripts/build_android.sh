#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
NDK="${ANDROID_NDK_HOME:?Set ANDROID_NDK_HOME to Android NDK r27b}"
python3 "$ROOT/scripts/fetch_assets.py" --verify-only
cmake -S "$ROOT" -B "$ROOT/build/android-arm64" -G Ninja \
 -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
 -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29 -DCMAKE_BUILD_TYPE=Release "$@"
cmake --build "$ROOT/build/android-arm64" --parallel 4
"$NDK/toolchains/llvm/prebuilt/darwin-x86_64/bin/llvm-readelf" -dW "$ROOT/build/android-arm64/dtln_probe"
