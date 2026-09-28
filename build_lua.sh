#!/usr/bin/env bash
# Build the vendored Lua 5.4.8 into lua54/liblua_cpp.a.
#
#   ./build_lua.sh
#
# The EDOPro core is compiled as C++ and links Lua with C++ mangling, so the
# stock C build cannot be used: every unit is compiled with -fexceptions and
# -DLUA_USE_CXX.  build.sh calls this automatically when lua54/liblua_cpp.a is
# missing, so a fresh clone needs nothing but ./build.sh.
#
# The tarball is the pinned input; lua54/ is its extracted copy (also committed,
# so the tree is readable without unpacking).  Extraction is skipped when the
# sources are already there, which keeps this non-destructive in a working tree.
set -euo pipefail
cd "$(dirname "$0")"

if [ ! -d lua54/src ]; then
    echo ">> unpacking Lua 5.4.8 from lua54.tar.gz"
    [ -f lua54.tar.gz ] || { echo "lua54.tar.gz is missing" >&2; exit 1; }
    mkdir -p lua54
    tar xzf lua54.tar.gz -C lua54 --strip-components=1
fi

echo ">> compiling lua54/liblua_cpp.a (C++, exceptions on)"
cd lua54/src
for f in lapi lauxlib lbaselib lcode lcorolib lctype ldblib ldebug ldo ldump \
         lfunc lgc linit liolib llex lmathlib lmem lobject lopcodes loslib lparser \
         lstate lstring lstrlib ltable ltablib ltm lundump lutf8lib lvm lzio; do
    g++ -std=c++17 -fexceptions -DLUA_USE_CXX -O2 -c "$f.c" -o "$f.o"
done
ar rcs ../liblua_cpp.a ./*.o
echo ">> done: lua54/liblua_cpp.a"
