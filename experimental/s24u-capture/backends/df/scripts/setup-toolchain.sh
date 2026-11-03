#!/bin/sh
set -eu
D=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
. "$D/pins.env"
[ "$(uname -s)-$(uname -m)" = Darwin-arm64 ] || { echo "This bootstrap pins Apple Silicon host." >&2; exit 1; }
export RUSTUP_HOME="$D/toolchain/rustup" CARGO_HOME="$D/toolchain/cargo"
mkdir -p "$D/downloads" "$D/artifacts"
if [ ! -f "$D/downloads/rustup-init" ]; then
    curl --fail --location --retry 2 --max-time 180 "https://static.rust-lang.org/rustup/archive/$RUSTUP_VERSION/aarch64-apple-darwin/rustup-init" -o "$D/downloads/rustup-init"
fi
[ "$(shasum -a 256 "$D/downloads/rustup-init" | awk '{print $1}')" = "$RUSTUP_SHA256" ]
if [ ! -f "$D/downloads/channel-rust-$RUST_VERSION.toml" ]; then
    curl --fail --location --retry 2 --max-time 90 "https://static.rust-lang.org/dist/channel-rust-$RUST_VERSION.toml" -o "$D/downloads/channel-rust-$RUST_VERSION.toml"
fi
[ "$(shasum -a 256 "$D/downloads/channel-rust-$RUST_VERSION.toml" | awk '{print $1}')" = "$RUST_MANIFEST_SHA256" ]
if [ ! -x "$CARGO_HOME/bin/rustup" ]; then
    chmod +x "$D/downloads/rustup-init"
    RUSTUP_INIT_SKIP_PATH_CHECK=yes "$D/downloads/rustup-init" -y --no-modify-path --profile minimal --default-host aarch64-apple-darwin --default-toolchain "$RUST_VERSION" --target aarch64-linux-android
else
    "$CARGO_HOME/bin/rustup" toolchain install "$RUST_VERSION" --profile minimal --target aarch64-linux-android --no-self-update
fi
"$CARGO_HOME/bin/rustup" run "$RUST_VERSION" rustc --version > "$D/artifacts/rustc-version.txt"
# No global homes, shell startup files, or system defaults are modified.
