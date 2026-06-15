#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$(dirname "$SCRIPT_DIR")"
RUN_FAYS_RECORD="$SCRIPT_DIR/run_fays_record.sh"

CONTROL_FIFO="/tmp/umi_stereo_camera_control.pipe"
STATUS_FILE="/tmp/umi_stereo_camera_status.json"
LEFT_CONFIG="$BUILD_DIR/config/fays_vikit_left.yaml"
RIGHT_CONFIG="$BUILD_DIR/config/fays_vikit_right.yaml"
LEFT_FIFO="/tmp/umi_left_fays_cmd"
RIGHT_FIFO="/tmp/umi_right_fays_cmd"
LEFT_CALIB_JSON="/tmp/umi_left_fays_calibration.json"
RIGHT_CALIB_JSON="/tmp/umi_right_fays_calibration.json"
LEFT_RUNTIME_STATUS_JSON="/dev/shm/umi_left_fays_runtime_status.json"
RIGHT_RUNTIME_STATUS_JSON="/dev/shm/umi_right_fays_runtime_status.json"
POLL_INTERVAL_SEC="${FAYS_STEREO_POLL_INTERVAL_SEC:-0.2}"
HEALTH_POLL_INTERVAL_SEC="${FAYS_STEREO_HEALTH_POLL_INTERVAL_SEC:-1}"
FINALIZE_TIMEOUT_SEC="${FAYS_STEREO_FINALIZE_TIMEOUT_SEC:-10}"
START_DELAY_SEC="${FAYS_STEREO_START_DELAY_SEC:-1.5}"
START_COMPLETE_TIMEOUT_SEC="${FAYS_STEREO_START_COMPLETE_TIMEOUT_SEC:-10}"
HEALTH_GRACE_SEC="${FAYS_STEREO_HEALTH_GRACE_SEC:-3}"
FRAME_STALE_SEC="${FAYS_STEREO_FRAME_STALE_SEC:-3}"
SYMLINK_STABLE_SEC="${FAYS_STEREO_SYMLINK_STABLE_SEC:-1}"
SYMLINK_STABLE_TIMEOUT_SEC="${FAYS_STEREO_SYMLINK_STABLE_TIMEOUT_SEC:-8}"
EXIT_GRACE_SEC="${FAYS_STEREO_EXIT_GRACE_SEC:-2}"
TERM_GRACE_SEC="${FAYS_STEREO_TERM_GRACE_SEC:-2}"
PORT_FREE_TIMEOUT_SEC="${FAYS_STEREO_PORT_FREE_TIMEOUT_SEC:-3}"
PORT_KILL_GRACE_SEC="${FAYS_STEREO_PORT_KILL_GRACE_SEC:-1}"

LEFT_PID=""
RIGHT_PID=""
LEFT_STARTED_AT=0
RIGHT_STARTED_AT=0
LAST_COMMAND_SEQ=0
ACTIVE_EPISODE_DIR=""
ACTIVE_START_US=0
ACTIVE_STOP_US=0
LAST_FINALIZED_EPISODE_DIR=""
LAST_FINALIZE_ERROR=""
LAST_SESSION_JSON="{}"
LAST_HEALTH_CHECK_MS=0
RECORDING=false
FINALIZE_PENDING=false
CLEANED_UP=false

mark_session_error() {
    local message="$1"
    if [ "$RECORDING" = "true" ] || [ "$FINALIZE_PENDING" = "true" ]; then
        if [ -z "$LAST_FINALIZE_ERROR" ]; then
            LAST_FINALIZE_ERROR="$message"
        fi
    fi
}

now_ms() {
    date +%s%3N
}

fays_ts() {
    date +"%H:%M:%S.%6N"
}

fays_log() {
    printf '[FAYS_TS %s] [fays_stereo_daemon] %s\n' "$(fays_ts)" "$*" >&2
}

usage() {
    cat <<'EOF'
Usage:
  run_fays_stereo_daemon.sh [options]

Options:
  --control-fifo FIFO
  --status-file FILE
  --left-config FILE
  --right-config FILE
  --left-fifo FIFO
  --right-fifo FIFO
  --left-calib-json FILE
  --right-calib-json FILE
  --left-runtime-status-json FILE
  --right-runtime-status-json FILE
EOF
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --control-file)
            CONTROL_FIFO="${2%.json}.pipe"
            shift 2
            ;;
        --control-fifo|--control-pipe)
            CONTROL_FIFO="$2"
            shift 2
            ;;
        --status-file)
            STATUS_FILE="$2"
            shift 2
            ;;
        --left-config)
            LEFT_CONFIG="$2"
            shift 2
            ;;
        --right-config)
            RIGHT_CONFIG="$2"
            shift 2
            ;;
        --left-fifo)
            LEFT_FIFO="$2"
            shift 2
            ;;
        --right-fifo)
            RIGHT_FIFO="$2"
            shift 2
            ;;
        --left-calib-json)
            LEFT_CALIB_JSON="$2"
            shift 2
            ;;
        --right-calib-json)
            RIGHT_CALIB_JSON="$2"
            shift 2
            ;;
        --left-runtime-status-json)
            LEFT_RUNTIME_STATUS_JSON="$2"
            shift 2
            ;;
        --right-runtime-status-json)
            RIGHT_RUNTIME_STATUS_JSON="$2"
            shift 2
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "Unknown argument: $1" >&2
            usage >&2
            exit 1
            ;;
    esac
done

ensure_control_fifo() {
    if [ -e "$CONTROL_FIFO" ] && [ ! -p "$CONTROL_FIFO" ]; then
        echo "Error: stereo control path exists but is not a FIFO: $CONTROL_FIFO" >&2
        return 1
    fi
    if [ ! -p "$CONTROL_FIFO" ]; then
        rm -f "$CONTROL_FIFO"
        mkfifo "$CONTROL_FIFO"
    fi
    return 0
}

