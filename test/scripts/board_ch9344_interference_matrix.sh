#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "${SCRIPT_DIR}/board_test_common.sh"

usage() {
    cat <<'EOF'
Usage: board_ch9344_interference_matrix.sh [options]

Options:
  --output-dir DIR        Result directory (default: /dev/shm/ugripper_ch9344_matrix_<time>)
  --side left|right       HMI UART used as stimulus (default: left)
  --baseline-sec N        Encoder-only baseline duration (default: 15)
  --quick-repeats N       Repetitions for raw open/config/flush phases (default: 1000)
  --tool-cycles N         GripperHmiTool reconnect cycles per phase (default: 40)
  --persistent-sec N      Persistent HMI traffic duration (default: 30)
  --encoder-max-gap-ms N  Gap threshold used for phase classification (default: 20)
  --service-name NAME     Service stopped during the matrix (default: ugripper.service)
  --ugripper-root DIR     Installed UGripper root (default: /opt/ugripper)
  --gripper-hmi-bin PATH  Override GripperHmiTool binary
  --sensor-recorder-bin PATH
                           Override SensorRecorder binary
  --python-bin PATH       Python with mcap installed (default: UGripper venv)
  --help                  Show this help

Phases isolate increasingly invasive operations on one HMI UART while both
encoders on the two CH9344 devices are recorded continuously per phase:

  baseline              no HMI access
  raw_open_close        os.open/os.close only
  termios_reconfigure   open + 115200 8N1 tcsetattr
  tcflush_both          open + configure + TCIOFLUSH
  tioc_exclusive        configure + flush + TIOCEXCL
  raw_state_query       exact setup plus raw state query/read
  raw_rgb_command       exact setup plus raw RGB command
  raw_beep_command      exact setup plus raw beep on/off/query
  hmi_reconnect_state   full GripperHmiTool reconnect and state traffic
  hmi_reconnect_beep    full reconnect plus beep enable/disable/readback
  hmi_persistent        one persistent HMI connection with state/beep traffic

The result identifies which operation first correlates with encoder gaps on the
same CH9344, and compares the opposite-side encoder as a control.
EOF
}

OUTPUT_DIR="/dev/shm/ugripper_ch9344_matrix_$(timestamp_slug)"
SIDE="left"
BASELINE_SEC=15
QUICK_REPEATS=1000
TOOL_CYCLES=40
PERSISTENT_SEC=30
ENCODER_MAX_GAP_MS=20
SERVICE_NAME="ugripper.service"
UGRIPPER_ROOT="$(default_ugripper_root)"
GRIPPER_HMI_BIN_OVERRIDE=""
SENSOR_RECORDER_BIN_OVERRIDE=""
PYTHON_BIN_OVERRIDE=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --output-dir) OUTPUT_DIR="$2"; shift 2 ;;
        --side) SIDE="$2"; shift 2 ;;
        --baseline-sec) BASELINE_SEC="$2"; shift 2 ;;
        --quick-repeats) QUICK_REPEATS="$2"; shift 2 ;;
        --tool-cycles) TOOL_CYCLES="$2"; shift 2 ;;
        --persistent-sec) PERSISTENT_SEC="$2"; shift 2 ;;
        --encoder-max-gap-ms) ENCODER_MAX_GAP_MS="$2"; shift 2 ;;
        --service-name) SERVICE_NAME="$2"; shift 2 ;;
        --ugripper-root) UGRIPPER_ROOT="$2"; shift 2 ;;
        --gripper-hmi-bin) GRIPPER_HMI_BIN_OVERRIDE="$2"; shift 2 ;;
        --sensor-recorder-bin) SENSOR_RECORDER_BIN_OVERRIDE="$2"; shift 2 ;;
        --python-bin) PYTHON_BIN_OVERRIDE="$2"; shift 2 ;;
        --help|-h) usage; exit 0 ;;
        *) echo "unknown argument: $1" >&2; usage; exit 2 ;;
    esac
done

case "${SIDE}" in left|right) ;; *) echo "side must be left or right" >&2; exit 2 ;; esac
for value_name in BASELINE_SEC QUICK_REPEATS TOOL_CYCLES PERSISTENT_SEC ENCODER_MAX_GAP_MS; do
    value="${!value_name}"
    [[ "${value}" =~ ^[1-9][0-9]*$ ]] || { echo "${value_name} must be a positive integer" >&2; exit 2; }
done

