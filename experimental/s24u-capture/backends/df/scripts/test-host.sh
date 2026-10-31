#!/bin/sh
set -eu
D=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
mkdir -p "$D/artifacts"
. "$D/scripts/env.sh"
# Exactly the correct model size but invalid contents: pin rejection before Rust parser.
dd if=/dev/zero of="$D/artifacts/invalid-model.tar.gz" bs=7983136 count=1 2>/dev/null
"$D/out/host/df_runtime_test" "$D/models/DeepFilterNet3_onnx.tar.gz" "$D/artifacts/invalid-model.tar.gz" > "$D/artifacts/host-runtime-test.log" 2>&1
cat "$D/artifacts/host-runtime-test.log"
"$D/out/host/df_backend_test" > "$D/artifacts/host-backend-test.log" 2>&1
cat "$D/artifacts/host-backend-test.log"
cargo +"$RUST_VERSION" test --locked --release -j4 --manifest-path "$D/runtime/Cargo.toml" --features allocation-metrics > "$D/artifacts/host-rust-unit-test.log" 2>&1
cat "$D/artifacts/host-rust-unit-test.log"
