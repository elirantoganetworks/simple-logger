#!/usr/bin/env bash
# Path 1: copied sources. Compile slog's src/*.cpp straight into your program.
# Nothing to install, no CMake. This is the fastest way to try slog.
set -euo pipefail
REPO="$(cd "$(dirname "$0")/../../.." && pwd)"

c++ -std=c++17 -I "$REPO/include" app.cpp "$REPO"/src/*.cpp -o app -pthread
./app
