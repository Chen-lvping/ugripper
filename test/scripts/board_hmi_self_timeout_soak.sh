#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/board_test_common.sh"

usage() {
    cat <<'EOF'
Usage: board_hmi_self_timeout_soak.sh [options]

Options:
  --output-dir DIR         Result directory (default: /dev/shm/ugripper_hmi_self_soak_<time>)
  --duration-sec N         Persistent HMI test duration (default: 600)
  --poll-ms N              HMI polling interval (default: 20)
  --hmi-inactive-ms N      HMI reply-age failure threshold (default: 5500)
  --encoder-max-gap-ms N   Encoder maximum allowed gap (default: 20)
  --service-name NAME      Service stopped during direct access (default: ugripper.service)
  --ugripper-root DIR      Installed UGripper root (default: /opt/ugripper)
  --gripper-hmi-bin PATH   Override GripperHmiTool binary
  --sensor-recorder-bin PATH
                            Override SensorRecorder binary
  --python-bin PATH        Python with mcap installed (default: UGripper venv)
  --help                    Show this help

The test opens both HMI ports once and keeps them open for the whole run. It
continuously sends a dynamic LED state, performs a beep on/off confirmation,
and requests state while SensorRecorder records both encoders. It intentionally
does not reconnect HMI ports during the stress window, so an HMI timeout cannot
be attributed to repeated tty open/close operations from the test itself.
EOF
}

OUTPUT_DIR="/dev/shm/ugripper_hmi_self_soak_$(timestamp_slug)"
DURATION_SEC=600
POLL_MS=20
HMI_INACTIVE_MS=5500
ENCODER_MAX_GAP_MS=20
SERVICE_NAME="ugripper.service"
UGRIPPER_ROOT="$(default_ugripper_root)"
GRIPPER_HMI_BIN_OVERRIDE=""
SENSOR_RECORDER_BIN_OVERRIDE=""
PYTHON_BIN_OVERRIDE=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --output-dir) OUTPUT_DIR="$2"; shift 2 ;;
        --duration-sec) DURATION_SEC="$2"; shift 2 ;;
        --poll-ms) POLL_MS="$2"; shift 2 ;;
        --hmi-inactive-ms) HMI_INACTIVE_MS="$2"; shift 2 ;;
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

for value_name in DURATION_SEC POLL_MS HMI_INACTIVE_MS ENCODER_MAX_GAP_MS; do
    value="${!value_name}"
    [[ "${value}" =~ ^[1-9][0-9]*$ ]] || { echo "${value_name} must be a positive integer" >&2; exit 2; }
done

GRIPPER_HMI_BIN="$(resolve_executable "${GRIPPER_HMI_BIN_OVERRIDE}" \
    "${UGRIPPER_ROOT}/bin/GripperHmiTool/GripperHmiTool")"
SENSOR_RECORDER_BIN="$(resolve_executable "${SENSOR_RECORDER_BIN_OVERRIDE}" \
    "${UGRIPPER_ROOT}/bin/SensorRecorder/SensorRecorder")"
PYTHON_BIN="$(resolve_executable "${PYTHON_BIN_OVERRIDE}" \
    "${UGRIPPER_ROOT}/.venv/bin/python3" python3)"

mkdir -p "${OUTPUT_DIR}/snapshots" "${OUTPUT_DIR}/sensor_probe"
START_TIME="$(date '+%Y-%m-%d %H:%M:%S')"
SERVICE_WAS_ACTIVE=0
SERVICE_RESTORED=0
SENSOR_PID=""
HMI_PID=""

collect_snapshot() {
    local name="$1"
    local dir="${OUTPUT_DIR}/snapshots/${name}"
    mkdir -p "${dir}"
    date -Ins > "${dir}/time.txt"
    systemctl status "${SERVICE_NAME}" --no-pager -l > "${dir}/service.txt" 2>&1 || true
    ps -eo pid,stat,comm,args > "${dir}/processes.txt" 2>&1 || true
    ls -l /dev/left_gripper /dev/left_encoder /dev/right_gripper /dev/right_encoder \
        > "${dir}/links.txt" 2>&1 || true
    lsusb -t > "${dir}/lsusb-tree.txt" 2>&1 || true
    journalctl -k --since "${START_TIME}" --no-pager -o short-precise -l \
        > "${dir}/kernel.log" 2>&1 || true
    if command -v hws >/dev/null 2>&1; then
        hws > "${dir}/hws.txt" 2>&1 || true
    fi
}

