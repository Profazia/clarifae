#!/usr/bin/env bash
set -euo pipefail
HERE=$(cd "$(dirname "$0")/.." && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
NDK="${ANDROID_NDK_HOME:?Set ANDROID_NDK_HOME to Android NDK r27b}"
BIN=$(echo "$NDK"/toolchains/llvm/prebuilt/*/bin)
CC="$BIN/aarch64-linux-android26-clang"
INC=(-I"$HERE/include" -I"$HERE/engine/include" -I"$HERE/engine")
FLAGS=(-std=gnu11 -O2 -Wall -Wextra -Werror -Wno-unused-parameter -Wl,-z,max-page-size=16384)
"$CC" "${FLAGS[@]}" "${INC[@]}" -shared -fPIC "$HERE/tests/mode_fixture.c" -o "$HERE/dist/libmode_fixture.so"
"$CC" "${FLAGS[@]}" "${INC[@]}" "$HERE/tests/mode_race_test.c" -ldl -o "$HERE/dist/mode-race-test"
OBJ="$HERE/build-engine/CMakeFiles/clarifae.dir"
OBJECTS=("$OBJ/effect.c.o" "$OBJ/backend_rnnoise.c.o" "$OBJ/ringbuf.c.o" "$OBJ/resampler.c.o")
while IFS= read -r file;do OBJECTS+=("$file");done < <(find "$OBJ" -type f -path '*/src/*.c.o' -name '*.c.o')
"$CC" "${FLAGS[@]}" "${INC[@]}" -I"$HERE/tests/property-stubs" "$HERE/tests/config_interleave.c" "$HERE/engine/config.c" "${OBJECTS[@]}" -llog -ldl -lm -o "$HERE/dist/config-interleave-test"
