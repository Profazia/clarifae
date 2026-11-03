#!/usr/bin/env bash
# macOS host-only synthetic regression gates. No downloads or device access.
set -euo pipefail
HERE=$(cd "$(dirname "$0")/.." && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
OUT="${BUILD_DIR:-$HERE/host-build/engine}"
CC="${CC:-$(xcrun -f clang)}"
SANITIZER="${SANITIZER:-address,undefined}"
mkdir -p "$OUT"
FLAGS=(-isysroot "$(xcrun --show-sdk-path)" -std=gnu11 -O1 -g
 -Wall -Wextra -Werror -Wno-unused-parameter -fno-omit-frame-pointer
 "-fsanitize=$SANITIZER" -fno-sanitize-recover=all)
INC=(-I"$HERE/tests/host-stubs" -I"$HERE/engine" -I"$HERE/engine/include" -I"$HERE/include")
export ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 TSAN_OPTIONS=halt_on_error=1
for suite in async_test output_matrix lifecycle_off;do
 "$CC" "${FLAGS[@]}" -DCLARIFAE_ASYNC_TEST "${INC[@]}" "$HERE/tests/$suite.c" -lpthread -lm -o "$OUT/$suite"
 "$OUT/$suite"
done
# These tests compile the real property reader against an explicit host store.
PINC=(-I"$HERE/tests/property-stubs" "${INC[@]}")
"$CC" "${FLAGS[@]}" "${PINC[@]}" "$HERE/tests/property_change.c" "$HERE/engine/config.c" -o "$OUT/property-tests"
"$OUT/property-tests"
"$CC" "${FLAGS[@]}" "${PINC[@]}" "$HERE/tests/config_interleave.c" "$HERE/engine/config.c" "$HERE/engine/effect.c" "$HERE/tests/host-stubs/disabled_backend.c" -lpthread -lm -o "$OUT/config-interleave-tests"
"$OUT/config-interleave-tests"
if [ "$SANITIZER" = thread ];then exit 0;fi
RNN="${RNNOISE_DIR:-$ROOT/native/third_party/rnnoise}"
SOURCES=()
for file in "$RNN"/src/*.c;do
 case "$file" in *dump_features*|*dump_rnnoise_tables*|*write_weights*)continue;;esac
 SOURCES+=("$file")
done
# Real RNNoise, synthetic PCM, stubbed controls; NOT real optional-plugin proof.
"$CC" "${FLAGS[@]}" -Wno-error "${INC[@]}" -DRNNOISE_BUILD -DCLARIFAE_ASYNC_TEST \
 -I"$RNN/src" -I"$RNN/include" "$HERE/tests/real_smoke.c" \
 "$HERE/tests/fixed_config.c" "$HERE/tests/host-stubs/real_optional.c" \
 "$HERE/engine/backend_rnnoise.c" "$HERE/engine/resampler.c" \
 "$HERE/engine/ringbuf.c" "${SOURCES[@]}" -lpthread -lm -o "$OUT/real-smoke"
"$OUT/real-smoke"
