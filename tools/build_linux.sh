#!/usr/bin/env bash
# VidScope Linux/macOS build wrapper.
# No arguments => Release configure + build + tests.

set -Eeuo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
BUILD_SCRIPT="$SCRIPT_DIR/build.py"

if command -v python3 >/dev/null 2>&1; then
    PYTHON_BIN="$(command -v python3)"
elif command -v python >/dev/null 2>&1; then
    PYTHON_BIN="$(command -v python)"
else
    echo "[ERROR] Python 3.8+ was not found in PATH." >&2
    exit 1
fi

if [[ $# -eq 0 ]]; then
    echo "[INFO] VidScope default: Release build + tests"
fi

exec "$PYTHON_BIN" "$BUILD_SCRIPT" "$@"
