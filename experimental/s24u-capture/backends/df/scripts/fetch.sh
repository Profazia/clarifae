#!/bin/sh
set -eu
D=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
. "$D/pins.env"
mkdir -p "$D/downloads" "$D/models" "$D/artifacts"
fetch() {
    dest=$1; url=$2; expected=$3
    if [ ! -f "$dest" ]; then
        curl --fail --location --retry 2 --max-time 300 "$url" -o "$dest.partial"
        actual=$(shasum -a 256 "$dest.partial" | awk '{print $1}')
        [ "$actual" = "$expected" ] || { echo "download hash mismatch: $dest" >&2; exit 1; }
        mv "$dest.partial" "$dest"
    fi
    actual=$(shasum -a 256 "$dest" | awk '{print $1}')
    [ "$actual" = "$expected" ] || { echo "existing hash mismatch: $dest (preserved)" >&2; exit 1; }
}
fetch "$D/downloads/DeepFilterNet-d375b2d.tar.gz" "https://codeload.github.com/Rikorose/DeepFilterNet/tar.gz/$DF_REV" "$DF_SOURCE_SHA256"
if [ ! -f "$D/upstream/libDF/src/tract.rs" ]; then
    mkdir -p "$D/upstream"
    tar -xzf "$D/downloads/DeepFilterNet-d375b2d.tar.gz" -C "$D/upstream" --strip-components=1
fi
(cd "$D" && shasum -a 256 -c UPSTREAM-SHA256SUMS > artifacts/source-verification.log)
if [ ! -f "$D/models/DeepFilterNet3_onnx.tar.gz" ]; then cp "$D/upstream/models/DeepFilterNet3_onnx.tar.gz" "$D/models/"; fi
[ "$(shasum -a 256 "$D/models/DeepFilterNet3_onnx.tar.gz" | awk '{print $1}')" = "$DF_MODEL_SHA256" ]
echo "source=$DF_REV model_sha256=$DF_MODEL_SHA256 verified"
