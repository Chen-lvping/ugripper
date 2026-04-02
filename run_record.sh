#!/bin/bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$script_dir" || exit 1

RUNTIME_BIN="./build/src/record_runtime/record_runtime"
ENV_FILE="/etc/environment"
DISK_ROOT="/mnt/data_disk"
LOG_DIR_LOCAL="/tmp"
LOG_DIR_DISK="$DISK_ROOT/logs"
WAIT_INTERVAL_SEC=2

trim_text() {
    local text="${1:-}"
    text="${text#"${text%%[![:space:]]*}"}"
    text="${text%"${text##*[![:space:]]}"}"
    printf '%s' "$text"
}

read_env_value() {
    local key="$1"
    local line=""
    local current_key=""
    local value=""

    [ -f "$ENV_FILE" ] || return 1

    while IFS= read -r line || [ -n "$line" ]; do
        line="$(trim_text "$line")"
        [ -n "$line" ] || continue

        case "$line" in
            \#*)
                continue
                ;;
        esac

        if [ "${line#*=}" = "$line" ]; then
            continue
        fi

        current_key="$(trim_text "${line%%=*}")"
        value="$(trim_text "${line#*=}")"
        if [ "$current_key" != "$key" ]; then
            continue
        fi

        value="${value#\"}"
        value="${value%\"}"
        value="${value#\'}"
        value="${value%\'}"
        printf '%s' "$(trim_text "$value")"
        return 0
    done < "$ENV_FILE"

    return 0
}

DEVICE_SN="$(read_env_value "DEVICE_SN" || true)"
DEVICE_SN_LOWER="${DEVICE_SN,,}"
DEVICE_SN_LOWER="${DEVICE_SN_LOWER:-unknown_device}"
TODAY="$(date +%Y%m%d)"

LOG_FILE_LOCAL="$LOG_DIR_LOCAL/umi_sys_${DEVICE_SN_LOWER}_${TODAY}.log"
LOG_FILE_DISK="$LOG_DIR_DISK/umi_sys_${DEVICE_SN_LOWER}_${TODAY}.log"
LOG_SYNC_POS_FILE="/tmp/umi_sys_${DEVICE_SN_LOWER}_${TODAY}.pos"
LOG_SYNC_LOCK_FILE="/tmp/umi_sys_${DEVICE_SN_LOWER}_${TODAY}.lock"

cleanup_old_logs() {
    local dir=""
    local file=""

    for dir in "$LOG_DIR_LOCAL" "$LOG_DIR_DISK"; do
        [ -d "$dir" ] || continue

        shopt -s nullglob
        for file in "$dir"/umi_sys_"${DEVICE_SN_LOWER}"_*.log; do
            if [[ "$file" != *"${TODAY}.log" ]]; then
                echo "[INFO] deleting old log: $file"
                rm -f "$file"
            fi
        done
        shopt -u nullglob
    done
}

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

sync_logs_once() {
    local src="$1"
    local dest="$2"
    local state_file="$3"
    local lock_file="$4"
    local last_pos=0
    local curr_size=0
    local lock_fd=""

    [ -f "$src" ] || return 0

    if ! is_storage_ready; then
        return 1
    fi

    mkdir -p "$(dirname "$dest")" 2>/dev/null || return 1

    exec {lock_fd}>"$lock_file" || return 1
    if ! flock -x "$lock_fd"; then
        exec {lock_fd}>&-
        return 1
    fi

    if [ -f "$state_file" ]; then
        last_pos="$(cat "$state_file" 2>/dev/null || echo 0)"
    fi
    curr_size="$(stat -c%s "$src" 2>/dev/null || echo 0)"

    if [ "$curr_size" -lt "$last_pos" ]; then
        last_pos=0
    fi

    if [ "$curr_size" -le "$last_pos" ]; then
        flock -u "$lock_fd" || true
        exec {lock_fd}>&-
        return 0
    fi

    if ! tail -c +$((last_pos + 1)) "$src" >> "$dest" 2>/dev/null; then
        flock -u "$lock_fd" || true
        exec {lock_fd}>&-
        return 1
    fi
    printf '%s' "$curr_size" > "$state_file"

    flock -u "$lock_fd" || true
    exec {lock_fd}>&-
}

cleanup() {
    sync_logs_once "$LOG_FILE_LOCAL" "$LOG_FILE_DISK" "$LOG_SYNC_POS_FILE" "$LOG_SYNC_LOCK_FILE" || true
}

mkdir -p "$LOG_DIR_LOCAL"
cleanup_old_logs

echo "[INFO] logging locally to: $LOG_FILE_LOCAL"
exec > >(tee -a "$LOG_FILE_LOCAL") 2>&1

trap cleanup EXIT

wait_logged=false
while ! is_storage_ready; do
    if [ "$wait_logged" = false ]; then
        echo "[INFO] waiting for writable data storage on $DISK_ROOT"
        wait_logged=true
    fi
    sleep "$WAIT_INTERVAL_SEC"
done

if [ "$wait_logged" = true ]; then
    echo "[INFO] writable data storage ready on $DISK_ROOT"
fi

cleanup_old_logs

runtime_exit_code=0
if "$RUNTIME_BIN" "$@"; then
    runtime_exit_code=0
else
    runtime_exit_code=$?
fi

exit "$runtime_exit_code"
