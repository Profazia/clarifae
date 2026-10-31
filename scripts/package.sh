#!/usr/bin/env bash
# package.sh — zip the module into a flashable dist/clarifae-<ver>.zip.
# Excludes build-only material (native sources, build dirs, host scripts).
set -euo pipefail

MOD="$(cd "$(dirname "$0")/.." && pwd)"     # project root == module root
VER="$(grep '^version=' "$MOD/module.prop" | cut -d= -f2)"
OUT="$MOD/dist/clarifae-$VER.zip"

# Warn (don't fail) if the native lib hasn't been built yet.
if ! ls "$MOD"/system/lib*/soundfx/libclarifae.so >/dev/null 2>&1 \
   && ! ls "$MOD"/lib/*/libclarifae.so >/dev/null 2>&1; then
  echo "WARNING: no libclarifae.so staged. Run ./build.sh first or the zip won't install." >&2
fi

mkdir -p "$MOD/dist"
rm -f "$OUT"

# Zip the module, excluding build-only material AND the repo docs/assets that now
# live alongside the module in the same folder.
( cd "$MOD" && zip -r9 "$OUT" . \
    -x 'native/*' \
    -x 'experimental/*' \
    -x 'build/*' \
    -x 'scripts/*' \
    -x 'build.sh' \
    -x 'dist/*' \
    -x 'docs/*' \
    -x '.git/*' \
    -x '*.o' \
    -x '*.tmp*' \
    -x 'rnnoise-*.tar.gz' \
    -x 'README.md' \
    -x 'INSTALL.md' \
    -x 'realtime_audio_denoising_models.md' \
    -x 'brand.txt' \
    -x 'logo.svg' >/dev/null )

echo "Created: $OUT  ($(du -h "$OUT" | cut -f1))"
echo "Contents (top level):"
unzip -l "$OUT" | awk 'NR>3 && $4 !~ /\// {print "  "$4}' | head -20
echo "  ..."
echo
echo "Flash $OUT in Magisk / KernelSU / APatch."
