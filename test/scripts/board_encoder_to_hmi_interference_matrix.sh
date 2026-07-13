#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "${SCRIPT_DIR}/board_test_common.sh"

usage() {
    cat <<'EOF'
Usage: board_encoder_to_hmi_interference_matrix.sh [options]

Options:
  --output-dir DIR        Result directory (default: /dev/shm/ugripper_encoder_hmi_matrix_<time>)
  --side left|right       Encoder UART used as stimulus (default: left)
  --baseline-sec N        HMI-only baseline duration (default: 15)
  --quick-repeats N       Encoder open/config/flush repetitions (default: 2000)
  --query-cycles N        Encoder request session count (default: 100)
  --requests-per-cycle N  Position requests per session (default: 100)
  --hmi-inactive-ms N     HMI reply-age failure threshold (default: 2000)
  --service-name NAME     Service stopped during the matrix (default: ugripper.service)
  --ugripper-root DIR     Installed UGripper root (default: /opt/ugripper)
  --gripper-hmi-bin PATH  Override GripperHmiTool binary
  --python-bin PATH       Python binary (default: UGripper venv)
  --help                  Show this help

Both HMI ports remain connected and keep sending state requests during every
phase. The selected encoder UART is then stressed with open/close, termios,
flush, and real 1Mbps position requests. Same-side HMI reply age is compared
with the opposite HMI to determine whether encoder UART lifecycle activity can
trigger HMI no-response on the same CH9344.
EOF
}

OUTPUT_DIR="/dev/shm/ugripper_encoder_hmi_matrix_$(timestamp_slug)"
SIDE="left"
BASELINE_SEC=15
QUICK_REPEATS=2000
QUERY_CYCLES=100
REQUESTS_PER_CYCLE=100
HMI_INACTIVE_MS=2000
SERVICE_NAME="ugripper.service"
UGRIPPER_ROOT="$(default_ugripper_root)"
GRIPPER_HMI_BIN_OVERRIDE=""
PYTHON_BIN_OVERRIDE=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --output-dir) OUTPUT_DIR="$2"; shift 2 ;;
        --side) SIDE="$2"; shift 2 ;;
        --baseline-sec) BASELINE_SEC="$2"; shift 2 ;;
        --quick-repeats) QUICK_REPEATS="$2"; shift 2 ;;
        --query-cycles) QUERY_CYCLES="$2"; shift 2 ;;
        --requests-per-cycle) REQUESTS_PER_CYCLE="$2"; shift 2 ;;
        --hmi-inactive-ms) HMI_INACTIVE_MS="$2"; shift 2 ;;
        --service-name) SERVICE_NAME="$2"; shift 2 ;;
        --ugripper-root) UGRIPPER_ROOT="$2"; shift 2 ;;
        --gripper-hmi-bin) GRIPPER_HMI_BIN_OVERRIDE="$2"; shift 2 ;;
        --python-bin) PYTHON_BIN_OVERRIDE="$2"; shift 2 ;;
        --help|-h) usage; exit 0 ;;
        *) echo "unknown argument: $1" >&2; usage; exit 2 ;;
    esac
done

case "${SIDE}" in left|right) ;; *) echo "side must be left or right" >&2; exit 2 ;; esac
for value_name in BASELINE_SEC QUICK_REPEATS QUERY_CYCLES REQUESTS_PER_CYCLE HMI_INACTIVE_MS; do
    value="${!value_name}"
    [[ "${value}" =~ ^[1-9][0-9]*$ ]] || { echo "${value_name} must be a positive integer" >&2; exit 2; }
done

GRIPPER_HMI_BIN="$(resolve_executable "${GRIPPER_HMI_BIN_OVERRIDE}" \
    "${UGRIPPER_ROOT}/bin/GripperHmiTool/GripperHmiTool")"
PYTHON_BIN="$(resolve_executable "${PYTHON_BIN_OVERRIDE}" \
    "${UGRIPPER_ROOT}/.venv/bin/python3" \
    "/usr/bin/python3")"
ENCODER_PORT="/dev/${SIDE}_encoder"

mkdir -p "${OUTPUT_DIR}/phases" "${OUTPUT_DIR}/snapshots"
MATRIX_TSV="${OUTPUT_DIR}/matrix.tsv"
printf 'phase\tstimulus_exit\tleft_max_age_ms\tleft_inactive_samples\tleft_io_failures\tright_max_age_ms\tright_inactive_samples\tright_io_failures\tresult\n' > "${MATRIX_TSV}"
START_TIME="$(date '+%Y-%m-%d %H:%M:%S')"
SERVICE_WAS_ACTIVE=0
SERVICE_RESTORED=0
MONITOR_PID=""