write_status() {
    local left_process_ready=false
    local right_process_ready=false

    if [ -n "$LEFT_PID" ] && kill -0 "$LEFT_PID" 2>/dev/null && [ -p "$LEFT_FIFO" ]; then
        left_process_ready=true
    fi
    if [ -n "$RIGHT_PID" ] && kill -0 "$RIGHT_PID" 2>/dev/null && [ -p "$RIGHT_FIFO" ]; then
        right_process_ready=true
    fi

    STATUS_FILE="$STATUS_FILE" \
    RECORDING="$RECORDING" \
    FINALIZE_PENDING="$FINALIZE_PENDING" \
    ACTIVE_EPISODE_DIR="$ACTIVE_EPISODE_DIR" \
    LAST_FINALIZED_EPISODE_DIR="$LAST_FINALIZED_EPISODE_DIR" \
    LAST_FINALIZE_ERROR="$LAST_FINALIZE_ERROR" \
    LAST_SESSION_JSON="$LAST_SESSION_JSON" \
    LEFT_PROCESS_READY="$left_process_ready" \
    RIGHT_PROCESS_READY="$right_process_ready" \
    LEFT_CONFIG="$LEFT_CONFIG" \
    RIGHT_CONFIG="$RIGHT_CONFIG" \
    LEFT_CALIB_JSON="$LEFT_CALIB_JSON" \
    RIGHT_CALIB_JSON="$RIGHT_CALIB_JSON" \
    LEFT_RUNTIME_STATUS_JSON="$LEFT_RUNTIME_STATUS_JSON" \
    RIGHT_RUNTIME_STATUS_JSON="$RIGHT_RUNTIME_STATUS_JSON" \
    python3 - <<'PY'
import json
import os
import tempfile
import time

def as_bool(name):
    return os.environ.get(name, "false") == "true"

def path_exists(path):
    return bool(path) and os.path.exists(path)

def calibration_state(path):
    root = {
        "path": path,
        "valid": False,
        "status": "missing",
        "serial_number": "",
        "devices": {},
    }
    if not path:
        root["status"] = "disabled"
        return root
    if not os.path.isfile(path):
        return root
    try:
        with open(path, "r", encoding="utf-8") as f:
            data = json.load(f)
    except Exception as exc:
        root["status"] = f"invalid_json:{exc}"
        return root
    if isinstance(data, dict) and data.get("valid") is True:
        device_info = data.get("device_info", {})
        root["valid"] = True
        root["status"] = "ready"
        root["schema"] = data.get("schema", "")
        root["sdk_version"] = device_info.get("sdk_version", "")
        root["serial_number"] = device_info.get("serial_number", "")
        root["device_model"] = device_info.get("device_model", "")
        root["firmware_version"] = device_info.get("firmware_version", "")
        if isinstance(data.get("devices"), dict):
            root["devices"] = data["devices"]
    else:
        root["status"] = "invalid_payload"
    return root

def runtime_state(path):
    root = {
        "path": path,
        "valid": False,
        "status": "missing",
    }
    if not path:
        root["status"] = "disabled"
        return root
    if not os.path.isfile(path):
        return root
    try:
        with open(path, "r", encoding="utf-8") as f:
            data = json.load(f)
    except Exception as exc:
        root["status"] = f"invalid_json:{exc}"
        return root
    if not isinstance(data, dict):
        root["status"] = "invalid_payload"
        return root
    root.update(data)
    root["valid"] = True
    last_frame_ns = int(data.get("last_frame_system_ns", 0) or 0)
    if last_frame_ns <= 0:
        root["status"] = "no_frame"
        root["frame_age_s"] = None
    else:
        age_s = max(0.0, (time.time_ns() - last_frame_ns) / 1e9)
        root["frame_age_s"] = round(age_s, 3)
        root["status"] = "ready"
    return root

def camera_state(name, process_ready, config_path, calibration, runtime):
    serial = calibration.get("serial_number", "")
    devices = calibration.get("devices", {})
    stereo_path = devices.get("stereo_dev_port", "") if isinstance(devices, dict) else ""
    imu_path = devices.get("imu_dev_port", "") if isinstance(devices, dict) else ""
    stereo_online = path_exists(stereo_path)
    imu_online = path_exists(imu_path)
    ready = bool(
        process_ready and
        calibration.get("valid") and
        serial and
        stereo_online and
        imu_online
    )
    state = "ready" if ready else "not-ready"
    if process_ready and calibration.get("valid") and serial and not stereo_online:
        state = "stereo-symlink-missing"
    elif process_ready and calibration.get("valid") and serial and not imu_online:
        state = "imu-symlink-missing"
    elif process_ready and calibration.get("valid") and not serial:
        state = "serial-missing"
    elif process_ready and not calibration.get("valid"):
        state = "calibration-" + str(calibration.get("status", "invalid"))
    elif process_ready and runtime.get("status") in ("missing", "disabled"):
        state = "runtime-status-missing"
    elif not process_ready and stereo_online and imu_online:
        state = "process-not-ready"
    elif not process_ready:
        state = "process-not-ready"
    return {
        "state": state,
        "device": config_path,
        "ready": ready,
        "process_ready": process_ready,
        "session_recording": as_bool("RECORDING"),
        "serial_number": serial,
        "stereo_symlink_online": stereo_online,
        "imu_symlink_online": imu_online,
        "calibration": calibration,
        "runtime": runtime,
    }

status_file = os.environ["STATUS_FILE"]
try:
    last_session = json.loads(os.environ.get("LAST_SESSION_JSON", "{}") or "{}")
except Exception:
    last_session = {}

left_calibration = calibration_state(os.environ.get("LEFT_CALIB_JSON", ""))
right_calibration = calibration_state(os.environ.get("RIGHT_CALIB_JSON", ""))
left_runtime = runtime_state(os.environ.get("LEFT_RUNTIME_STATUS_JSON", ""))
right_runtime = runtime_state(os.environ.get("RIGHT_RUNTIME_STATUS_JSON", ""))
left_process_ready = as_bool("LEFT_PROCESS_READY")
right_process_ready = as_bool("RIGHT_PROCESS_READY")
left_camera = camera_state(
    "left_stereo",
    left_process_ready,
    os.environ.get("LEFT_CONFIG", ""),
    left_calibration,
    left_runtime,
)
right_camera = camera_state(
    "right_stereo",
    right_process_ready,
    os.environ.get("RIGHT_CONFIG", ""),
    right_calibration,
    right_runtime,
)

multi_device_error = ""
left_serial = left_camera.get("serial_number", "")
right_serial = right_camera.get("serial_number", "")
if left_process_ready and right_process_ready:
    if not left_serial or not right_serial:
        multi_device_error = "missing_fays_serial"
    elif left_serial == right_serial:
        multi_device_error = f"duplicate_fays_serial:{left_serial}"

if multi_device_error:
    left_camera["ready"] = False
    right_camera["ready"] = False
    left_camera["state"] = multi_device_error
    right_camera["state"] = multi_device_error

ready = bool(left_camera["ready"] and right_camera["ready"] and not multi_device_error)
if as_bool("FINALIZE_PENDING"):
    service_state = "finalizing"
elif as_bool("RECORDING"):
    service_state = "recording"
elif ready:
    service_state = "ready"
else:
    service_state = "not-ready"

root = {
    "recording": as_bool("RECORDING"),
    "finalize_pending": as_bool("FINALIZE_PENDING"),
    "active_episode_dir": os.environ.get("ACTIVE_EPISODE_DIR", ""),
    "last_finalized_episode_dir": os.environ.get("LAST_FINALIZED_EPISODE_DIR", ""),
    "last_finalize_error": os.environ.get("LAST_FINALIZE_ERROR", ""),
    "last_session": last_session,
    "ready": ready,
    "service_state": service_state,
    "multi_device_error": multi_device_error,
    "cameras": {
        "left_stereo": left_camera,
        "right_stereo": right_camera,
    },
}

directory = os.path.dirname(status_file) or "."
os.makedirs(directory, exist_ok=True)
fd, tmp = tempfile.mkstemp(prefix=".stereo_status.", dir=directory)
with os.fdopen(fd, "w", encoding="utf-8") as f:
    json.dump(root, f, indent=2)
    f.write("\n")
os.replace(tmp, status_file)
PY
}

