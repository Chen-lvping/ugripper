#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$(dirname "$SCRIPT_DIR")"
RUN_FAYS_RECORD="$SCRIPT_DIR/run_fays_record.sh"

CONTROL_FILE="/tmp/umi_stereo_camera_control.json"
STATUS_FILE="/tmp/umi_stereo_camera_status.json"
LEFT_CONFIG="$BUILD_DIR/config/fays_vikit_left.yaml"
RIGHT_CONFIG="$BUILD_DIR/config/fays_vikit_right.yaml"
LEFT_FIFO="/tmp/umi_left_fays_cmd"
RIGHT_FIFO="/tmp/umi_right_fays_cmd"
LEFT_CALIB_JSON="/tmp/umi_left_fays_calibration.json"
RIGHT_CALIB_JSON="/tmp/umi_right_fays_calibration.json"
POLL_INTERVAL_SEC="${FAYS_STEREO_POLL_INTERVAL_SEC:-0.2}"
FINALIZE_TIMEOUT_SEC="${FAYS_STEREO_FINALIZE_TIMEOUT_SEC:-10}"

LEFT_PID=""
RIGHT_PID=""
LAST_COMMAND_SEQ=0
ACTIVE_EPISODE_DIR=""
ACTIVE_START_US=0
ACTIVE_STOP_US=0
LAST_FINALIZED_EPISODE_DIR=""
LAST_FINALIZE_ERROR=""
LAST_SESSION_JSON="{}"
RECORDING=false
FINALIZE_PENDING=false

usage() {
    cat <<'EOF'
Usage:
  run_fays_stereo_daemon.sh [options]

Options:
  --control-file FILE
  --status-file FILE
  --left-config FILE
  --right-config FILE
  --left-fifo FIFO
  --right-fifo FIFO
  --left-calib-json FILE
  --right-calib-json FILE
EOF
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --control-file)
            CONTROL_FILE="$2"
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

json_read_command() {
    python3 - "$CONTROL_FILE" <<'PY'
import json
import sys

path = sys.argv[1]
try:
    with open(path, "r", encoding="utf-8") as f:
        root = json.load(f)
except Exception:
    sys.exit(1)

print(
    "{}\t{}\t{}\t{}\t{}".format(
        int(root.get("command_seq", 0)),
        1 if root.get("recording", False) else 0,
        str(root.get("episode_dir", "")),
        int(root.get("start_system_time_us", 0)),
        int(root.get("stop_system_time_us", 0)),
    )
)
PY
}

