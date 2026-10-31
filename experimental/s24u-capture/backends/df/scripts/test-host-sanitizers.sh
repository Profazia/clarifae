#!/bin/sh
set -eu
D=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
OUT="$D/out/host"
clang -std=c11 -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined -Wall -Wextra -Werror -DCLARIFAE_WITH_DF "-DDF_MODEL_PATH=\"$D/models/DeepFilterNet3_onnx.tar.gz\"" -I"$D/tests/stubs" -I"$D/include" -I"$D/../../engine" "$D/../../engine/backend_deepfilternet.c" "$D/tests/backend_test.c" "$D/tests/test_log.c" -L"$OUT" -ldf -Wl,-rpath,"$OUT" -lm -o "$OUT/df_backend_test_sanitized"
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 "$OUT/df_backend_test_sanitized" > "$D/artifacts/host-c-asan-ubsan.log" 2>&1
cat "$D/artifacts/host-c-asan-ubsan.log"
echo "C experimental/s24u-capture/test ASan+UBSan PASS. Rust dependencies NOT sanitizer-instrumented. Leak sanitizer disabled."
