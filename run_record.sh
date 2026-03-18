#!/bin/bash
set -e

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$script_dir" || exit 1

RUNTIME_BIN="./build/src/record_runtime/record_runtime"
DISK_ROOT="/mnt/data_disk"
WAIT_INTERVAL_SEC=2
WAIT_LOG_INTERVAL_SEC=30

if [ ! -x "$RUNTIME_BIN" ]; then
    echo "[ERROR] record_runtime binary not found: $RUNTIME_BIN"
    echo "[INFO] build it first: cmake -S . -B build && cmake --build build --target record_runtime -j\$(nproc)"
    exit 1
fi

is_storage_ready() {
    local mount_source mount_opts

    if ! findmnt -rn --target "$DISK_ROOT" >/dev/null 2>&1; then
        return 1
    fi

    mount_source="$(findmnt -rn -o SOURCE --target "$DISK_ROOT" 2>/dev/null || true)"
    mount_opts="$(findmnt -rn -o OPTIONS --target "$DISK_ROOT" 2>/dev/null || true)"

    [ -n "$mount_source" ] || return 1
    [ -e "$mount_source" ] || return 1

    case ",$mount_opts," in
        *,rw,*)
            ;;
        *)
            return 1
            ;;
    esac

    [ -w "$DISK_ROOT" ]
}

last_wait_log_ts=0
while ! is_storage_ready; do
    now_ts=$(date +%s)
    if [ $((now_ts - last_wait_log_ts)) -ge "$WAIT_LOG_INTERVAL_SEC" ]; then
        echo "[INFO] waiting for writable data storage on $DISK_ROOT"
        last_wait_log_ts=$now_ts
    fi
    sleep "$WAIT_INTERVAL_SEC"
done

exec "$RUNTIME_BIN" "$@"