GRIPPER_HMI_BIN="$(resolve_executable "${GRIPPER_HMI_BIN_OVERRIDE}" \
    "${UGRIPPER_ROOT}/bin/GripperHmiTool/GripperHmiTool")"
SENSOR_RECORDER_BIN="$(resolve_executable "${SENSOR_RECORDER_BIN_OVERRIDE}" \
    "${UGRIPPER_ROOT}/bin/SensorRecorder/SensorRecorder")"
PYTHON_BIN="$(resolve_executable "${PYTHON_BIN_OVERRIDE}" \
    "${UGRIPPER_ROOT}/.venv/bin/python3" \
    "/usr/bin/python3")"
HMI_PORT="/dev/${SIDE}_gripper"

mkdir -p "${OUTPUT_DIR}/phases" "${OUTPUT_DIR}/snapshots"
MATRIX_TSV="${OUTPUT_DIR}/matrix.tsv"
printf 'phase\tstimulus_exit\tleft_count\tleft_max_gap_ms\tleft_gap_over_threshold\tright_count\tright_max_gap_ms\tright_gap_over_threshold\tresult\n' > "${MATRIX_TSV}"
START_TIME="$(date '+%Y-%m-%d %H:%M:%S')"
SERVICE_WAS_ACTIVE=0
SERVICE_RESTORED=0
SENSOR_PID=""

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

stop_sensor() {
    if [[ -n "${SENSOR_PID}" ]] && kill -0 "${SENSOR_PID}" 2>/dev/null; then
        kill -INT "${SENSOR_PID}" 2>/dev/null || true
        for _ in $(seq 1 50); do
            kill -0 "${SENSOR_PID}" 2>/dev/null || break
            sleep 0.1
        done
        kill -TERM "${SENSOR_PID}" 2>/dev/null || true
        wait "${SENSOR_PID}" 2>/dev/null || true
    fi
    SENSOR_PID=""
}

restore_service() {
    stop_sensor
    if [[ "${SERVICE_WAS_ACTIVE}" == "1" && "${SERVICE_RESTORED}" != "1" ]]; then
        systemctl start "${SERVICE_NAME}" || true
        for _ in $(seq 1 30); do
            systemctl is-active --quiet "${SERVICE_NAME}" && break
            sleep 1
        done
        SERVICE_RESTORED=1
    fi
}
trap restore_service EXIT INT TERM

analyze_phase() {
    local phase="$1"
    local phase_dir="$2"
    local stimulus_exit="$3"
    local report="${phase_dir}/encoder_gap_report.tsv"

    "${PYTHON_BIN}" - "${phase_dir}/sensor" "${ENCODER_MAX_GAP_MS}" > "${report}" <<'PY'
import sys
from pathlib import Path
from mcap.reader import make_reader
from mcap.stream_reader import StreamReader


def messages(path):
    with path.open("rb") as stream:
        reader = make_reader(stream)
        yielded = False
        try:
            for _schema, channel, message in reader.iter_messages():
                yielded = True
                yield channel, message
        except Exception:
            if yielded:
                raise
        if yielded:
            return
        stream.seek(0)
        channels = {}
        for record in StreamReader(stream).records:
            name = type(record).__name__
            if name == "Channel":
                channels[record.id] = record
            elif name == "Message" and record.channel_id in channels:
                yield channels[record.channel_id], record


root = Path(sys.argv[1])
threshold_ns = int(sys.argv[2]) * 1_000_000
print("side\tcount\tspan_ns\tmax_gap_ns\tmax_gap_ms\tgaps_over_threshold")
for side in ("left", "right"):
    count = 0
    first = None
    previous = None
    last = None
    max_gap = 0
    over = 0
    events = []
    for _channel, message in messages(root / f"sensor_{side}.mcap"):
        now = int(getattr(message, "log_time", getattr(message, "logTime", 0)))
        if first is None:
            first = now
        if previous is not None:
            gap = now - previous
            max_gap = max(max_gap, gap)
            if gap > threshold_ns:
                over += 1
                events.append((previous, now, gap))
        previous = now
        last = now
        count += 1
    span = 0 if first is None or last is None else last - first
    print(f"{side}\t{count}\t{span}\t{max_gap}\t{max_gap / 1e6:.3f}\t{over}")
    with (root.parent / f"encoder_{side}_gap_events.tsv").open("w") as output:
        output.write("previous_ns\tcurrent_ns\tgap_ns\tgap_ms\n")
        for previous_ns, current_ns, gap_ns in events:
            output.write(f"{previous_ns}\t{current_ns}\t{gap_ns}\t{gap_ns / 1e6:.3f}\n")
PY

    left_line="$(awk -F '\t' '$1=="left" {print}' "${report}")"
    right_line="$(awk -F '\t' '$1=="right" {print}' "${report}")"
    left_count="$(echo "${left_line}" | cut -f2)"
    left_max="$(echo "${left_line}" | cut -f5)"
    left_over="$(echo "${left_line}" | cut -f6)"
    right_count="$(echo "${right_line}" | cut -f2)"
    right_max="$(echo "${right_line}" | cut -f5)"
    right_over="$(echo "${right_line}" | cut -f6)"
    result=pass
    if [[ "${stimulus_exit}" != "0" || "${left_count}" == "0" || "${right_count}" == "0" || \
          "${left_over}" != "0" || "${right_over}" != "0" ]]; then
        result=fail
        collect_snapshot "failure_${phase}"
    fi
    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
        "${phase}" "${stimulus_exit}" "${left_count}" "${left_max}" "${left_over}" \
        "${right_count}" "${right_max}" "${right_over}" "${result}" >> "${MATRIX_TSV}"
    printf 'phase=%s result=%s left_max_gap_ms=%s left_over=%s right_max_gap_ms=%s right_over=%s\n' \
        "${phase}" "${result}" "${left_max}" "${left_over}" "${right_max}" "${right_over}"
}

