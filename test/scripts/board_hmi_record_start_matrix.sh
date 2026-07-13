#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/board_test_common.sh"

usage() {
    cat <<'EOF'
Usage: board_hmi_record_start_matrix.sh [options]

Options:
  --mode combo|timing|led-only
                           Test mode (required)
  --output-dir DIR         Result directory (default: /dev/shm/ugripper_hmi_record_start_<time>)
  --cycles N               Cycles per combo/led-only test or per timing offset (default: 50)
  --pre-sec N              Pre-start LED dwell seconds (default: 2)
  --record-sec N           Recording LED observation seconds (default: 7)
  --post-sec N             Post-cycle dwell seconds (default: 1)
  --offsets-ms CSV         Sensor start offsets for timing mode
                           (default: -1000,-500,-200,-50,0,50,200,500,1000)
  --hmi-inactive-ms N      HMI valid-frame timeout threshold (default: 5500)
  --service-name NAME      Service stopped during direct access (default: ugripper.service)
  --ugripper-root DIR      Installed UGripper root (default: /opt/ugripper)
  --sensor-recorder-bin PATH
                           Override SensorRecorder binary
  --python-bin PATH        Python used for the monitor (default: UGripper venv)
  --help                   Show this help

All modes keep both HMI ports open once, use the runtime's real 1Hz state query
and 250ms LED resend policy, and send no beep commands.

combo:
  left Ready + right tactile-warning -> both Recording while SensorRecorder is
  launched at the same boundary. SensorRecorder is stopped after each cycle.

timing:
  Same as combo, but scans SensorRecorder launch relative to the Recording LED
  transition. Negative offsets open/configure encoders before the LED switch.

led-only:
  SensorRecorder stays open for the whole test. Only the real left Ready/right
  tactile-warning -> both Recording LED transition is repeated.
EOF
}

MODE=""
OUTPUT_DIR="/dev/shm/ugripper_hmi_record_start_$(timestamp_slug)"
CYCLES=50
PRE_SEC=2
RECORD_SEC=7
POST_SEC=1
OFFSETS_MS="-1000,-500,-200,-50,0,50,200,500,1000"
HMI_INACTIVE_MS=5500
SERVICE_NAME="ugripper.service"
UGRIPPER_ROOT="$(default_ugripper_root)"
SENSOR_RECORDER_BIN_OVERRIDE=""
PYTHON_BIN_OVERRIDE=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --mode) MODE="$2"; shift 2 ;;
        --output-dir) OUTPUT_DIR="$2"; shift 2 ;;
        --cycles) CYCLES="$2"; shift 2 ;;
        --pre-sec) PRE_SEC="$2"; shift 2 ;;
        --record-sec) RECORD_SEC="$2"; shift 2 ;;
        --post-sec) POST_SEC="$2"; shift 2 ;;
        --offsets-ms) OFFSETS_MS="$2"; shift 2 ;;
        --hmi-inactive-ms) HMI_INACTIVE_MS="$2"; shift 2 ;;
        --service-name) SERVICE_NAME="$2"; shift 2 ;;
        --ugripper-root) UGRIPPER_ROOT="$2"; shift 2 ;;
        --sensor-recorder-bin) SENSOR_RECORDER_BIN_OVERRIDE="$2"; shift 2 ;;
        --python-bin) PYTHON_BIN_OVERRIDE="$2"; shift 2 ;;
        --help|-h) usage; exit 0 ;;
        *) echo "unknown argument: $1" >&2; usage; exit 2 ;;
    esac
done

case "${MODE}" in combo|timing|led-only) ;; *) echo "--mode must be combo, timing, or led-only" >&2; exit 2 ;; esac
for value_name in CYCLES PRE_SEC RECORD_SEC POST_SEC HMI_INACTIVE_MS; do
    value="${!value_name}"
    [[ "${value}" =~ ^[1-9][0-9]*$ ]] || { echo "${value_name} must be a positive integer" >&2; exit 2; }
done

SENSOR_RECORDER_BIN="$(resolve_executable "${SENSOR_RECORDER_BIN_OVERRIDE}" \
    "${UGRIPPER_ROOT}/bin/SensorRecorder/SensorRecorder")"
PYTHON_BIN="$(resolve_executable "${PYTHON_BIN_OVERRIDE}" \
    "${UGRIPPER_ROOT}/.venv/bin/python3" python3)"

mkdir -p "${OUTPUT_DIR}/snapshots" "${OUTPUT_DIR}/sensor_cycles"
START_TIME="$(date '+%Y-%m-%d %H:%M:%S')"
SERVICE_WAS_ACTIVE=0
SERVICE_RESTORED=0
MATRIX_PID=""