wait_for_fifo() {
    local fifo="$1"
    local timeout_sec="${2:-8}"
    local deadline=$((SECONDS + timeout_sec))
    while [ "$SECONDS" -lt "$deadline" ]; do
        [ -p "$fifo" ] && return 0
        sleep 0.1
    done
    return 1
}

log_fifo_startup_timeout() {
    local side="$1"
    local fifo="$2"
    local pid="$3"
    local config="$4"
    local runtime_status_json="$5"
    local process_alive=false
    local stereo_path imu_path stereo_resolved="" imu_resolved=""

    kill -0 "$pid" 2>/dev/null && process_alive=true
    stereo_path="$(config_value "$config" stereo_dev_port)"
    imu_path="$(config_value "$config" imu_dev_port)"
    [ -n "$stereo_path" ] && stereo_resolved="$(readlink -f "$stereo_path" 2>/dev/null || true)"
    [ -n "$imu_path" ] && imu_resolved="$(readlink -f "$imu_path" 2>/dev/null || true)"
    fays_log "$side stereo control FIFO startup timeout: fifo=$fifo pid=$pid process_alive=$process_alive runtime_status_exists=$([ -f "$runtime_status_json" ] && echo true || echo false) stereo=${stereo_path:-<empty>} resolved_stereo=${stereo_resolved:-<missing>} imu=${imu_path:-<empty>} resolved_imu=${imu_resolved:-<missing>}"
}

wait_for_process_exit() {
    local pid="$1"
    local timeout_sec="$2"
    local deadline=$((SECONDS + timeout_sec))

    [ -z "$pid" ] && return 0
    while [ "$SECONDS" -lt "$deadline" ]; do
        if ! kill -0 "$pid" 2>/dev/null; then
            wait "$pid" 2>/dev/null || true
            return 0
        fi
        sleep 0.1
    done
    if ! kill -0 "$pid" 2>/dev/null; then
        wait "$pid" 2>/dev/null || true
        return 0
    fi
    return 1
}

process_group_id() {
    local pid="$1"
    ps -o pgid= -p "$pid" 2>/dev/null | tr -d '[:space:]'
}

current_process_group_id() {
    process_group_id "$$"
}

terminate_process_tree() {
    local pid="$1"
    local label="$2"
    local pgid
    local self_pgid

    [ -n "$pid" ] || return 0
    kill -0 "$pid" 2>/dev/null || return 0

    pgid="$(process_group_id "$pid")"
    self_pgid="$(current_process_group_id)"
    fays_log "$label: SIGTERM pid=$pid pgid=${pgid:-unknown}"
    if [ -n "$pgid" ] && [ "$pgid" != "$self_pgid" ]; then
        kill -TERM "-$pgid" 2>/dev/null || kill -TERM "$pid" 2>/dev/null || true
    else
        kill -TERM "$pid" 2>/dev/null || true
    fi
    if wait_for_process_exit "$pid" "$TERM_GRACE_SEC"; then
        return 0
    fi

    fays_log "$label: SIGKILL pid=$pid pgid=${pgid:-unknown}"
    if [ -n "$pgid" ] && [ "$pgid" != "$self_pgid" ]; then
        kill -KILL "-$pgid" 2>/dev/null || kill -KILL "$pid" 2>/dev/null || true
    else
        kill -KILL "$pid" 2>/dev/null || true
    fi
    wait_for_process_exit "$pid" 1 || true
}

config_value() {
    local config="$1"
    local key="$2"
    awk -F: -v key="$key" '
        $1 ~ "^[[:space:]]*" key "[[:space:]]*$" {
            sub(/^[^:]*:[[:space:]]*/, "", $0)
            sub(/[[:space:]]+#.*/, "", $0)
            gsub(/^[[:space:]]+|[[:space:]]+$/, "", $0)
            print
            exit
        }
    ' "$config"
}

side_video_nodes() {
    local config="$1"
    local stereo_path imu_path resolved

    stereo_path="$(config_value "$config" stereo_dev_port)"
    imu_path="$(config_value "$config" imu_dev_port)"
    for path in "$stereo_path" "$imu_path"; do
        [ -n "$path" ] && [ "$path" != "NULL" ] || continue
        [ -e "$path" ] || continue
        resolved="$(readlink -f "$path" 2>/dev/null || true)"
        [ -n "$resolved" ] || continue
        printf '%s\n' "$resolved"
    done | awk '!seen[$0]++'
}

fuser_pids_for_path() {
    local path="$1"
    fuser "$path" 2>/dev/null | tr ' ' '\n' | awk 'NF && $1 ~ /^[0-9]+$/'
}

log_side_port_users() {
    local side="$1"
    local config="$2"
    local node

    fays_log "$side video port users:"
    while IFS= read -r node; do
        [ -n "$node" ] || continue
        echo "  $node" >&2
        fuser -v "$node" >&2 2>&1 || true
    done <<EOF
$(side_video_nodes "$config")
EOF
}

kill_side_port_users() {
    local side="$1"
    local config="$2"
    local tracked_pid="${3:-}"
    local pids pid

    pids="$(
        while IFS= read -r node; do
            [ -n "$node" ] || continue
            fuser_pids_for_path "$node"
        done <<EOF
$(side_video_nodes "$config")
EOF
    )"
    pids="$(printf '%s\n' "$pids" | awk 'NF && !seen[$0]++')"
    [ -n "$pids" ] || return 0

    fays_log "$side video ports still busy; killing holders before restart"
    printf '%s\n' "$pids" | while IFS= read -r pid; do
        [ -n "$pid" ] || continue
        [ "$pid" != "$$" ] || continue
        if [ -n "$tracked_pid" ] && [ "$pid" = "$tracked_pid" ]; then
            terminate_process_tree "$pid" "$side tracked Fays recorder"
        else
            terminate_process_tree "$pid" "$side stale Fays video-port holder"
        fi
    done
}

