#!/bin/sh
set -e
cd "$(dirname "$0")"

TC="$HOME/.espressif/tools/xtensa-esp-elf/esp-14.2.0_20241119/xtensa-esp-elf/bin"
GCC="$TC/xtensa-esp32s3-elf-gcc"
AR="$TC/xtensa-esp32s3-elf-ar"
STRIP="$TC/xtensa-esp32s3-elf-strip"
NM="$TC/xtensa-esp32s3-elf-nm"

LUA_VER="5.4.7"
LUA_DIR="lua-$LUA_VER"
LUA_URL="https://www.lua.org/ftp/lua-$LUA_VER.tar.gz"

if [ ! -d "$LUA_DIR" ]; then
    curl -L "$LUA_URL" | tar xz
fi

DIST="$(pwd)/dist"
mkdir -p "$DIST"

LIBGCC="$HOME/.espressif/tools/xtensa-esp-elf/esp-14.2.0_20241119/xtensa-esp-elf/lib/gcc/xtensa-esp-elf/14.2.0/esp32s3/libgcc.a"

CFLAGS="-O2 -fPIC -DLUA_32BITS -I$LUA_DIR/src -fno-common"

# Build liblua.a from all Lua core + lib sources (exclude lua.c and luac.c — standalone mains)
SRCS=$(ls "$LUA_DIR/src"/*.c | grep -v -e '/lua\.c$' -e '/luac\.c$')
OBJS=""
for src in $SRCS; do
    obj="${src%.c}.o"
    "$GCC" $CFLAGS -c "$src" -o "$obj"
    OBJS="$OBJS $obj"
done
"$AR" rcs liblua.a $OBJS

# Link lua_main.c + liblua.a into a shared ELF
"$GCC" \
  -O2 -fPIC \
  -DLUA_32BITS \
  -I"$LUA_DIR/src" \
  -nostartfiles -nostdlib \
  -shared \
  -fvisibility=hidden \
  -Wl,-e,app_main \
  -Wl,--gc-sections \
  lua_main.c liblua.a "$LIBGCC" \
  -o "$DIST/lua.xtensa.elf"

"$STRIP" --strip-all --remove-section=.xt.prop "$DIST/lua.xtensa.elf"
echo "built $DIST/lua.xtensa.elf"
echo "Potential required exports:"
"$NM" -D -u "$DIST/lua.xtensa.elf"
