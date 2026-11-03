#!/usr/bin/env bash
set -euo pipefail
HERE=$(cd "$(dirname "$0")/.." && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
NDK="${ANDROID_NDK_HOME:?Set ANDROID_NDK_HOME to Android NDK r27b}"
BIN=$(echo "$NDK"/toolchains/llvm/prebuilt/*/bin)
CC="$BIN/aarch64-linux-android26-clang"
INC=(-I"$HERE/include" -I"$HERE/engine/include" -I"$HERE/engine")
FLAGS=(-std=gnu11 -O2 -Wall -Wextra -Werror -Wno-unused-parameter -Wl,-z,max-page-size=16384)
mkdir -p "$HERE/dist"
# Fake-backend Android executables; crossbuilding is NOT execution. The old
# wrapper-owned mode-worker fixture no longer models this engine architecture.
for suite in async_test output_matrix lifecycle_off;do
 "$CC" "${FLAGS[@]}" "${INC[@]}" -DCLARIFAE_ASYNC_TEST "$HERE/tests/$suite.c" -llog -lm -o "$HERE/dist/$suite"
done
"$CC" "${FLAGS[@]}" "${INC[@]}" -I"$HERE/tests/property-stubs" "$HERE/tests/config_interleave.c" "$HERE/engine/config.c" "$HERE/engine/effect.c" "$HERE/tests/host-stubs/disabled_backend.c" -llog -lm -o "$HERE/dist/config-interleave-test"
