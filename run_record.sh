#!/bin/bash
set -e

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$script_dir" || exit 1

RUNTIME_BIN="./build/src/record_runtime/record_runtime"

if [ ! -x "$RUNTIME_BIN" ]; then
    echo "[ERROR] record_runtime binary not found: $RUNTIME_BIN"
    echo "[INFO] build it first: cmake -S . -B build && cmake --build build --target record_runtime -j\$(nproc)"
    exit 1
fi

exec "$RUNTIME_BIN" "$@"