write_status() {
    local left_ready=false
    local right_ready=false
    local ready=false
    local service_state="warming"

    if [ -n "$LEFT_PID" ] && kill -0 "$LEFT_PID" 2>/dev/null && [ -p "$LEFT_FIFO" ]; then
        left_ready=true
    fi
    if [ -n "$RIGHT_PID" ] && kill -0 "$RIGHT_PID" 2>/dev/null && [ -p "$RIGHT_FIFO" ]; then
        right_ready=true
    fi
    if [ "$left_ready" = true ] && [ "$right_ready" = true ]; then
        ready=true
    fi
    if [ "$FINALIZE_PENDING" = true ]; then
        service_state="finalizing"
    elif [ "$RECORDING" = true ]; then
        service_state="recording"
    elif [ "$ready" = true ]; then
        service_state="ready"
    fi

    STATUS_FILE="$STATUS_FILE" \
    RECORDING="$RECORDING" \
    FINALIZE_PENDING="$FINALIZE_PENDING" \
    ACTIVE_EPISODE_DIR="$ACTIVE_EPISODE_DIR" \
    LAST_FINALIZED_EPISODE_DIR="$LAST_FINALIZED_EPISODE_DIR" \
    LAST_FINALIZE_ERROR="$LAST_FINALIZE_ERROR" \
    LAST_SESSION_JSON="$LAST_SESSION_JSON" \
    READY="$ready" \
    SERVICE_STATE="$service_state" \
    LEFT_READY="$left_ready" \
    RIGHT_READY="$right_ready" \
    LEFT_CONFIG="$LEFT_CONFIG" \
    RIGHT_CONFIG="$RIGHT_CONFIG" \
    LEFT_CALIB_JSON="$LEFT_CALIB_JSON" \
    RIGHT_CALIB_JSON="$RIGHT_CALIB_JSON" \
    python3 - <<'PY'
import json
import os
import tempfile

def as_bool(name):
    return os.environ.get(name, "false") == "true"

def calibration_state(path):
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
    if isinstance(data, dict) and data.get("valid") is True:
        root["valid"] = True
        root["status"] = "ready"
        root["schema"] = data.get("schema", "")
        root["sdk_version"] = data.get("device_info", {}).get("sdk_version", "")
    else:
        root["status"] = "invalid_payload"
    return root

status_file = os.environ["STATUS_FILE"]
try:
    last_session = json.loads(os.environ.get("LAST_SESSION_JSON", "{}") or "{}")
except Exception:
    last_session = {}

root = {
    "recording": as_bool("RECORDING"),
    "finalize_pending": as_bool("FINALIZE_PENDING"),
    "active_episode_dir": os.environ.get("ACTIVE_EPISODE_DIR", ""),
    "last_finalized_episode_dir": os.environ.get("LAST_FINALIZED_EPISODE_DIR", ""),
    "last_finalize_error": os.environ.get("LAST_FINALIZE_ERROR", ""),
    "last_session": last_session,
    "ready": as_bool("READY"),
    "service_state": os.environ.get("SERVICE_STATE", "warming"),
    "cameras": {
        "left_stereo": {
            "state": os.environ.get("SERVICE_STATE", "warming"),
            "device": os.environ.get("LEFT_CONFIG", ""),
            "ready": as_bool("LEFT_READY"),
            "session_recording": as_bool("RECORDING"),
            "calibration": calibration_state(os.environ.get("LEFT_CALIB_JSON", "")),
        },
        "right_stereo": {
            "state": os.environ.get("SERVICE_STATE", "warming"),
            "device": os.environ.get("RIGHT_CONFIG", ""),
            "ready": as_bool("RIGHT_READY"),
            "session_recording": as_bool("RECORDING"),
            "calibration": calibration_state(os.environ.get("RIGHT_CALIB_JSON", "")),
        },
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
    local deadline=$((SECONDS + 8))
    while [ "$SECONDS" -lt "$deadline" ]; do
        [ -p "$fifo" ] && return 0
        sleep 0.1
    done
    return 1
}

dump_side_calibration() {
    local side="$1"
    local config="$2"
    local calib_json="$3"

    rm -f "$calib_json"
    if ! "$RUN_FAYS_RECORD" --config "$config" --calib-json "$calib_json" dump-calib-json; then
        echo "Warning: failed to dump $side Fays calibration before daemon start: $calib_json" >&2
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

    if [ ! -x "$RUN_FAYS_RECORD" ]; then
        echo "Fays record script is missing or not executable: $RUN_FAYS_RECORD" >&2
        return 1
    fi
    if [ ! -f "$config" ]; then
        echo "Fays config is missing for $side: $config" >&2
        return 1
    fi

    rm -f "$fifo"
    "$RUN_FAYS_RECORD" \
        --config "$config" \
        --control-fifo "$fifo" \
        --video-name "$video_name" \
        --mcap-name "$mcap_name" \
        --calib-json "$calib_json" \
        daemon &
    local pid="$!"
    if ! wait_for_fifo "$fifo"; then
        echo "Timed out waiting for $side Fays FIFO: $fifo" >&2
        kill "$pid" 2>/dev/null || true
        wait "$pid" 2>/dev/null || true
        return 1
    fi

    if [ "$side" = "left" ]; then
        LEFT_PID="$pid"
    else
        RIGHT_PID="$pid"
    fi
}

start_daemons() {
    rm -f "$LEFT_CALIB_JSON" "$RIGHT_CALIB_JSON"
    dump_side_calibration left "$LEFT_CONFIG" "$LEFT_CALIB_JSON" || true
    dump_side_calibration right "$RIGHT_CONFIG" "$RIGHT_CALIB_JSON" || true
    start_side_daemon left "$LEFT_CONFIG" "$LEFT_FIFO" left_stereo.mkv left_fays_data.mcap "$LEFT_CALIB_JSON"
    start_side_daemon right "$RIGHT_CONFIG" "$RIGHT_FIFO" right_stereo.mkv right_fays_data.mcap "$RIGHT_CALIB_JSON"
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
        [ -s "$path" ] && return 0
        sleep 0.1
    done
    return 1
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

    LAST_FINALIZE_ERROR=""
    LAST_SESSION_JSON="{}"
    FINALIZE_PENDING=false
    send_side_start "$LEFT_FIFO" "$episode_dir"
    send_side_start "$RIGHT_FIFO" "$episode_dir"
    ACTIVE_EPISODE_DIR="$episode_dir"
    ACTIVE_START_US="$start_us"
    ACTIVE_STOP_US=0
    RECORDING=true
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
    write_status

    send_side_command "$LEFT_FIFO" stop || LAST_FINALIZE_ERROR="failed to stop left Fays recorder"
    send_side_command "$RIGHT_FIFO" stop || LAST_FINALIZE_ERROR="failed to stop right Fays recorder"

    if [ -z "$LAST_FINALIZE_ERROR" ]; then
        wait_for_file_nonempty "$episode_dir/left_stereo.mkv" || LAST_FINALIZE_ERROR="left_stereo.mkv missing or empty"
    fi
    if [ -z "$LAST_FINALIZE_ERROR" ]; then
        wait_for_file_nonempty "$episode_dir/right_stereo.mkv" || LAST_FINALIZE_ERROR="right_stereo.mkv missing or empty"
    fi
    if [ -z "$LAST_FINALIZE_ERROR" ]; then
        wait_for_file_nonempty "$episode_dir/left_fays_data.mcap" || LAST_FINALIZE_ERROR="left_fays_data.mcap missing or empty"
    fi
    if [ -z "$LAST_FINALIZE_ERROR" ]; then
        wait_for_file_nonempty "$episode_dir/right_fays_data.mcap" || LAST_FINALIZE_ERROR="right_fays_data.mcap missing or empty"
    fi

    if [ -z "$LAST_FINALIZE_ERROR" ]; then
        LAST_SESSION_JSON="$(build_last_session_json "$episode_dir" "$ACTIVE_START_US" "$stop_us")"
        LAST_FINALIZED_EPISODE_DIR="$episode_dir"
    fi

    FINALIZE_PENDING=false
    ACTIVE_EPISODE_DIR=""
    ACTIVE_START_US=0
    ACTIVE_STOP_US=0
}

cleanup() {
    set +e
    if [ -p "$LEFT_FIFO" ]; then
        send_side_command "$LEFT_FIFO" exit >/dev/null 2>&1 || true
    fi
    if [ -p "$RIGHT_FIFO" ]; then
        send_side_command "$RIGHT_FIFO" exit >/dev/null 2>&1 || true
    fi
    [ -n "$LEFT_PID" ] && wait "$LEFT_PID" 2>/dev/null || true
    [ -n "$RIGHT_PID" ] && wait "$RIGHT_PID" 2>/dev/null || true
}

trap cleanup EXIT
trap 'cleanup; exit 0' INT TERM

start_daemons
write_status

while true; do
    if [ -n "$LEFT_PID" ] && ! kill -0 "$LEFT_PID" 2>/dev/null; then
        LAST_FINALIZE_ERROR="left Fays daemon exited"
        write_status
        exit 1
    fi
    if [ -n "$RIGHT_PID" ] && ! kill -0 "$RIGHT_PID" 2>/dev/null; then
        LAST_FINALIZE_ERROR="right Fays daemon exited"
        write_status
        exit 1
    fi

    if command_line="$(json_read_command 2>/dev/null)"; then
        IFS="$(printf '\t')" read -r command_seq command_recording command_episode command_start_us command_stop_us <<EOF
$command_line
EOF
        if [ "${command_seq:-0}" -ne "$LAST_COMMAND_SEQ" ]; then
            LAST_COMMAND_SEQ="$command_seq"
            if [ "$command_recording" = "1" ]; then
                handle_start "$command_episode" "$command_start_us"
            else
                handle_stop "$command_episode" "$command_stop_us"
            fi
        fi
    fi

    write_status
    sleep "$POLL_INTERVAL_SEC"
done
