#!/usr/bin/env bash
# Build the headless EDOPro-core duel runner (Phase 3+).
#
#   ./build.sh
#
# Produces ./duel_runner_edo (EDOPro ocgcore fork + Edison-format flags +
# ML neural policies). Requirements: g++ (C++14/17), sqlite3.
set -euo pipefail
cd "$(dirname "$0")"

echo ">> building vendored lua 5.4.8 as C++ (exceptions) for the EDOPro core"
# a fresh clone has no liblua_cpp.a; build it from the vendored tarball rather
# than telling the reader to fetch Lua from the network
if [ ! -f lua54/liblua_cpp.a ]; then
    ./build_lua.sh
fi

echo ">> compiling edo-core (EDOPro ocgcore fork; mangled lua linkage, no C-wrap)"
rm -rf edo-core/build_wrap
mkdir -p edo-core/build_wrap
for f in edo-core/*.cpp; do
    g++ -std=c++17 -fPIC -c "$f" -I edo-core -I lua54/src \
        -o "edo-core/build_wrap/$(basename "${f%.cpp}").o"
done
ar rcs edo-core/build_wrap/libocgcore_edo.a edo-core/build_wrap/*.o

echo ">> compiling duel_runner_edo"
g++ -std=c++14 -O2 -I edo-core -I lua54/src \
    duel_runner_edo.cpp edo-core/build_wrap/libocgcore_edo.a lua54/liblua_cpp.a -lsqlite3 -o duel_runner_edo

echo ">> done: ./duel_runner_edo (EDOPro core + Edison flags)"
