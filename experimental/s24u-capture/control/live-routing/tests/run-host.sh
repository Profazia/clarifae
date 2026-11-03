#!/usr/bin/env bash
# Host mocks only. Never runs the production Android manager entrypoint.
set -euo pipefail
HERE=$(cd "$(dirname "$0")/.." && pwd)
sh -n "$HERE/live-targets.sh"
sh -n "$HERE/integration/functions.sh"
sh -n "$HERE/integration/clarifae-ctl"
node --check "$HERE/integration/app.js"
node --check "$HERE/integration/kernelsu.js"
python3 "$HERE/tests/test_renderer.py"
python3 "$HERE/tests/test_transactions.py"
python3 "$HERE/tests/test_safety.py"
python3 "$HERE/tests/test_integration.py"
node --test "$HERE/tests/test_ui.cjs"
