#!/usr/bin/env bash
# Path 2: third_party drop-in with CMake. add_subdirectory(slog) then link the
# exported slog::slog target. A subdirectory build never inherits slog's strict
# warnings.
set -euo pipefail
REPO="$(cd "$(dirname "$0")/../../.." && pwd)"

cmake -S . -B build -DSLOG_REPO="$REPO" >/dev/null
cmake --build build >/dev/null
./build/app
