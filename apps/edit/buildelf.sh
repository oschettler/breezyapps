#!/bin/sh
set -e
cd "$(dirname "$0")"

TC="$HOME/.espressif/tools/xtensa-esp-elf/esp-14.2.0_20241119/xtensa-esp-elf/bin"
GCC="$TC/xtensa-esp32s3-elf-gcc"
STRIP="$TC/xtensa-esp32s3-elf-strip"
NM="$TC/xtensa-esp32s3-elf-nm"

DIST="$(pwd)/dist"
mkdir -p "$DIST"
OUT="$DIST/edit.xtensa.elf"

"$GCC" \
  -O2 \
  -Dmain=app_main \
  -nostartfiles -nostdlib \
  -fPIC -shared \
  -fvisibility=hidden \
  -Wl,-e,app_main \
  -Wl,--gc-sections \
  edit.c -o "$OUT"

"$STRIP" --strip-all --remove-section=.xt.prop "$OUT"

echo "built $OUT"
echo "Potential required exports:"
"$NM" -D -u "$OUT"
