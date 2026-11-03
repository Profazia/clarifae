#!/bin/sh
set -eu
D=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
. "$D/scripts/env.sh"
"$D/scripts/fetch.sh"
NDK="${ANDROID_NDK_HOME:?Set ANDROID_NDK_HOME to Android NDK r27b}"
grep -q "Pkg.Revision = $NDK_VERSION" "$NDK/source.properties" || { echo "NDK revision must be $NDK_VERSION" >&2; exit 1; }
LLVM=$(echo "$NDK"/toolchains/llvm/prebuilt/*)
CC="$LLVM/bin/aarch64-linux-android${ANDROID_API}-clang"
export CARGO_TARGET_AARCH64_LINUX_ANDROID_LINKER="$CC"
export CC_aarch64_linux_android="$CC" AR_aarch64_linux_android="$LLVM/bin/llvm-ar"
export CFLAGS_aarch64_linux_android="-O2 -fPIC"
export RUSTFLAGS="-C link-arg=-Wl,-soname,libdf.so -C link-arg=-Wl,-z,max-page-size=16384"
OUT="$D/out/android"
mkdir -p "$OUT" "$D/artifacts"
cargo +"$RUST_VERSION" build --locked --release -j4 --target aarch64-linux-android --manifest-path "$D/runtime/Cargo.toml"
cp "$D/runtime/target/aarch64-linux-android/release/libdf.so" "$OUT/"
"$CC" -std=c11 -O2 -Wall -Wextra -Werror -I"$D/include" "$D/tests/runtime_test.c" -L"$OUT" -ldf -Wl,-rpath,'$ORIGIN' -Wl,-z,max-page-size=16384 -pthread -lm -o "$OUT/df_runtime_test"
"$CC" -std=c11 -O2 -Wall -Wextra -Werror -DCLARIFAE_WITH_DF '-DDF_MODEL_PATH="./DeepFilterNet3_onnx.tar.gz"' -I"$D/include" -I"$D/../../engine" "$D/../../engine/backend_deepfilternet.c" "$D/tests/backend_test.c" "$D/tests/test_log.c" -L"$OUT" -ldf -llog -Wl,-rpath,'$ORIGIN' -Wl,-z,max-page-size=16384 -lm -o "$OUT/df_backend_test"
"$CC" -std=c11 -O2 -Wall -Wextra -Werror -DCLARIFAE_WITH_DF -I"$D/../../engine" -c "$D/../../engine/backend_deepfilternet.c" -o "$OUT/backend_deepfilternet-production.o"
"$CC" -std=c11 -Wall -Wextra -Werror -c "$D/../../engine/backend_deepfilternet.c" -o "$OUT/disabled-guard.o"
cp "$D/models/DeepFilterNet3_onnx.tar.gz" "$OUT/"
"$LLVM/bin/llvm-readelf" -h -l -d -s "$OUT/libdf.so" > "$D/artifacts/android-elf.txt"
"$LLVM/bin/llvm-readelf" -h -l -d "$OUT/df_runtime_test" > "$D/artifacts/android-test-elf.txt"
shasum -a 256 "$OUT/libdf.so" "$OUT/df_runtime_test" "$OUT/df_backend_test" "$OUT/DeepFilterNet3_onnx.tar.gz" > "$D/artifacts/android-artifact-sha256.txt"
"$D/scripts/check-android.sh"
echo "Android API $ANDROID_API runtime and isolated tests: $OUT (NOT executed here)"
