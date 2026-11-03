#!/usr/bin/env bash
set -euo pipefail
HERE=$(cd "$(dirname "$0")/.." && pwd)
NDK="${ANDROID_NDK_HOME:?Set ANDROID_NDK_HOME to Android NDK r27b}"
TOOLBIN=$(echo "$NDK"/toolchains/llvm/prebuilt/*/bin)
CC="$TOOLBIN/aarch64-linux-android29-clang"
FLAGS=(-std=gnu11 -O2 -fPIC -mbranch-protection=standard -Wall -Wextra -Werror -Wno-unused-parameter -Wl,-z,defs -Wl,-z,max-page-size=16384)
mkdir -p "$HERE/dist"
"$CC" "${FLAGS[@]}" -shared -DCLARIFAE_WITH_DTLN=1 -I"$HERE/engine" -I"$HERE/backends/dtln/runtime/include" -I"$HERE/backends/dtln/runtime/include/external/org_tensorflow" \
 "$HERE/engine/backend_dtln.c" "$HERE/engine/resampler.c" "$HERE/src/plugin_log.c" "$HERE/backends/dtln/runtime/android-arm64/libtensorflowlite_jni.so" \
 -Wl,-soname,libclarifae_dtln_backend.so -Wl,--version-script,"$HERE/src/plugin_dtln.exports" -Wl,-rpath,'$ORIGIN' -lm -llog -o "$HERE/dist/libclarifae_dtln_backend.so"
"$CC" "${FLAGS[@]}" -shared -DCLARIFAE_WITH_DF=1 -I"$HERE/engine" \
 "$HERE/engine/backend_deepfilternet.c" "$HERE/src/plugin_log.c" "$HERE/backends/df/out/android/libdf.so" \
 -Wl,-soname,libclarifae_df_backend.so -Wl,--version-script,"$HERE/src/plugin_df.exports" -Wl,-rpath,'$ORIGIN' -lm -llog -o "$HERE/dist/libclarifae_df_backend.so"
cp "$HERE/backends/dtln/runtime/android-arm64/libtensorflowlite_jni.so" "$HERE/dist/"
cp "$HERE/backends/df/out/android/libdf.so" "$HERE/dist/"
"$TOOLBIN/llvm-nm" -D --defined-only "$HERE/dist/libclarifae_dtln_backend.so" | grep -q clarifae_backend_dtln
"$TOOLBIN/llvm-nm" -D --defined-only "$HERE/dist/libclarifae_df_backend.so" | grep -q clarifae_backend_deepfilternet
(cd "$HERE/dist";shasum -a256 *.so > SHA256SUMS)
echo 'Optional Android backend plugins built with vendor-relative dependency RUNPATH.'