run_phase() {
    local phase="$1"
    shift
    local phase_dir="${OUTPUT_DIR}/phases/${phase}"
    mkdir -p "${phase_dir}/sensor"
    "${SENSOR_RECORDER_BIN}" "${phase_dir}/sensor" > "${phase_dir}/sensor_recorder.log" 2>&1 &
    SENSOR_PID=$!
    sleep 2
    if ! kill -0 "${SENSOR_PID}" 2>/dev/null; then
        cat "${phase_dir}/sensor_recorder.log" >&2
        return 1
    fi
    set +e
    "$@" > "${phase_dir}/stimulus.log" 2>&1
    stimulus_exit=$?
    set -e
    sleep 2
    stop_sensor
    analyze_phase "${phase}" "${phase_dir}" "${stimulus_exit}"
}

quick_stimulus() {
    local mode="$1"
    "${PYTHON_BIN}" - "${HMI_PORT}" "${QUICK_REPEATS}" "${mode}" <<'PY'
import os
import sys
import termios
import time

port = sys.argv[1]
repeats = int(sys.argv[2])
mode = sys.argv[3]

for index in range(repeats):
    fd = os.open(port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    try:
        if mode in ("configure", "flush"):
            attrs = termios.tcgetattr(fd)
            attrs[0] = 0
            attrs[1] = 0
            attrs[2] = termios.CLOCAL | termios.CREAD | termios.CS8
            attrs[3] = 0
            attrs[4] = termios.B115200
            attrs[5] = termios.B115200
            attrs[6][termios.VMIN] = 0
            attrs[6][termios.VTIME] = 0
            termios.tcsetattr(fd, termios.TCSANOW, attrs)
        if mode == "flush":
            termios.tcflush(fd, termios.TCIOFLUSH)
    finally:
        os.close(fd)
    if index % 100 == 0:
        print(f"mode={mode} iteration={index}", flush=True)
    time.sleep(0.001)
PY
}

raw_protocol_stimulus() {
    local mode="$1"
    "${PYTHON_BIN}" - "${HMI_PORT}" "${TOOL_CYCLES}" "${mode}" <<'PY'
import fcntl
import os
import select
import sys
import termios
import time

port = sys.argv[1]
cycles = int(sys.argv[2])
mode = sys.argv[3]
TIOCEXCL = 0x540C


def xor_frame(values):
    checksum = 0
    for value in values:
        checksum ^= value
    return bytes(values + [checksum])


state_request = xor_frame([0x5A, 0x5A, 0x04, 0x02, 0x00, 0x00])
rgb_command = xor_frame([0x5A, 0x5A, 0x0F, 0x30, 0x80, 0xC0, 0x00])
beep_on = xor_frame([0x5A, 0x5A, 0x0E, 30, 0x03, 0xE8])
beep_off = xor_frame([0x5A, 0x5A, 0x0E, 0, 0x03, 0xE8])

for cycle in range(1, cycles + 1):
    fd = os.open(port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    try:
        fcntl.ioctl(fd, TIOCEXCL)
        attrs = termios.tcgetattr(fd)
        attrs[0] = 0
        attrs[1] = 0
        attrs[2] = termios.CLOCAL | termios.CREAD | termios.CS8
        attrs[3] = 0
        attrs[4] = termios.B115200
        attrs[5] = termios.B115200
        attrs[6][termios.VMIN] = 0
        attrs[6][termios.VTIME] = 0
        termios.tcsetattr(fd, termios.TCSANOW, attrs)
        termios.tcflush(fd, termios.TCIOFLUSH)

        commands = []
        if mode == "state":
            commands = [state_request] * 10
        elif mode == "rgb":
            commands = [rgb_command] * 10
        elif mode == "beep":
            commands = [beep_on, state_request, beep_off, state_request] * 5

        received = 0
        for command in commands:
            os.write(fd, command)
            readable, _, _ = select.select([fd], [], [], 0.02)
            if readable:
                received += len(os.read(fd, 256))
            time.sleep(0.02)
        print(f"cycle={cycle} mode={mode} received_bytes={received}", flush=True)
        termios.tcflush(fd, termios.TCIOFLUSH)
    finally:
        os.close(fd)
    time.sleep(0.02)
PY
}

hmi_reconnect_stimulus() {
    local with_beep="$1"
    local cycle
    for cycle in $(seq 1 "${TOOL_CYCLES}"); do
        args=(--port "${HMI_PORT}" --duration 1 --poll-ms 20 --rgb 30 80 130)
        if [[ "${with_beep}" == "1" ]]; then
            args+=(--beep-duty 30 --beep-freq 1000 --beep-ms 200)
        fi
        timeout 8 "${GRIPPER_HMI_BIN}" "${args[@]}"
        echo "cycle=${cycle} with_beep=${with_beep}"
    done
}

print_section "CH9344 Interference Matrix"
echo "output_dir=${OUTPUT_DIR}"
echo "side=${SIDE}"
echo "hmi_port=${HMI_PORT}"
echo "quick_repeats=${QUICK_REPEATS}"
echo "tool_cycles=${TOOL_CYCLES}"
echo "encoder_max_gap_ms=${ENCODER_MAX_GAP_MS}"
collect_snapshot before

if systemctl is-active --quiet "${SERVICE_NAME}"; then SERVICE_WAS_ACTIVE=1; fi
systemctl stop "${SERVICE_NAME}"
for _ in $(seq 1 30); do systemctl is-active --quiet "${SERVICE_NAME}" || break; sleep 1; done
systemctl is-active --quiet "${SERVICE_NAME}" && { echo "service is still active" >&2; exit 1; }

run_phase baseline sleep "${BASELINE_SEC}"
run_phase raw_open_close quick_stimulus open
run_phase termios_reconfigure quick_stimulus configure
run_phase tcflush_both quick_stimulus flush
run_phase tioc_exclusive raw_protocol_stimulus exclusive
run_phase raw_state_query raw_protocol_stimulus state
run_phase raw_rgb_command raw_protocol_stimulus rgb
run_phase raw_beep_command raw_protocol_stimulus beep
run_phase hmi_reconnect_state hmi_reconnect_stimulus 0
run_phase hmi_reconnect_beep hmi_reconnect_stimulus 1
run_phase hmi_persistent timeout "$((PERSISTENT_SEC + 8))" "${GRIPPER_HMI_BIN}" \
    --port "${HMI_PORT}" --duration "${PERSISTENT_SEC}" --poll-ms 20 \
    --rgb 60 120 180 --beep-duty 30 --beep-freq 1000 --beep-ms 300

collect_snapshot after_direct_test
restore_service
collect_snapshot after_service_restore

FAIL_PHASES="$(awk -F '\t' 'NR>1 && $9!="pass" {count++} END {print count+0}' "${MATRIX_TSV}")"
cat > "${OUTPUT_DIR}/summary.txt" <<EOF
start_time=${START_TIME}
end_time=$(date '+%Y-%m-%d %H:%M:%S')
side=${SIDE}
quick_repeats=${QUICK_REPEATS}
tool_cycles=${TOOL_CYCLES}
encoder_max_gap_ms=${ENCODER_MAX_GAP_MS}
failed_phases=${FAIL_PHASES}
service_was_active=${SERVICE_WAS_ACTIVE}
service_restored=${SERVICE_RESTORED}
EOF

print_section "Matrix Summary"
column -t -s $'\t' "${MATRIX_TSV}" 2>/dev/null || cat "${MATRIX_TSV}"
cat "${OUTPUT_DIR}/summary.txt"
echo "result_dir=${OUTPUT_DIR}"

[[ "${FAIL_PHASES}" == "0" ]]