ports_are_free() {
    local config="$1"
    local node

    while IFS= read -r node; do
        [ -n "$node" ] || continue
        if fuser "$node" >/dev/null 2>&1; then
            return 1
        fi
    done <<EOF
$(side_video_nodes "$config")
EOF
    return 0
}

wait_for_side_ports_free() {
    local side="$1"
    local config="$2"
    local timeout_sec="${3:-$PORT_FREE_TIMEOUT_SEC}"
    local deadline=$((SECONDS + timeout_sec))

    while [ "$SECONDS" -lt "$deadline" ]; do
        ports_are_free "$config" && return 0
        sleep 0.1
    done
    ports_are_free "$config"
}

wait_kill_and_confirm_side_ports_free() {
    local side="$1"
    local config="$2"
    local tracked_pid="${3:-}"

    if wait_for_side_ports_free "$side" "$config" "$PORT_FREE_TIMEOUT_SEC"; then
        return 0
    fi

    fays_log "$side video ports did not become free after ${PORT_FREE_TIMEOUT_SEC}s"
    log_side_port_users "$side" "$config"
    kill_side_port_users "$side" "$config" "$tracked_pid"

    if wait_for_side_ports_free "$side" "$config" "$PORT_KILL_GRACE_SEC"; then
        return 0
    fi

    fays_log "$side video ports are still busy after forced cleanup"
    log_side_port_users "$side" "$config"
    return 1
}

cleanup_stale_fays_recorders() {
    local executable="$BUILD_DIR/fays_record_example"
    local pid cmd

    [ -x "$executable" ] || return 0
    ps -eo pid=,args= | while IFS= read -r line; do
        pid="$(printf '%s\n' "$line" | awk '{print $1}')"
        cmd="$(printf '%s\n' "$line" | sed 's/^[[:space:]]*[0-9][0-9]*[[:space:]]*//')"
        [ -n "$pid" ] || continue
        [ "$pid" != "$$" ] || continue
        case "$cmd" in
            *"$executable"*)
                fays_log "stale recorder found on daemon startup: pid=$pid cmd=$cmd"
                terminate_process_tree "$pid" "stale Fays recorder"
                ;;
        esac
    done
}

check_side_device_paths() {
    local side="$1"
    local config="$2"
    local fifo="$3"
    local require_fifo="${4:-false}"
    local quiet="${5:-false}"
    local stereo_path imu_path
    stereo_path="$(config_value "$config" stereo_dev_port)"
    imu_path="$(config_value "$config" imu_dev_port)"

    if [ -z "$stereo_path" ] || [ "$stereo_path" = "NULL" ] || [ ! -e "$stereo_path" ]; then
        [ "$quiet" = "true" ] || fays_log "$side stereo symlink missing: ${stereo_path:-<empty>}"
        return 1
    fi
    if [ -z "$imu_path" ] || [ "$imu_path" = "NULL" ] || [ ! -e "$imu_path" ]; then
        [ "$quiet" = "true" ] || fays_log "$side IMU symlink missing: ${imu_path:-<empty>}"
        return 1
    fi
    if [ "$require_fifo" = "true" ] && [ ! -p "$fifo" ]; then
        [ "$quiet" = "true" ] || fays_log "$side FIFO missing: $fifo"
        return 1
    fi
    return 0
}

wait_for_side_symlinks_stable() {
    local side="$1"
    local config="$2"
    local stereo_path imu_path
    local last_stereo=""
    local last_imu=""
    local stable_since_ms=0
    local start_ms deadline_ms now_ms

    stereo_path="$(config_value "$config" stereo_dev_port)"
    imu_path="$(config_value "$config" imu_dev_port)"
    start_ms="$(date +%s%3N)"
    deadline_ms=$((start_ms + SYMLINK_STABLE_TIMEOUT_SEC * 1000))

    while true; do
        now_ms="$(date +%s%3N)"
        [ "$now_ms" -lt "$deadline_ms" ] || break

        if [ -n "$stereo_path" ] && [ "$stereo_path" != "NULL" ] &&
           [ -n "$imu_path" ] && [ "$imu_path" != "NULL" ] &&
           [ -e "$stereo_path" ] && [ -e "$imu_path" ]; then
            local stereo_resolved imu_resolved
            stereo_resolved="$(readlink -f "$stereo_path" 2>/dev/null || true)"
            imu_resolved="$(readlink -f "$imu_path" 2>/dev/null || true)"
            if [ -n "$stereo_resolved" ] && [ -n "$imu_resolved" ]; then
                if [ "$stereo_resolved" = "$last_stereo" ] && [ "$imu_resolved" = "$last_imu" ]; then
                    [ "$stable_since_ms" -ne 0 ] || stable_since_ms="$now_ms"
                    if [ $((now_ms - stable_since_ms)) -ge $((SYMLINK_STABLE_SEC * 1000)) ]; then
                        return 0
                    fi
                else
                    last_stereo="$stereo_resolved"
                    last_imu="$imu_resolved"
                    stable_since_ms="$now_ms"
                fi
            fi
        else
            last_stereo=""
            last_imu=""
            stable_since_ms=0
        fi
        sleep 0.1
    done

    fays_log "$side symlinks did not become stable before daemon start: stereo=${stereo_path:-<empty>} imu=${imu_path:-<empty>}"
    return 1
}

side_started_at() {
    local side="$1"
    if [ "$side" = "left" ]; then
        echo "$LEFT_STARTED_AT"
    else
        echo "$RIGHT_STARTED_AT"
    fi
}

side_in_health_grace() {
    local side="$1"
    local started_at
    started_at="$(side_started_at "$side")"
    [ "$started_at" -gt 0 ] && [ $((SECONDS - started_at)) -lt "$HEALTH_GRACE_SEC" ]
}

calibration_serial() {
    local calib_json="$1"
    python3 - "$calib_json" <<'PY'
import json
import sys

path = sys.argv[1]
try:
    with open(path, "r", encoding="utf-8") as f:
        root = json.load(f)
    if not isinstance(root, dict) or root.get("valid") is not True:
        sys.exit(1)
    device_info = root.get("device_info", {})
    serial = device_info.get("serial_number", "") if isinstance(device_info, dict) else ""
    if not serial:
        sys.exit(1)
    print(serial)
except Exception:
    sys.exit(1)
PY
}