collect_snapshot() {
    local name="$1"
    local dir="${OUTPUT_DIR}/snapshots/${name}"
    mkdir -p "${dir}"
    date -Is > "${dir}/time.txt"
    systemctl status "${SERVICE_NAME}" --no-pager -l > "${dir}/service.txt" 2>&1 || true
    /usr/local/bin/hws > "${dir}/hws.txt" 2>&1 || true
    ls -l /dev/left_gripper /dev/left_encoder /dev/right_gripper /dev/right_encoder > "${dir}/links.txt" 2>&1 || true
    lsusb -t > "${dir}/lsusb-tree.txt" 2>&1 || true
    journalctl -k --since "${START_TIME}" --no-pager -o short-precise -l > "${dir}/kernel.log" 2>&1 || true
}

stop_monitor() {
    if [[ -n "${MONITOR_PID}" ]] && kill -0 "${MONITOR_PID}" 2>/dev/null; then
        kill -INT "${MONITOR_PID}" 2>/dev/null || true
        for _ in $(seq 1 50); do
            kill -0 "${MONITOR_PID}" 2>/dev/null || break
            sleep 0.1
        done
        kill -TERM "${MONITOR_PID}" 2>/dev/null || true
        wait "${MONITOR_PID}" 2>/dev/null || true
    fi
    MONITOR_PID=""
}

restore_service() {
    stop_monitor
    if [[ "${SERVICE_WAS_ACTIVE}" == "1" && "${SERVICE_RESTORED}" != "1" ]]; then
        systemctl start "${SERVICE_NAME}" || true
        for _ in $(seq 1 30); do systemctl is-active --quiet "${SERVICE_NAME}" && break; sleep 1; done
        SERVICE_RESTORED=1
    fi
}
trap restore_service EXIT INT TERM