stop_process() {
    local pid="$1"
    if [[ -z "${pid}" ]] || ! kill -0 "${pid}" 2>/dev/null; then
        return
    fi
    kill -INT "${pid}" 2>/dev/null || true
    for _ in $(seq 1 100); do
        kill -0 "${pid}" 2>/dev/null || break
        sleep 0.1
    done
    kill -TERM "${pid}" 2>/dev/null || true
    wait "${pid}" 2>/dev/null || true
}

restore_service() {
    stop_process "${HMI_PID}"
    stop_process "${SENSOR_PID}"
    HMI_PID=""
    SENSOR_PID=""
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

print_section "HMI Self-timeout Soak Configuration"
echo "output_dir=${OUTPUT_DIR}"
echo "duration_sec=${DURATION_SEC}"
echo "poll_ms=${POLL_MS}"
echo "hmi_inactive_ms=${HMI_INACTIVE_MS}"
echo "encoder_max_gap_ms=${ENCODER_MAX_GAP_MS}"
echo "gripper_hmi_bin=${GRIPPER_HMI_BIN}"
echo "sensor_recorder_bin=${SENSOR_RECORDER_BIN}"

collect_snapshot before
if systemctl is-active --quiet "${SERVICE_NAME}"; then
    SERVICE_WAS_ACTIVE=1
fi

print_section "Stop Service"
systemctl stop "${SERVICE_NAME}"
for _ in $(seq 1 30); do
    systemctl is-active --quiet "${SERVICE_NAME}" || break
    sleep 1
done
systemctl is-active --quiet "${SERVICE_NAME}" && { echo "service is still active" >&2; exit 1; }

for path in /dev/left_gripper /dev/left_encoder /dev/right_gripper /dev/right_encoder; do
    [[ -e "${path}" ]] || { echo "missing device: ${path}" >&2; exit 1; }
done

print_section "Start Concurrent Encoder Recording"
"${SENSOR_RECORDER_BIN}" "${OUTPUT_DIR}/sensor_probe" > "${OUTPUT_DIR}/sensor_recorder.log" 2>&1 &
SENSOR_PID=$!
sleep 3
if ! kill -0 "${SENSOR_PID}" 2>/dev/null; then
    cat "${OUTPUT_DIR}/sensor_recorder.log" >&2
    exit 1
fi

print_section "Start Single-session HMI Stress"
set +e
"${GRIPPER_HMI_BIN}" \
    --port /dev/left_gripper \
    --port /dev/right_gripper \
    --duration "${DURATION_SEC}" \
    --poll-ms "${POLL_MS}" \
    --state RECORDING \
    --beep-duty 30 \
    --beep-freq 1000 \
    --beep-ms 300 \
    > "${OUTPUT_DIR}/hmi.log" 2>&1 &
HMI_PID=$!
wait "${HMI_PID}"
HMI_EXIT=$?
HMI_PID=""
set -e

stop_process "${SENSOR_PID}"
SENSOR_PID=""

set +e
"${PYTHON_BIN}" - "${OUTPUT_DIR}/hmi.log" "${HMI_INACTIVE_MS}" > "${OUTPUT_DIR}/hmi_report.tsv" <<'PY'
import re
import sys
from pathlib import Path

log_path = Path(sys.argv[1])
inactive_limit = int(sys.argv[2])
lines = log_path.read_text(errors="replace").splitlines()
print("side\tsamples\tmax_age_ms\tinactive_samples\tio_failures\ttx_state_req\trx_frames\trx_beep_states\tresult")
failed = False
for side in ("left", "right"):
    port = f"/dev/{side}_gripper"
    ages = []
    inactive = 0
    for line in lines:
        if f"[{port}]" not in line or "active=" not in line:
            continue
        match = re.search(r"active=(yes|no).*age_ms=(\d+)", line)
        if not match:
            continue
        active, age_text = match.groups()
        age = int(age_text)
        ages.append(age)
        if active == "no" or age > inactive_limit:
            inactive += 1

    summary = None
    for line in lines:
        if "category=io_summary" in line and f"port={port} " in line:
            summary = line
    def field(name, default):
        if summary is None:
            return default
        match = re.search(rf"\b{name}=(\d+)", summary)
        return int(match.group(1)) if match else default

    io_failures = field("io_failures", 999)
    tx_state_req = field("tx_state_req", 0)
    rx_frames = field("rx_frames", 0)
    rx_beep_states = field("rx_beep_states", 0)
    result = "pass" if ages and inactive == 0 and io_failures == 0 and tx_state_req > 0 and rx_frames > 0 else "fail"
    failed = failed or result == "fail"
    print(f"{side}\t{len(ages)}\t{max(ages, default=0)}\t{inactive}\t{io_failures}\t{tx_state_req}\t{rx_frames}\t{rx_beep_states}\t{result}")
raise SystemExit(1 if failed else 0)
PY
HMI_REPORT_EXIT=$?
set -e

set +e
"${PYTHON_BIN}" - "${OUTPUT_DIR}/sensor_probe" "${ENCODER_MAX_GAP_MS}" > "${OUTPUT_DIR}/encoder_gap_report.tsv" <<'PY'
import sys
from pathlib import Path

from mcap.reader import make_reader
from mcap.stream_reader import StreamReader


def iter_messages(path):
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
            record_name = type(record).__name__
            if record_name == "Channel":
                channels[record.id] = record
            elif record_name == "Message" and record.channel_id in channels:
                yield channels[record.channel_id], record

sensor_dir = Path(sys.argv[1])
allowed_ms = int(sys.argv[2])
thresholds_ms = (2, 5, 10, 20, 100, 1000)
print("side\tcount\tspan_sec\trate_hz\tmax_gap_ms\tgt2ms\tgt5ms\tgt10ms\tgt20ms\tgt100ms\tgt1000ms\tresult")
failed = False
for side in ("left", "right"):
    path = sensor_dir / f"sensor_{side}.mcap"
    timestamps = []
    if path.is_file():
        for _channel, message in iter_messages(path):
            timestamps.append(int(getattr(message, "log_time", getattr(message, "logTime", 0))))
    gaps = [current - previous for previous, current in zip(timestamps, timestamps[1:])]
    span_ns = timestamps[-1] - timestamps[0] if len(timestamps) > 1 else 0
    span_sec = span_ns / 1e9
    rate_hz = (len(timestamps) - 1) / span_sec if span_sec > 0 else 0.0
    max_gap_ms = max(gaps, default=0) / 1e6
    counts = [sum(gap > threshold * 1_000_000 for gap in gaps) for threshold in thresholds_ms]
    result = "pass" if timestamps and max_gap_ms <= allowed_ms else "fail"
    failed = failed or result == "fail"
    print(f"{side}\t{len(timestamps)}\t{span_sec:.3f}\t{rate_hz:.3f}\t{max_gap_ms:.3f}\t" +
          "\t".join(str(count) for count in counts) + f"\t{result}")
raise SystemExit(1 if failed else 0)
PY
ENCODER_REPORT_EXIT=$?
set -e

collect_snapshot after_direct_test
restore_service
collect_snapshot after_service_restore
journalctl -u "${SERVICE_NAME}" --since "${START_TIME}" --no-pager -o short-precise -l \
    > "${OUTPUT_DIR}/service.log" 2>&1 || true

cat > "${OUTPUT_DIR}/summary.txt" <<EOF
start_time=${START_TIME}
end_time=$(date '+%Y-%m-%d %H:%M:%S')
hmi_exit=${HMI_EXIT}
hmi_report_exit=${HMI_REPORT_EXIT}
encoder_report_exit=${ENCODER_REPORT_EXIT}
service_was_active=${SERVICE_WAS_ACTIVE}
service_restored=${SERVICE_RESTORED}
EOF

print_section "Summary"
cat "${OUTPUT_DIR}/summary.txt"
cat "${OUTPUT_DIR}/hmi_report.tsv"
cat "${OUTPUT_DIR}/encoder_gap_report.tsv"
echo "result_dir=${OUTPUT_DIR}"

[[ "${HMI_EXIT}" == "0" && "${HMI_REPORT_EXIT}" == "0" && "${ENCODER_REPORT_EXIT}" == "0" ]]