runtime_status_path() {
    local side="$1"
    if [ "$side" = "left" ]; then
        echo "$LEFT_RUNTIME_STATUS_JSON"
    else
        echo "$RIGHT_RUNTIME_STATUS_JSON"
    fi
}

runtime_frame_fresh() {
    local side="$1"
    local status_json
    status_json="$(runtime_status_path "$side")"
    python3 - "$status_json" "$FRAME_STALE_SEC" <<'PY'
import json
import os
import sys
import time

path = sys.argv[1]
stale_sec = float(sys.argv[2])
try:
    with open(path, "r", encoding="utf-8") as f:
        root = json.load(f)
    last_ns = int(root.get("last_frame_system_ns", 0) or 0)
    if last_ns <= 0:
        sys.exit(1)
    age = (time.time_ns() - last_ns) / 1e9
    sys.exit(0 if age <= stale_sec else 1)
except Exception:
    sys.exit(1)
PY
}

check_side_working_state() {
    local side="$1"
    local config="$2"
    local fifo="$3"
    local calib_json="$4"
    local pid="$5"
    local quiet="${6:-false}"

    if ! check_side_device_paths "$side" "$config" "$fifo" true "$quiet"; then
        return 1
    fi
    if [ -z "$pid" ] || ! kill -0 "$pid" 2>/dev/null; then
        [ "$quiet" = "true" ] || fays_log "$side process not running"
        return 1
    fi
    if calibration_serial "$calib_json" >/dev/null 2>&1; then
        if runtime_frame_fresh "$side"; then
            return 0
        fi
        if side_in_health_grace "$side"; then
            return 0
        fi
        [ "$quiet" = "true" ] || fays_log "$side warmup frame stale after ${FRAME_STALE_SEC}s"
        return 1
    fi
    if side_in_health_grace "$side"; then
        return 0
    fi
    [ "$quiet" = "true" ] || fays_log "$side calibration/serial not ready after ${HEALTH_GRACE_SEC}s: $calib_json"
    return 1
}

side_pid() {
    local side="$1"
    if [ "$side" = "left" ]; then
        echo "$LEFT_PID"
    else
        echo "$RIGHT_PID"
    fi
}

side_stereo_video_index() {
    local config="$1"
    local stereo_path resolved node

    stereo_path="$(config_value "$config" stereo_dev_port)"
    [ -n "$stereo_path" ] && [ "$stereo_path" != "NULL" ] || return 1
    [ -e "$stereo_path" ] || return 1
    resolved="$(readlink -f "$stereo_path" 2>/dev/null || true)"
    [ -n "$resolved" ] || return 1
    node="$(basename "$resolved")"
    case "$node" in
        video*[!0-9]*|"")
            return 1
            ;;
        video*)
            printf '%s\n' "${node#video}"
            return 0
            ;;
        *)
            return 1
            ;;
    esac
}

stereo_start_order() {
    local left_index right_index

    left_index="$(side_stereo_video_index "$LEFT_CONFIG" 2>/dev/null || true)"
    right_index="$(side_stereo_video_index "$RIGHT_CONFIG" 2>/dev/null || true)"

    if [ -n "$left_index" ] && [ -n "$right_index" ]; then
        if [ "$right_index" -lt "$left_index" ]; then
            fays_log "stereo recorder start order: right,left (right video$right_index before left video$left_index)"
            printf '%s\n%s\n' right left
            return 0
        fi
        fays_log "stereo recorder start order: left,right (left video$left_index before right video$right_index)"
        printf '%s\n%s\n' left right
        return 0
    fi

    fays_log "stereo recorder start order fallback: left,right (left_video=${left_index:-unknown} right_video=${right_index:-unknown})"
    printf '%s\n%s\n' left right
}

wait_for_side_start_complete() {
    local side="$1"
    local config="$2"
    local fifo="$3"
    local calib_json="$4"
    local deadline_ms current_ms pid

    pid="$(side_pid "$side")"
    if [ -z "$pid" ] && [ ! -p "$fifo" ]; then
        fays_log "$side recorder SDK startup wait skipped: recorder not running"
        return 1
    fi

    deadline_ms=$(($(now_ms) + START_COMPLETE_TIMEOUT_SEC * 1000))
    fays_log "$side recorder waiting for SDK startup completion: timeout_s=$START_COMPLETE_TIMEOUT_SEC"
    while true; do
        pid="$(side_pid "$side")"
        if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null &&
           check_side_device_paths "$side" "$config" "$fifo" true true &&
           calibration_serial "$calib_json" >/dev/null 2>&1 &&
           runtime_frame_fresh "$side"; then
            fays_log "$side recorder SDK startup complete: pid=$pid fifo=$fifo"
            return 0
        fi

        current_ms="$(now_ms)"
        [ "$current_ms" -lt "$deadline_ms" ] || break
        sleep 0.1
    done

    pid="$(side_pid "$side")"
    fays_log "$side recorder SDK startup wait timed out: timeout_s=$START_COMPLETE_TIMEOUT_SEC pid=${pid:-none} fifo_ready=$([ -p "$fifo" ] && echo true || echo false) calib_ready=$(calibration_serial "$calib_json" >/dev/null 2>&1 && echo true || echo false) frame_fresh=$(runtime_frame_fresh "$side" && echo true || echo false)"
    return 1
}

dump_side_calibration() {
    local side="$1"
    local config="$2"
    local calib_json="$3"

    rm -f "$calib_json"
    if ! "$RUN_FAYS_RECORD" --config "$config" --calib-json "$calib_json" dump-calib-json; then
        fays_log "warning: failed to dump $side calibration before daemon start: $calib_json"
        return 1
    fi
    return 0
}

