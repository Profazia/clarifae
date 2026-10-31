#!/usr/bin/env bash
# Host-only synthetic PCM and command/property tests. No device access/downloads.
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${BUILD_DIR:-$HERE/build/host-tests}"
RNN="${RNNOISE_DIR:-$HERE/native/third_party/rnnoise}"
cmake -S "$HERE/tests" -B "$BUILD" \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo -DRNNOISE_DIR="$RNN" \
  -DCLARIFAE_SANITIZERS="${CLARIFAE_SANITIZERS:-ON}"
cmake --build "$BUILD" --parallel "${JOBS:-4}"
ctest --test-dir "$BUILD" --output-on-failure
