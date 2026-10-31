#!/bin/sh
set -eu
D=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
NDK="${ANDROID_NDK_HOME:?Set ANDROID_NDK_HOME to Android NDK r27b}"
LLVM="$NDK/toolchains/llvm/prebuilt/darwin-x86_64/bin"
OUT="$D/out/android"
for file in libdf.so df_runtime_test df_backend_test; do
    "$LLVM/llvm-readelf" -h "$OUT/$file" | grep -q AArch64
    "$LLVM/llvm-readelf" -l "$OUT/$file" | awk '/LOAD/{ if($NF!="0x4000")exit 1; found=1 }END{if(!found)exit 1}'
done
"$LLVM/llvm-nm" -D --defined-only "$OUT/libdf.so" > "$D/artifacts/android-exported-symbols.txt"
for sym in create process reset free frame_length sample_rate delay_samples backend_name processed_frames set_atten_lim; do
    grep -q " clarifae_df_$sym$" "$D/artifacts/android-exported-symbols.txt"
done
if grep -q ' df_create$' "$D/artifacts/android-exported-symbols.txt"; then echo "unsafe upstream capi accidentally enabled" >&2; exit 1; fi
needed=$("$LLVM/llvm-readelf" -d "$OUT/libdf.so" | sed -n 's/.*Shared library: \[\(.*\)\]/\1/p' | LC_ALL=C sort | tr '\n' ' ')
[ "$needed" = "libc.so libdl.so libm.so " ] || { echo "Unexpected runtime deps: $needed" >&2; exit 1; }
"$LLVM/llvm-readelf" -d "$OUT/libdf.so" | grep 'SONAME' | grep -q '\[libdf.so\]'
"$LLVM/llvm-strings" "$OUT/backend_deepfilternet-production.o" | grep -q '^/vendor/lib64/soundfx/clarifae_models/df/DeepFilterNet3_onnx.tar.gz$'
(cd "$D" && shasum -a 256 -c artifacts/android-artifact-sha256.txt)
echo "Android ARM64/16KiB/checked ABI/dependencies/vendor-path checks PASS (execution not claimed)"