start_side_daemon() {
    local side="$1"
    local config="$2"
    local fifo="$3"
    local video_name="$4"
    local mcap_name="$5"
    local calib_json="$6"
    local runtime_status_json
    runtime_status_json="$(runtime_status_path "$side")"

    if [ ! -x "$RUN_FAYS_RECORD" ]; then
        echo "Fays record script is missing or not executable: $RUN_FAYS_RECORD" >&2
        return 1
    fi
    if [ ! -f "$config" ]; then
        echo "Fays config is missing for $side: $config" >&2
        return 1
    fi

    wait_for_side_symlinks_stable "$side" "$config" || return 1
    wait_kill_and_confirm_side_ports_free "$side" "$config" || return 1
    fays_log "$side recorder start requested: config=$config fifo=$fifo video=$video_name mcap=$mcap_name delay_s=$START_DELAY_SEC"
    sleep "$START_DELAY_SEC"
    rm -f "$fifo"
    rm -f "$runtime_status_json"
    setsid "$RUN_FAYS_RECORD" \
        --config "$config" \
        --control-fifo "$fifo" \
        --video-name "$video_name" \
        --mcap-name "$mcap_name" \
        --calib-json "$calib_json" \
        --status-json "$runtime_status_json" \
        daemon &
    local pid="$!"
    if ! wait_for_fifo "$fifo" 8; then
        log_fifo_startup_timeout "$side" "$fifo" "$pid" "$config" "$runtime_status_json"
        terminate_process_tree "$pid" "$side Fays recorder failed startup"
        return 1
    fi

    if [ "$side" = "left" ]; then
        LEFT_PID="$pid"
        LEFT_STARTED_AT="$SECONDS"
    else
        RIGHT_PID="$pid"
        RIGHT_STARTED_AT="$SECONDS"
    fi
    fays_log "$side recorder started: pid=$pid fifo=$fifo"
}

stop_side_daemon() {
    local side="$1"
    local fifo="$2"
    local pid="$3"
    local config=""

    if [ "$side" = "left" ]; then
        config="$LEFT_CONFIG"
    else
        config="$RIGHT_CONFIG"
    fi

    if [ -p "$fifo" ]; then
        fays_log "$side recorder stop requested via FIFO: pid=${pid:-none} fifo=$fifo"
        send_side_command "$fifo" exit >/dev/null 2>&1 || true
    fi
    if [ -n "$pid" ]; then
        if ! wait_for_process_exit "$pid" "$EXIT_GRACE_SEC"; then
            fays_log "$side recorder did not exit after EXIT command; escalating pid=$pid"
            terminate_process_tree "$pid" "$side Fays recorder"
        fi
    fi
    wait_kill_and_confirm_side_ports_free "$side" "$config" "$pid" || true
    rm -f "$fifo"
    rm -f "$(runtime_status_path "$side")"

    if [ "$side" = "left" ]; then
        LEFT_PID=""
        LEFT_STARTED_AT=0
    else
        RIGHT_PID=""
        RIGHT_STARTED_AT=0
    fi
}

maintain_side_daemon() {
    local side="$1"
    local config="$2"
    local fifo="$3"
    local video_name="$4"
    local mcap_name="$5"
    local calib_json="$6"
    local pid="$7"

    if ! check_side_device_paths "$side" "$config" "$fifo" false true; then
        if [ -n "$pid" ] || [ -p "$fifo" ]; then
            fays_log "$side devices disappeared; stopping recorder pid=${pid:-none}"
            stop_side_daemon "$side" "$fifo" "$pid"
        fi
        mark_session_error "$side Fays devices missing"
        return 0
    fi

    if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null && [ -p "$fifo" ] &&
       check_side_working_state "$side" "$config" "$fifo" "$calib_json" "$pid" true; then
        return 0
    fi

    if [ -n "$pid" ] || [ -p "$fifo" ]; then
        fays_log "$side recorder unhealthy; restarting pid=${pid:-none} recording=$RECORDING finalizing=$FINALIZE_PENDING"
        mark_session_error "$side Fays recorder unhealthy during session"
        stop_side_daemon "$side" "$fifo" "$pid"
    fi

    if start_side_daemon "$side" "$config" "$fifo" "$video_name" "$mcap_name" "$calib_json"; then
        fays_log "$side recorder maintain start ok"
        return 0
    fi

    if check_side_device_paths "$side" "$config" "$fifo" false true; then
        mark_session_error "$side stereo control FIFO startup timeout"
    else
        mark_session_error "failed to start $side Fays daemon"
    fi
    fays_log "$side recorder maintain start failed"
    stop_side_daemon "$side" "$fifo" "$pid"
    return 0
}

maintain_side_daemon_by_name() {
    local side="$1"
    local already_working=false

    if [ "$side" = "left" ]; then
        if [ -n "$LEFT_PID" ] && kill -0 "$LEFT_PID" 2>/dev/null && [ -p "$LEFT_FIFO" ] &&
           check_side_working_state left "$LEFT_CONFIG" "$LEFT_FIFO" "$LEFT_CALIB_JSON" "$LEFT_PID" true; then
            already_working=true
        fi
        maintain_side_daemon left "$LEFT_CONFIG" "$LEFT_FIFO" stereo_left.mkv fays_data_left.mcap "$LEFT_CALIB_JSON" "$LEFT_PID"
        [ "$already_working" = "true" ] || wait_for_side_start_complete left "$LEFT_CONFIG" "$LEFT_FIFO" "$LEFT_CALIB_JSON" || true
    else
        if [ -n "$RIGHT_PID" ] && kill -0 "$RIGHT_PID" 2>/dev/null && [ -p "$RIGHT_FIFO" ] &&
           check_side_working_state right "$RIGHT_CONFIG" "$RIGHT_FIFO" "$RIGHT_CALIB_JSON" "$RIGHT_PID" true; then
            already_working=true
        fi
        maintain_side_daemon right "$RIGHT_CONFIG" "$RIGHT_FIFO" stereo_right.mkv fays_data_right.mcap "$RIGHT_CALIB_JSON" "$RIGHT_PID"
        [ "$already_working" = "true" ] || wait_for_side_start_complete right "$RIGHT_CONFIG" "$RIGHT_FIFO" "$RIGHT_CALIB_JSON" || true
    fi
}

maintain_stereo_daemons_in_insert_order() {
    local side

    while IFS= read -r side; do
        [ -n "$side" ] || continue
        maintain_side_daemon_by_name "$side"
    done <<EOF
$(stereo_start_order)
EOF
}

handle_multi_device_health() {
    local left_serial right_serial

    if ! check_side_working_state left "$LEFT_CONFIG" "$LEFT_FIFO" "$LEFT_CALIB_JSON" "$LEFT_PID" true; then
        return 0
    fi
    if ! check_side_working_state right "$RIGHT_CONFIG" "$RIGHT_FIFO" "$RIGHT_CALIB_JSON" "$RIGHT_PID" true; then
        return 0
    fi

    left_serial="$(calibration_serial "$LEFT_CALIB_JSON" 2>/dev/null || true)"
    right_serial="$(calibration_serial "$RIGHT_CALIB_JSON" 2>/dev/null || true)"
    if [ -n "$left_serial" ] && [ "$left_serial" = "$right_serial" ]; then
        fays_log "serial collision detected; restarting both recorders: serial=$left_serial"
        mark_session_error "duplicate Fays serial detected during session: $left_serial"
        stop_side_daemon left "$LEFT_FIFO" "$LEFT_PID"
        stop_side_daemon right "$RIGHT_FIFO" "$RIGHT_PID"
    fi
}

