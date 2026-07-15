#!/usr/bin/env bash
# Path 3: prebuilt shared library. Build and install slog once into a local
# prefix, then compile against the installed headers and link -lslog.
set -euo pipefail
REPO="$(cd "$(dirname "$0")/../../.." && pwd)"
PREFIX="$PWD/prefix"

cmake -S "$REPO" -B build-lib -DBUILD_SHARED_LIBS=ON -DSLOG_BUILD_TESTS=OFF >/dev/null
cmake --build build-lib >/dev/null
cmake --install build-lib --prefix "$PREFIX" >/dev/null

c++ -std=c++17 -I "$PREFIX/include" app.cpp -o app \
    -L"$PREFIX/lib" -lslog -Wl,-rpath,"$PREFIX/lib" -pthread
./app
