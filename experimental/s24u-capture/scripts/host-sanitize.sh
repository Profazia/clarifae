#!/usr/bin/env bash
set -euo pipefail
HERE=$(cd "$(dirname "$0")/.." && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
cd "$ROOT"
OUT="$HERE/host-build"
mkdir -p "$OUT"
CLANG=$(xcrun -f clang);SDK=$(xcrun --show-sdk-path)
RNN="${RNNOISE_DIR:-$ROOT/native/third_party/rnnoise}"
SOURCES=()
for file in "$RNN"/src/*.c;do case "$file" in *dump_features*|*dump_rnnoise_tables*|*write_weights*)continue;;esac;SOURCES+=("$file");done
FLAGS=(-isysroot "$SDK" -std=gnu11 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer)
INC=(-I"$HERE/include" -I"$HERE/engine/include")
"$CLANG" "${FLAGS[@]}" -dynamiclib -DRNNOISE_BUILD -I"$HERE/tests/host-stubs" -I"$HERE/engine" -I"$HERE/engine/include" -I"$HERE/include" -I"$RNN/include" -I"$RNN/src" \
 "$HERE/engine/effect.c" "$HERE/engine/config.c" "$HERE/engine/ringbuf.c" "$HERE/engine/resampler.c" "$HERE/engine/backend_rnnoise.c" \
 "$HERE/tests/host-stubs/backend_optional.c" "${SOURCES[@]}" -lm -o "$OUT/libclarifae.dylib"
"$CLANG" "${FLAGS[@]}" "${INC[@]}" -I"$HERE/tests/host-stubs" -dynamiclib -DCLF_BRIDGE_TESTING "$HERE/src/capture_bridge.c" -lm -o "$OUT/libbridge.dylib"
"$CLANG" "${FLAGS[@]}" "${INC[@]}" -dynamiclib "$HERE/tests/fake_vendor.c" -o "$OUT/libfake.dylib"
"$CLANG" "${FLAGS[@]}" "${INC[@]}" -dynamiclib "$HERE/tests/engine_fault.c" -o "$OUT/libfault.dylib"
"$CLANG" "${FLAGS[@]}" "${INC[@]}" "$HERE/tests/bridge_tests.c" -lm -o "$OUT/bridge-tests"
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 "$OUT/bridge-tests" "$OUT/libbridge.dylib" "$OUT/libfake.dylib" "$OUT/libclarifae.dylib" > "$HERE/evidence/host-asan-normal.txt" 2>&1
for stage in process init config enable stats nan range;do
 CLF_FAULT_STAGE="$stage" ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 "$OUT/bridge-tests" "$OUT/libbridge.dylib" "$OUT/libfake.dylib" "$OUT/libfault.dylib" "$stage" > "$HERE/evidence/host-asan-$stage.txt" 2>&1
done
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 "$OUT/bridge-tests" "$OUT/libbridge.dylib" "$OUT/libfake.dylib" /no-such-clarifae-engine.so missing > "$HERE/evidence/host-asan-missing.txt" 2>&1
tail -2 "$HERE/evidence/host-asan-normal.txt"
for stage in process init config enable stats nan range missing;do tail -1 "$HERE/evidence/host-asan-$stage.txt";done
