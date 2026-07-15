#!/usr/bin/env bash
# Build and run the consumer program three ways: copied sources, third_party
# subdirectory, and a prebuilt shared library. Any failure fails the test.
set -euo pipefail

ROOT="$1"     # repo root
WORK="$2"     # scratch dir
CXX="${CXX:-c++}"
APP="$ROOT/tests/consume/app.cpp"

rm -rf "$WORK"
mkdir -p "$WORK"

echo "== path 1: copied sources =="
mkdir -p "$WORK/copy"
"$CXX" -std=c++17 -I "$ROOT/include" "$APP" "$ROOT"/src/*.cpp \
    -o "$WORK/copy/app" -pthread
"$WORK/copy/app"

echo "== path 2: third_party add_subdirectory =="
cmake -S "$ROOT/tests/consume/third_party" -B "$WORK/tp" \
    -DSLOG_REPO="$ROOT" -DAPP="$APP" -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$WORK/tp" >/dev/null
"$WORK/tp/consumer"

echo "== path 3: prebuilt shared library =="
cmake -S "$ROOT" -B "$WORK/lib" -DBUILD_SHARED_LIBS=ON \
    -DSLOG_BUILD_TESTS=OFF -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$WORK/lib" >/dev/null
cmake --install "$WORK/lib" --prefix "$WORK/prefix" >/dev/null
"$CXX" -std=c++17 -I "$WORK/prefix/include" "$APP" -o "$WORK/prebuilt_app" \
    -L"$WORK/prefix/lib" -lslog -Wl,-rpath,"$WORK/prefix/lib" -pthread
"$WORK/prebuilt_app"

echo "== path 4: find_package(slog) =="
mkdir -p "$WORK/fp"
cat > "$WORK/fp/CMakeLists.txt" <<EOF
cmake_minimum_required(VERSION 3.14)
project(slog_find_package_consumer CXX)
find_package(slog REQUIRED)
add_executable(fp_app "$APP")
target_link_libraries(fp_app PRIVATE slog::slog)
EOF
cmake -S "$WORK/fp" -B "$WORK/fp/build" -DCMAKE_PREFIX_PATH="$WORK/prefix" \
    -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$WORK/fp/build" >/dev/null
"$WORK/fp/build/fp_app"

echo "== symbol check: the shared library exports no internal symbols =="
LIB=$(ls "$WORK/prefix"/lib/libslog.so 2>/dev/null | head -1)
if [ -z "$LIB" ]; then
    echo "FAIL: shared library not found for the symbol check"
    exit 1
fi
LEAKED=$(nm -D --defined-only -C "$LIB" | grep -c 'slog::detail::' || true)
if [ "$LEAKED" -ne 0 ]; then
    echo "FAIL: $LEAKED internal slog::detail:: symbols are exported"
    nm -D --defined-only -C "$LIB" | grep 'slog::detail::' | head
    exit 1
fi
echo "ok: no internal symbols exported"

echo "all consumption paths built and ran"
