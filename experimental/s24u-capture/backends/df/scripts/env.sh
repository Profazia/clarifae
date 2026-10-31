# Source from this directory's scripts; D must already be absolute.
. "$D/pins.env"
export RUSTUP_HOME="$D/toolchain/rustup" CARGO_HOME="$D/toolchain/cargo"
export PATH="$CARGO_HOME/bin:$PATH"
[ -x "$CARGO_HOME/bin/rustup" ] || { echo "Run scripts/setup-toolchain.sh first" >&2; exit 1; }