clear_recovery_error_if_ready() {
    if [ "$RECORDING" = "true" ] || [ "$FINALIZE_PENDING" = "true" ]; then
        return 0
    fi
    if [ -n "$LEFT_PID" ] && kill -0 "$LEFT_PID" 2>/dev/null &&
       [ -n "$RIGHT_PID" ] && kill -0 "$RIGHT_PID" 2>/dev/null &&
       [ -p "$LEFT_FIFO" ] && [ -p "$RIGHT_FIFO" ]; then
        case "$LAST_FINALIZE_ERROR" in
            *"Fays devices missing"|*"Fays recorder unhealthy"*|*"duplicate Fays serial"*|*"failed to start "*|"")
                LAST_FINALIZE_ERROR=""
                ;;
        esac
    fi
}

start_daemons() {
    cleanup_stale_fays_recorders
    rm -f "$LEFT_CALIB_JSON" "$RIGHT_CALIB_JSON"
    rm -f "$LEFT_RUNTIME_STATUS_JSON" "$RIGHT_RUNTIME_STATUS_JSON"

    # Avoid creating extra short-lived SDK handles before the warmup daemons.
    # The vendor SDK can cross-bind or re-enumerate devices when calibration
    # probes are opened immediately before the long-lived recorder handles.
    # Each recorder daemon writes its own calibration JSON after its stable
    # handle is created.
    maintain_stereo_daemons_in_insert_order
}

send_side_command() {
    local fifo="$1"
    local command="$2"
    "$RUN_FAYS_RECORD" --control-fifo "$fifo" "$command"
}

send_side_start() {
    local fifo="$1"
    local episode_dir="$2"
    "$RUN_FAYS_RECORD" --control-fifo "$fifo" start "$episode_dir"
}

wait_for_file_nonempty() {
    local path="$1"
    local deadline=$((SECONDS + FINALIZE_TIMEOUT_SEC))
    while [ "$SECONDS" -le "$deadline" ]; do
        check_runtime_health_during_finalize
        [ -z "$LAST_FINALIZE_ERROR" ] || return 1
        [ -s "$path" ] && return 0
        sleep 0.1
    done
    return 1
}

wait_for_mcap_complete() {
    local path="$1"
    local deadline=$((SECONDS + FINALIZE_TIMEOUT_SEC))
    while [ "$SECONDS" -le "$deadline" ]; do
        check_runtime_health_during_finalize
        [ -z "$LAST_FINALIZE_ERROR" ] || return 1
        if [ -s "$path" ]; then
            python3 - "$path" <<'PY' && return 0
import os
import sys

path = sys.argv[1]
magic = b"\x89MCAP0\r\n"
try:
    size = os.path.getsize(path)
    if size < len(magic) * 2:
        sys.exit(1)
    with open(path, "rb") as fh:
        head = fh.read(len(magic))
        fh.seek(-len(magic), os.SEEK_END)
        tail = fh.read(len(magic))
    sys.exit(0 if head == magic and tail == magic else 1)
except Exception:
    sys.exit(1)
PY
        fi
        sleep 0.1
    done
    return 1
}

check_runtime_health_during_finalize() {
    if [ -n "$LAST_FINALIZE_ERROR" ]; then
        return 0
    fi
    if ! runtime_frame_fresh left; then
        LAST_FINALIZE_ERROR="left Fays warmup frame stale during session"
        fays_log "finalize health error: $LAST_FINALIZE_ERROR episode=$ACTIVE_EPISODE_DIR"
        write_status
        return 0
    fi
    if ! runtime_frame_fresh right; then
        LAST_FINALIZE_ERROR="right Fays warmup frame stale during session"
        fays_log "finalize health error: $LAST_FINALIZE_ERROR episode=$ACTIVE_EPISODE_DIR"
        write_status
        return 0
    fi
}

build_last_session_json() {
    local episode_dir="$1"
    local start_us="$2"
    local stop_us="$3"
    python3 - "$episode_dir" "$start_us" "$stop_us" <<'PY'
import json
import sys

episode_dir = sys.argv[1]
start_us = int(sys.argv[2])
stop_us = int(sys.argv[3])
duration_us = max(0, stop_us - start_us)

def camera_info():
    return {
        "record_time_offset_us": start_us,
        "first_written_frame_pts_us": 0,
        "first_written_frame_system_time_us": start_us,
        "last_written_frame_pts_us": duration_us,
        "last_written_frame_system_time_us": stop_us,
    }

print(json.dumps({
    "episode_dir": episode_dir,
    "start_system_time_us": start_us,
    "stop_system_time_us": stop_us,
    "cameras": {
        "left_stereo": camera_info(),
        "right_stereo": camera_info(),
    },
}))
PY
}

handle_start() {
    local episode_dir="$1"
    local start_us="$2"
    local missing_fifos=()

    LAST_FINALIZE_ERROR=""
    LAST_SESSION_JSON="{}"
    FINALIZE_PENDING=false
    fays_log "session start requested: episode=$episode_dir start_us=$start_us"
    if [ ! -p "$LEFT_FIFO" ] || [ ! -p "$RIGHT_FIFO" ]; then
        [ -p "$LEFT_FIFO" ] || missing_fifos+=("left")
        [ -p "$RIGHT_FIFO" ] || missing_fifos+=("right")
        if [ "${#missing_fifos[@]}" -eq 1 ] &&
           check_side_device_paths "${missing_fifos[0]}" \
               "$([ "${missing_fifos[0]}" = "left" ] && echo "$LEFT_CONFIG" || echo "$RIGHT_CONFIG")" \
               "$([ "${missing_fifos[0]}" = "left" ] && echo "$LEFT_FIFO" || echo "$RIGHT_FIFO")" \
               false true; then
            LAST_FINALIZE_ERROR="${missing_fifos[0]} FIFO missing while device paths are online"
        else
            LAST_FINALIZE_ERROR="cannot start stereo session before both Fays recorders are ready"
        fi
        fays_log "session start failed: $LAST_FINALIZE_ERROR"
        write_status
        return 1
    fi
    send_side_start "$LEFT_FIFO" "$episode_dir" || {
        LAST_FINALIZE_ERROR="failed to start left Fays session"
        fays_log "session start failed: $LAST_FINALIZE_ERROR"
        write_status
        return 1
    }
    send_side_start "$RIGHT_FIFO" "$episode_dir" || {
        LAST_FINALIZE_ERROR="failed to start right Fays session"
        fays_log "session start failed: $LAST_FINALIZE_ERROR"
        write_status
        return 1
    }
    ACTIVE_EPISODE_DIR="$episode_dir"
    ACTIVE_START_US="$start_us"
    ACTIVE_STOP_US=0
    RECORDING=true
    fays_log "session started: episode=$episode_dir"
    write_status
}

