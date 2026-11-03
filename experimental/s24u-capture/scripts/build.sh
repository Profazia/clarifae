#!/usr/bin/env bash
set -euo pipefail
HERE=$(cd "$(dirname "$0")/.." && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
NDK="${ANDROID_NDK_HOME:?Set ANDROID_NDK_HOME to Android NDK r27b}"
TOOLBIN=$(echo "$NDK"/toolchains/llvm/prebuilt/*/bin)
mkdir -p "$HERE/build" "$HERE/dist" "$HERE/evidence"
if [ ! -f "${RNNOISE_DIR:-$ROOT/native/third_party/rnnoise}/src/rnnoise_data.c" ]; then
 echo "Set RNNOISE_DIR to the verified RNNoise 0.2 source tree with bundled weights" >&2; exit 2
fi
cmake -S "$HERE/engine" -B "$HERE/build-engine" -G Ninja \
 -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
 -DCMAKE_C_FLAGS="-Werror -mbranch-protection=standard" \
 -DCLARIFAE_ISOLATED_TEST_CONFIG=OFF -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-26 -DCMAKE_BUILD_TYPE=Release \
 -DRNNOISE_DIR="${RNNOISE_DIR:-$ROOT/native/third_party/rnnoise}"
cmake --build "$HERE/build-engine" -j6
"$TOOLBIN/llvm-strip" --strip-unneeded -o "$HERE/dist/libclarifae.so" "$HERE/build-engine/libclarifae.so"
# Test-only property stubs make isolated benchmarks independent of live tuning.
cmake -S "$HERE/engine" -B "$HERE/build-isolated-engine" -G Ninja \
 -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
 -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-26 -DCMAKE_BUILD_TYPE=Release \
 -DRNNOISE_DIR="${RNNOISE_DIR:-$ROOT/native/third_party/rnnoise}" -DCMAKE_C_FLAGS="-Werror -mbranch-protection=standard" -DCLARIFAE_ISOLATED_TEST_CONFIG=ON
cmake --build "$HERE/build-isolated-engine" -j4
"$TOOLBIN/llvm-strip" --strip-unneeded -o "$HERE/dist/libclarifae_isolated.so" "$HERE/build-isolated-engine/libclarifae.so"

INCLUDES=(-I"$HERE/include" -I"$HERE/engine/include")
FLAGS=(-std=gnu11 -O2 -g -mbranch-protection=standard -fPIC -fvisibility=hidden -Wall -Wextra -Werror -Wno-unused-parameter -Wl,-z,defs -Wl,-z,now -Wl,-z,nodelete -Wl,-z,max-page-size=16384)
"$TOOLBIN/aarch64-linux-android26-clang" "${FLAGS[@]}" "${INCLUDES[@]}" -shared "$HERE/src/capture_bridge.c" \
 -Wl,-soname,libclarifae_capture_bridge.so -llog -ldl -lm -o "$HERE/dist/libclarifae_capture_bridge.so"
"$TOOLBIN/aarch64-linux-android26-clang" "${FLAGS[@]}" "${INCLUDES[@]}" -DCLF_BRIDGE_TESTING -shared "$HERE/src/capture_bridge.c" \
 -Wl,-soname,libclarifae_capture_bridge_test.so -llog -ldl -lm -o "$HERE/dist/libclarifae_capture_bridge_test.so"
"$TOOLBIN/aarch64-linux-android26-clang" -std=gnu11 -O2 -Wall -Wextra -Werror -fPIC -shared "${INCLUDES[@]}" \
 "$HERE/tests/fake_vendor.c" -Wl,-soname,libfake_audio_primary.so -Wl,-z,max-page-size=16384 -o "$HERE/dist/libfake_audio_primary.so"
"$TOOLBIN/aarch64-linux-android26-clang" -std=gnu11 -O2 -Wall -Wextra -Werror "${INCLUDES[@]}" \
 "$HERE/tests/bridge_tests.c" -ldl -lm -Wl,-z,max-page-size=16384 -o "$HERE/dist/bridge-tests"
"$TOOLBIN/aarch64-linux-android26-clang" -std=gnu11 -O2 -Wall -Wextra -Werror -Wno-unused-parameter -fPIC -shared "${INCLUDES[@]}" \
 "$HERE/tests/engine_fault.c" -Wl,-soname,libengine_fault.so -Wl,-z,max-page-size=16384 -o "$HERE/dist/libengine_fault.so"
"$TOOLBIN/aarch64-linux-android26-clang" -std=gnu11 -O2 -Wall -Wextra -Werror -I"$HERE/tests/property-stubs" -I"$HERE/engine" -I"$HERE/engine/include" "$HERE/tests/property_change.c" "$HERE/engine/config.c" -llog -Wl,-z,max-page-size=16384 -o "$HERE/dist/property-tests"
"$TOOLBIN/llvm-readelf" -h -l -d -Ws "$HERE/dist/libclarifae_capture_bridge.so" > "$HERE/evidence/bridge-elf.txt"
(cd "$HERE/dist";shasum -a256 *.so > SHA256SUMS)
echo 'Android bridge, engine and isolated stock fixture built; nothing deployed.'
