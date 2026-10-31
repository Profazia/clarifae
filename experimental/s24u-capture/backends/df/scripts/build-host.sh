#!/bin/sh
set -eu
D=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
. "$D/scripts/env.sh"
"$D/scripts/fetch.sh"
OUT="$D/out/host"
mkdir -p "$OUT" "$D/artifacts"
cargo +"$RUST_VERSION" build --locked --release -j4 --features allocation-metrics --manifest-path "$D/runtime/Cargo.toml"
cp "$D/runtime/target/release/libdf.dylib" "$OUT/"
CC=${CC:-clang}
"$CC" -std=c11 -O2 -Wall -Wextra -Werror -I"$D/include" "$D/tests/runtime_test.c" -L"$OUT" -ldf -Wl,-rpath,"$OUT" -pthread -o "$OUT/df_runtime_test"
"$CC" -std=c11 -O2 -Wall -Wextra -Werror -DCLARIFAE_WITH_DF "-DDF_MODEL_PATH=\"$D/models/DeepFilterNet3_onnx.tar.gz\"" -I"$D/tests/stubs" -I"$D/include" -I"$D/../../engine" "$D/../../engine/backend_deepfilternet.c" "$D/tests/backend_test.c" "$D/tests/test_log.c" -L"$OUT" -ldf -Wl,-rpath,"$OUT" -lm -o "$OUT/df_backend_test"
"$CC" -std=c11 -Wall -Wextra -Werror -c "$D/../../engine/backend_deepfilternet.c" -o "$OUT/disabled-guard.o"
cargo +"$RUST_VERSION" tree --locked --manifest-path "$D/runtime/Cargo.toml" > "$D/artifacts/dependency-tree.txt"
shasum -a 256 "$OUT/libdf.dylib" "$OUT/df_runtime_test" "$OUT/df_backend_test" > "$D/artifacts/host-artifact-sha256.txt"
echo "Host runtime and tests: $OUT"