handle_stop() {
    local episode_dir="$1"
    local stop_us="$2"

    if [ -z "$ACTIVE_EPISODE_DIR" ] || [ "$episode_dir" != "$ACTIVE_EPISODE_DIR" ]; then
        return 0
    fi

    RECORDING=false
    FINALIZE_PENDING=true
    ACTIVE_STOP_US="$stop_us"
    fays_log "session stop requested: episode=$episode_dir stop_us=$stop_us"
    write_status
    check_runtime_health_during_finalize
    [ -z "$LAST_FINALIZE_ERROR" ] || write_status

    if [ -z "$LAST_FINALIZE_ERROR" ]; then
        send_side_command "$LEFT_FIFO" stop || {
            LAST_FINALIZE_ERROR="failed to stop left Fays recorder"
            fays_log "session stop error: $LAST_FINALIZE_ERROR"
        }
    else
        send_side_command "$LEFT_FIFO" stop >/dev/null 2>&1 || true
    fi
    if [ -z "$LAST_FINALIZE_ERROR" ]; then
        send_side_command "$RIGHT_FIFO" stop || {
            LAST_FINALIZE_ERROR="failed to stop right Fays recorder"
            fays_log "session stop error: $LAST_FINALIZE_ERROR"
        }
    else
        send_side_command "$RIGHT_FIFO" stop >/dev/null 2>&1 || true
    fi

    if [ -z "$LAST_FINALIZE_ERROR" ]; then
        wait_for_file_nonempty "$episode_dir/stereo_left.mkv" || {
            LAST_FINALIZE_ERROR="stereo_left.mkv missing or empty"
            fays_log "session finalize error: $LAST_FINALIZE_ERROR"
        }
    fi
    if [ -z "$LAST_FINALIZE_ERROR" ]; then
        wait_for_file_nonempty "$episode_dir/stereo_right.mkv" || {
            LAST_FINALIZE_ERROR="stereo_right.mkv missing or empty"
            fays_log "session finalize error: $LAST_FINALIZE_ERROR"
        }
    fi
    if [ -z "$LAST_FINALIZE_ERROR" ]; then
        wait_for_mcap_complete "$episode_dir/fays_data_left.mcap" || {
            LAST_FINALIZE_ERROR="fays_data_left.mcap missing or incomplete"
            fays_log "session finalize error: $LAST_FINALIZE_ERROR"
        }
    fi
    if [ -z "$LAST_FINALIZE_ERROR" ]; then
        wait_for_mcap_complete "$episode_dir/fays_data_right.mcap" || {
            LAST_FINALIZE_ERROR="fays_data_right.mcap missing or incomplete"
            fays_log "session finalize error: $LAST_FINALIZE_ERROR"
        }
    fi

    if [ -z "$LAST_FINALIZE_ERROR" ]; then
        LAST_SESSION_JSON="$(build_last_session_json "$episode_dir" "$ACTIVE_START_US" "$stop_us")"
        LAST_FINALIZED_EPISODE_DIR="$episode_dir"
        fays_log "session finalized: episode=$episode_dir"
    else
        LAST_SESSION_JSON="{}"
        LAST_FINALIZED_EPISODE_DIR="$episode_dir"
        fays_log "session finalized with error: episode=$episode_dir error=$LAST_FINALIZE_ERROR"
    fi

    FINALIZE_PENDING=false
    ACTIVE_EPISODE_DIR=""
    ACTIVE_START_US=0
    ACTIVE_STOP_US=0
    write_status
}

handle_control_command() {
    local command_line="$1"
    local command command_seq command_episode command_time_us

    [ -n "$command_line" ] || return 0
    IFS='|' read -r command command_seq command_episode command_time_us <<EOF
$command_line
EOF

    case "$command" in
        START|STOP)
            ;;
        EXIT)
            exit 0
            ;;
        *)
            echo "Unknown stereo control command: $command_line" >&2
            return 0
            ;;
    esac

    case "${command_seq:-}" in
        ''|*[!0-9]*)
            echo "Invalid stereo control command seq: $command_line" >&2
            return 0
            ;;
    esac
    if [ "$command_seq" -le "$LAST_COMMAND_SEQ" ]; then
        return 0
    fi
    LAST_COMMAND_SEQ="$command_seq"

    case "${command_time_us:-}" in
        ''|-)
            command_time_us=0
            ;;
        *[!0-9-]*)
            echo "Invalid stereo control timestamp: $command_line" >&2
            return 0
            ;;
    esac

    if [ "$command" = "START" ]; then
        handle_start "$command_episode" "$command_time_us"
    else
        handle_stop "$command_episode" "$command_time_us"
    fi
}

drain_control_fifo() {
    local timeout_sec="${1:-0.001}"
    local command_line=""
    while IFS= read -r -t "$timeout_sec" command_line <&9; do
        handle_control_command "$command_line"
        timeout_sec=0.001
    done
}

cleanup() {
    set +e
    if [ "$CLEANED_UP" = "true" ]; then
        return 0
    fi
    CLEANED_UP=true
    exec 9>&- 2>/dev/null || true
    rm -f "$CONTROL_FIFO"
    stop_side_daemon left "$LEFT_FIFO" "$LEFT_PID"
    stop_side_daemon right "$RIGHT_FIFO" "$RIGHT_PID"
    cleanup_stale_fays_recorders
}

trap cleanup EXIT
trap 'cleanup; exit 0' INT TERM

ensure_control_fifo
exec 9<>"$CONTROL_FIFO"

start_daemons
clear_recovery_error_if_ready
write_status

while true; do
    drain_control_fifo 0.001
    current_ms="$(now_ms)"
    if [ "$LAST_HEALTH_CHECK_MS" -eq 0 ] ||
       [ $((current_ms - LAST_HEALTH_CHECK_MS)) -ge $((HEALTH_POLL_INTERVAL_SEC * 1000)) ]; then
        LAST_HEALTH_CHECK_MS="$current_ms"
        maintain_stereo_daemons_in_insert_order
        handle_multi_device_health
        clear_recovery_error_if_ready

        write_status
    fi
    drain_control_fifo "$POLL_INTERVAL_SEC"
done