collect_snapshot() {
    local name="$1"
    local dir="${OUTPUT_DIR}/snapshots/${name}"
    mkdir -p "${dir}"
    date -Ins > "${dir}/time.txt"
    systemctl status "${SERVICE_NAME}" --no-pager -l > "${dir}/service.txt" 2>&1 || true
    ps -eo pid,stat,comm,args > "${dir}/processes.txt" 2>&1 || true
    ls -l /dev/left_gripper /dev/left_encoder /dev/right_gripper /dev/right_encoder > "${dir}/links.txt" 2>&1 || true
    lsusb -t > "${dir}/lsusb-tree.txt" 2>&1 || true
    journalctl -k --since "${START_TIME}" --no-pager -o short-precise -l > "${dir}/kernel.log" 2>&1 || true
    command -v hws >/dev/null 2>&1 && hws > "${dir}/hws.txt" 2>&1 || true
}

restore_service() {
    if [[ -n "${MATRIX_PID}" ]] && kill -0 "${MATRIX_PID}" 2>/dev/null; then
        kill -INT "${MATRIX_PID}" 2>/dev/null || true
        for _ in $(seq 1 100); do kill -0 "${MATRIX_PID}" 2>/dev/null || break; sleep 0.1; done
        kill -TERM "${MATRIX_PID}" 2>/dev/null || true
        wait "${MATRIX_PID}" 2>/dev/null || true
    fi
    MATRIX_PID=""
    if [[ "${SERVICE_WAS_ACTIVE}" == "1" && "${SERVICE_RESTORED}" != "1" ]]; then
        systemctl start "${SERVICE_NAME}" || true
        for _ in $(seq 1 30); do systemctl is-active --quiet "${SERVICE_NAME}" && break; sleep 1; done
        SERVICE_RESTORED=1
    fi
}
trap restore_service EXIT INT TERM

print_section "HMI Record-start Matrix Configuration"
echo "mode=${MODE}"
echo "output_dir=${OUTPUT_DIR}"
echo "cycles=${CYCLES}"
echo "pre_sec=${PRE_SEC}"
echo "record_sec=${RECORD_SEC}"
echo "post_sec=${POST_SEC}"
echo "offsets_ms=${OFFSETS_MS}"

collect_snapshot before
systemctl is-active --quiet "${SERVICE_NAME}" && SERVICE_WAS_ACTIVE=1
systemctl stop "${SERVICE_NAME}"
for _ in $(seq 1 30); do systemctl is-active --quiet "${SERVICE_NAME}" || break; sleep 1; done
systemctl is-active --quiet "${SERVICE_NAME}" && { echo "service is still active" >&2; exit 1; }
for path in /dev/left_gripper /dev/left_encoder /dev/right_gripper /dev/right_encoder; do
    [[ -e "${path}" ]] || { echo "missing device: ${path}" >&2; exit 1; }
done

set +e
"${PYTHON_BIN}" "${SCRIPT_DIR}/board_hmi_record_start_matrix.py" \
    "${MODE}" "${OUTPUT_DIR}" "${CYCLES}" "${PRE_SEC}" "${RECORD_SEC}" \
    "${POST_SEC}" "${OFFSETS_MS}" "${HMI_INACTIVE_MS}" "${SENSOR_RECORDER_BIN}" \
    > "${OUTPUT_DIR}/matrix.log" 2>&1 &
MATRIX_PID=$!
wait "${MATRIX_PID}"
MATRIX_EXIT=$?
MATRIX_PID=""
set -e

collect_snapshot after_direct_test
restore_service
collect_snapshot after_service_restore
journalctl -u "${SERVICE_NAME}" --since "${START_TIME}" --no-pager -o short-precise -l > "${OUTPUT_DIR}/service.log" 2>&1 || true

cat > "${OUTPUT_DIR}/summary.txt" <<EOF
start_time=${START_TIME}
end_time=$(date '+%Y-%m-%d %H:%M:%S')
mode=${MODE}
matrix_exit=${MATRIX_EXIT}
service_was_active=${SERVICE_WAS_ACTIVE}
service_restored=${SERVICE_RESTORED}
EOF

print_section "Summary"
cat "${OUTPUT_DIR}/summary.txt"
cat "${OUTPUT_DIR}/cycle_report.tsv" 2>/dev/null || true
cat "${OUTPUT_DIR}/hmi_report.tsv" 2>/dev/null || true
echo "result_dir=${OUTPUT_DIR}"
exit "${MATRIX_EXIT}"