analyze_hmi_log() {
    local phase="$1"
    local log="$2"
    local stimulus_exit="$3"
    local result=pass
    local values=()
    local port max_age inactive io_failures

    for current_side in left right; do
        port="/dev/${current_side}_gripper"
        max_age="$(sed -n "/\[${port//\//\\/}\]/s/.*age_ms=\([0-9][0-9]*\).*/\1/p" "${log}" | sort -nr | head -n 1)"
        max_age="${max_age:-0}"
        inactive="$(sed -n "/\[${port//\//\\/}\]/s/.*active=\(yes\|no\).*age_ms=\([0-9][0-9]*\).*/\1 \2/p" "${log}" | awk -v limit="${HMI_INACTIVE_MS}" '$1=="no" || $2>limit {count++} END {print count+0}')"
        io_failures="$(sed -n "/port=${port//\//\\/} /s/.*io_failures=\([0-9][0-9]*\).*/\1/p" "${log}" | tail -n 1)"
        io_failures="${io_failures:-999}"
        values+=("${max_age}" "${inactive}" "${io_failures}")
        if [[ "${inactive}" != "0" || "${io_failures}" != "0" ]]; then result=fail; fi
    done
    if [[ "${stimulus_exit}" != "0" ]]; then result=fail; fi
    if [[ "${result}" != "pass" ]]; then collect_snapshot "failure_${phase}"; fi
    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
        "${phase}" "${stimulus_exit}" "${values[0]}" "${values[1]}" "${values[2]}" \
        "${values[3]}" "${values[4]}" "${values[5]}" "${result}" >> "${MATRIX_TSV}"
    printf 'phase=%s result=%s left_max_age_ms=%s left_inactive=%s right_max_age_ms=%s right_inactive=%s\n' \
        "${phase}" "${result}" "${values[0]}" "${values[1]}" "${values[3]}" "${values[4]}"
}

run_phase() {
    local phase="$1"
    shift
    local phase_dir="${OUTPUT_DIR}/phases/${phase}"
    mkdir -p "${phase_dir}"
    "${GRIPPER_HMI_BIN}" --port /dev/left_gripper --port /dev/right_gripper \
        --duration 0 --poll-ms 20 > "${phase_dir}/hmi_monitor.log" 2>&1 &
    MONITOR_PID=$!
    sleep 3
    if ! kill -0 "${MONITOR_PID}" 2>/dev/null; then
        cat "${phase_dir}/hmi_monitor.log" >&2
        return 1
    fi
    set +e
    "$@" > "${phase_dir}/stimulus.log" 2>&1
    stimulus_exit=$?
    set -e
    sleep 7
    stop_monitor
    analyze_hmi_log "${phase}" "${phase_dir}/hmi_monitor.log" "${stimulus_exit}"
}

encoder_stimulus() {
    local mode="$1"
    local repeats="$2"
    "${PYTHON_BIN}" - "${ENCODER_PORT}" "${repeats}" "${REQUESTS_PER_CYCLE}" "${mode}" <<'PY'
import os
import select
import sys
import termios
import time

port = sys.argv[1]
cycles = int(sys.argv[2])
requests_per_cycle = int(sys.argv[3])
mode = sys.argv[4]
position_request = bytes([0x01, 0x03, 0x00, 0x40, 0x00, 0x01, 0xD4, 0x1E])

for cycle in range(1, cycles + 1):
    fd = os.open(port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    try:
        if mode in ("configure", "flush", "query"):
            attrs = termios.tcgetattr(fd)
            attrs[0] = 0
            attrs[1] = 0
            attrs[2] = termios.CLOCAL | termios.CREAD | termios.CS8
            attrs[3] = 0
            attrs[4] = termios.B1000000
            attrs[5] = termios.B1000000
            attrs[6][termios.VMIN] = 0
            attrs[6][termios.VTIME] = 0
            termios.tcsetattr(fd, termios.TCSANOW, attrs)
        if mode in ("flush", "query"):
            termios.tcflush(fd, termios.TCIOFLUSH)
        received = 0
        if mode == "query":
            for _ in range(requests_per_cycle):
                os.write(fd, position_request)
                readable, _, _ = select.select([fd], [], [], 0.002)
                if readable:
                    received += len(os.read(fd, 256))
                time.sleep(0.001)
        if cycle % 100 == 0 or mode == "query":
            print(f"mode={mode} cycle={cycle} received_bytes={received}", flush=True)
    finally:
        os.close(fd)
    time.sleep(0.001 if mode != "query" else 0.01)
PY
}

print_section "Encoder to HMI Interference Matrix"
echo "output_dir=${OUTPUT_DIR}"
echo "side=${SIDE}"
echo "encoder_port=${ENCODER_PORT}"
echo "quick_repeats=${QUICK_REPEATS}"
echo "query_cycles=${QUERY_CYCLES}"
echo "requests_per_cycle=${REQUESTS_PER_CYCLE}"
collect_snapshot before

if systemctl is-active --quiet "${SERVICE_NAME}"; then SERVICE_WAS_ACTIVE=1; fi
systemctl stop "${SERVICE_NAME}"
for _ in $(seq 1 30); do systemctl is-active --quiet "${SERVICE_NAME}" || break; sleep 1; done
systemctl is-active --quiet "${SERVICE_NAME}" && { echo "service is still active" >&2; exit 1; }

run_phase baseline sleep "${BASELINE_SEC}"
run_phase encoder_open_close encoder_stimulus open "${QUICK_REPEATS}"
run_phase encoder_termios encoder_stimulus configure "${QUICK_REPEATS}"
run_phase encoder_flush encoder_stimulus flush "${QUICK_REPEATS}"
run_phase encoder_query encoder_stimulus query "${QUERY_CYCLES}"

collect_snapshot after_direct_test
restore_service
collect_snapshot after_service_restore

FAIL_PHASES="$(awk -F '\t' 'NR>1 && $9!="pass" {count++} END {print count+0}' "${MATRIX_TSV}")"
cat > "${OUTPUT_DIR}/summary.txt" <<EOF
start_time=${START_TIME}
end_time=$(date '+%Y-%m-%d %H:%M:%S')
side=${SIDE}
quick_repeats=${QUICK_REPEATS}
query_cycles=${QUERY_CYCLES}
requests_per_cycle=${REQUESTS_PER_CYCLE}
hmi_inactive_ms=${HMI_INACTIVE_MS}
failed_phases=${FAIL_PHASES}
service_was_active=${SERVICE_WAS_ACTIVE}
service_restored=${SERVICE_RESTORED}
EOF

print_section "Matrix Summary"
column -t -s $'\t' "${MATRIX_TSV}" 2>/dev/null || cat "${MATRIX_TSV}"
cat "${OUTPUT_DIR}/summary.txt"
echo "result_dir=${OUTPUT_DIR}"

[[ "${FAIL_PHASES}" == "0" ]]
