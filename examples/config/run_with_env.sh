#!/usr/bin/env bash
# Configure slog entirely through environment variables, then run the demo. No
# code change and no rebuild: the same binary behaves differently by env alone.
#
# Usage: run_with_env.sh [path-to-config_demo]
# Build config_demo first (see examples/README.md), then pass its path here, or
# run this from the directory that holds it.
set -euo pipefail

export SLOG_VERBOSITY="warning,net=debug"  # global warning, net at debug
export SLOG_STDOUT="info"                   # console shows info and more severe
export SLOG_DIR="./logs"                    # write files under ./logs
export SLOG_TIME="on"                       # add the time field to the prefix

DEMO="${1:-./config_demo}"
exec "$DEMO"
