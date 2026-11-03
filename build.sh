#!/usr/bin/env bash
# build.sh — cross-compile libclarifae.so for each Android ABI with the NDK and
# stage it into lib/<abi>/ and system/lib(64)/soundfx/ ready for packaging.
#
#   ABIS="arm64-v8a armeabi-v7a x86_64" ./build.sh
#   CLARIFAE_WITH_DTLN=ON TFLITE_DIR=/path/to/tflite ./build.sh   # optional
#   CLARIFAE_WITH_DF=ON   DF_DIR=/path/to/libdf       ./build.sh   # optional
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
NATIVE="$HERE/native"

# --- locate NDK ---
NDK="${ANDROID_NDK_HOME:-${ANDROID_NDK_ROOT:-}}"
if [ -z "$NDK" ]; then
  for base in "$HOME/Android/Sdk/ndk" "$HOME/Library/Android/sdk/ndk" "/opt/android-sdk/ndk"; do
    if [ -d "$base" ]; then
      NDK="$base/$(ls "$base" | sort -V | tail -n1)"
      break
    fi
  done
fi
[ -n "$NDK" ] && [ -d "$NDK" ] || { echo "ERROR: Android NDK not found. Set ANDROID_NDK_HOME."; exit 1; }
echo ">> NDK: $NDK"

TOOLCHAIN="$NDK/build/cmake/android.toolchain.cmake"
[ -f "$TOOLCHAIN" ] || { echo "ERROR: toolchain not found: $TOOLCHAIN"; exit 1; }

# --- ensure RNNoise is present; an explicit external tree is never fetched ---
RNN="${RNNOISE_DIR:-$NATIVE/third_party/rnnoise}"
if [ ! -f "$RNN/include/rnnoise.h" ]; then
  if [ -n "${RNNOISE_DIR:-}" ]; then
    echo "ERROR: RNNOISE_DIR must contain include/rnnoise.h." >&2
    exit 1
  fi
  echo ">> Fetching RNNoise..."
  "$HERE/scripts/fetch_deps.sh"
fi
RNN="$(cd "$RNN" && pwd)"

ABIS="${ABIS:-arm64-v8a armeabi-v7a x86_64}"
API="${ANDROID_API:-26}"
GEN="Unix Makefiles"; JOBS="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"
if command -v ninja >/dev/null 2>&1; then GEN="Ninja"; fi

EXTRA=()
[ "${CLARIFAE_WITH_DTLN:-OFF}" = "ON" ] && EXTRA+=("-DCLARIFAE_WITH_DTLN=ON" "-DTFLITE_DIR=${TFLITE_DIR:?set TFLITE_DIR}")
[ "${CLARIFAE_WITH_DF:-OFF}"   = "ON" ] && EXTRA+=("-DCLARIFAE_WITH_DF=ON"   "-DDF_DIR=${DF_DIR:?set DF_DIR}")

for ABI in $ABIS; do
  echo ">> Building $ABI ..."
  BUILD="$HERE/build/$ABI"
  rm -rf "$BUILD"; mkdir -p "$BUILD"
  cmake -S "$NATIVE" -B "$BUILD" -G "$GEN" \
        -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" \
        -DANDROID_ABI="$ABI" \
        -DANDROID_PLATFORM="android-$API" \
        -DCMAKE_BUILD_TYPE=Release \
        -DRNNOISE_DIR="$RNN" \
        "${EXTRA[@]}"
  cmake --build "$BUILD" -j "$JOBS"

  SO="$BUILD/libclarifae.so"
  [ -f "$SO" ] || { echo "ERROR: $SO not produced"; exit 1; }
  "$NDK"/toolchains/llvm/prebuilt/*/bin/llvm-strip --strip-unneeded "$SO" 2>/dev/null || true

  # Stage per-ABI only. customize.sh installs the one matching the device into
  # system/lib(64)/soundfx at flash time (both 64-bit ABIs share lib64, so we
  # must NOT pre-stage system/ here — they would collide).
  mkdir -p "$HERE/lib/$ABI"
  cp -f "$SO" "$HERE/lib/$ABI/libclarifae.so"
  echo "   -> lib/$ABI/libclarifae.so  ($(du -h "$SO" | cut -f1))"
done

# scrub any stale pre-staged libs (the module ships per-ABI in lib/, not system/)
rm -f "$HERE"/system/lib/soundfx/libclarifae.so "$HERE"/system/lib64/soundfx/libclarifae.so 2>/dev/null

echo ">> Done. Built ABIs: $ABIS"
