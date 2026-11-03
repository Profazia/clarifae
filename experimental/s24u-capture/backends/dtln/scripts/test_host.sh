#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
PYTHON=${PYTHON:-python3.11}
"$PYTHON" "$ROOT/scripts/fetch_assets.py" --host
if ! test -x "$ROOT/build/host-venv/bin/python"; then
 "$PYTHON" -m venv "$ROOT/build/host-venv"
fi
"$ROOT/build/host-venv/bin/python" -m pip install --no-index --no-deps --no-cache-dir "$ROOT"/downloads/numpy-2.4.6-cp311-*.whl
exec "$ROOT/build/host-venv/bin/python" "$ROOT/tests/host_test.py"
