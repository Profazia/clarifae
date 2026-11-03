#!/system/bin/sh
# Explicitly run by parent only, after authorized staging. No mic/HA/device setup.
# Working directory: staging dir containing this script, binaries and model.
set -eu
export LD_LIBRARY_PATH="$PWD${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
dd if=/dev/zero of=invalid-model.tar.gz bs=7983136 count=1 2>/dev/null
./df_runtime_test ./DeepFilterNet3_onnx.tar.gz ./invalid-model.tar.gz
./df_backend_test
