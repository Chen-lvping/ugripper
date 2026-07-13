#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "${SCRIPT_DIR}/board_test_common.sh"

usage() {
    cat <<'EOF'
Usage: board_gripper_hmi_link_stress.sh [options]

Options:
  --output-dir DIR          Result directory (default: /dev/shm/ugripper_hmi_link_stress_<time>)
  --cycles N                Test cycles per selected side (default: 20)
  --side left|right|both    HMI side selection (default: both)
  --duration-sec N          GripperHmiTool duration per cycle (default: 3)
  --beep-ms N               Beep-on duration per cycle (default: 300)
  --inter-cycle-ms N        Delay between cycles (default: 200)
  --service-name NAME       Service stopped during direct hardware access (default: ugripper.service)
  --ugripper-root DIR       Installed UGripper root (default: /opt/ugripper)
  --gripper-hmi-bin PATH    Override GripperHmiTool binary
  --sensor-recorder-bin PATH
                             Override SensorRecorder binary
  --encoder-max-gap-ms N     Maximum allowed encoder timestamp gap (default: 100)
  --python-bin PATH          Python with the mcap package (default: UGripper venv)
  --skip-encoder-probe      Do not run SensorRecorder beside the HMI test
  --no-service-management   Require the service to already be inactive and do not restart it
  --help                    Show this help

The test repeatedly reconnects to each selected HMI, sends RGB and beep commands,
requires valid state replies, and verifies both beep-on and beep-off readback. By
default SensorRecorder runs for the whole stress window to prove whether the
encoder UARTs on the same CH9344 devices remain healthy.
EOF
}

OUTPUT_DIR="/dev/shm/ugripper_hmi_link_stress_$(timestamp_slug)"
CYCLES=20
SIDE="both"
DURATION_SEC=3
BEEP_MS=300
INTER_CYCLE_MS=200
SERVICE_NAME="ugripper.service"
UGRIPPER_ROOT="$(default_ugripper_root)"
GRIPPER_HMI_BIN_OVERRIDE=""
SENSOR_RECORDER_BIN_OVERRIDE=""
ENCODER_MAX_GAP_MS=100
PYTHON_BIN_OVERRIDE=""
SKIP_ENCODER_PROBE=0
MANAGE_SERVICE=1

while [[ $# -gt 0 ]]; do
    case "$1" in
        --output-dir) OUTPUT_DIR="$2"; shift 2 ;;
        --cycles) CYCLES="$2"; shift 2 ;;
        --side) SIDE="$2"; shift 2 ;;
        --duration-sec) DURATION_SEC="$2"; shift 2 ;;
        --beep-ms) BEEP_MS="$2"; shift 2 ;;
        --inter-cycle-ms) INTER_CYCLE_MS="$2"; shift 2 ;;
        --service-name) SERVICE_NAME="$2"; shift 2 ;;
        --ugripper-root) UGRIPPER_ROOT="$2"; shift 2 ;;
        --gripper-hmi-bin) GRIPPER_HMI_BIN_OVERRIDE="$2"; shift 2 ;;
        --sensor-recorder-bin) SENSOR_RECORDER_BIN_OVERRIDE="$2"; shift 2 ;;
        --encoder-max-gap-ms) ENCODER_MAX_GAP_MS="$2"; shift 2 ;;
        --python-bin) PYTHON_BIN_OVERRIDE="$2"; shift 2 ;;
        --skip-encoder-probe) SKIP_ENCODER_PROBE=1; shift ;;
        --no-service-management) MANAGE_SERVICE=0; shift ;;
        --help|-h) usage; exit 0 ;;
        *) echo "unknown argument: $1" >&2; usage; exit 2 ;;
    esac
done

[[ "${CYCLES}" =~ ^[1-9][0-9]*$ ]] || { echo "cycles must be a positive integer" >&2; exit 2; }
[[ "${DURATION_SEC}" =~ ^[1-9][0-9]*$ ]] || { echo "duration-sec must be a positive integer" >&2; exit 2; }
[[ "${BEEP_MS}" =~ ^[1-9][0-9]*$ ]] || { echo "beep-ms must be a positive integer" >&2; exit 2; }
[[ "${INTER_CYCLE_MS}" =~ ^[0-9]+$ ]] || { echo "inter-cycle-ms must be a non-negative integer" >&2; exit 2; }
[[ "${ENCODER_MAX_GAP_MS}" =~ ^[1-9][0-9]*$ ]] || { echo "encoder-max-gap-ms must be a positive integer" >&2; exit 2; }
case "${SIDE}" in
    left|right|both) ;;
    *) echo "side must be left, right, or both" >&2; exit 2 ;;
esac

GRIPPER_HMI_BIN="$(resolve_executable "${GRIPPER_HMI_BIN_OVERRIDE}" \
    "${UGRIPPER_ROOT}/bin/GripperHmiTool/GripperHmiTool")"
SENSOR_RECORDER_BIN=""
PYTHON_BIN=""
if [[ "${SKIP_ENCODER_PROBE}" != "1" ]]; then
    SENSOR_RECORDER_BIN="$(resolve_executable "${SENSOR_RECORDER_BIN_OVERRIDE}" \
        "${UGRIPPER_ROOT}/bin/SensorRecorder/SensorRecorder")"
    PYTHON_BIN="$(resolve_executable "${PYTHON_BIN_OVERRIDE}" \
        "${UGRIPPER_ROOT}/.venv/bin/python3" \
        "/usr/bin/python3")"
fi

mkdir -p "${OUTPUT_DIR}/cycles" "${OUTPUT_DIR}/snapshots"
SUMMARY_TSV="${OUTPUT_DIR}/cycle_summary.tsv"
printf 'cycle\tside\texit_code\tconnected\tactive_replies\tbeep_on_replies\tbeep_off_replies\ttx_state_req\trx_beep_states\tio_failures\tmax_age_ms\tresult\n' > "${SUMMARY_TSV}"

SERVICE_WAS_ACTIVE=0
SERVICE_RESTORED=0
SENSOR_PID=""
SENSOR_STOPPED=0
START_TIME="$(date '+%Y-%m-%d %H:%M:%S')"

collect_snapshot() {
    local name="$1"
    local dir="${OUTPUT_DIR}/snapshots/${name}"
    mkdir -p "${dir}"
    date -Is > "${dir}/time.txt"
    systemctl status "${SERVICE_NAME}" --no-pager -l > "${dir}/service.txt" 2>&1 || true
    /usr/local/bin/hws > "${dir}/hws.txt" 2>&1 || true
    ls -l /dev/left_gripper /dev/left_encoder /dev/right_gripper /dev/right_encoder \
        > "${dir}/links.txt" 2>&1 || true
    lsusb -t > "${dir}/lsusb-tree.txt" 2>&1 || true
    lsusb > "${dir}/lsusb.txt" 2>&1 || true
    journalctl -k --since "${START_TIME}" --no-pager -o short-precise -l \
        > "${dir}/kernel.log" 2>&1 || true
}

stop_sensor_probe() {
    if [[ -n "${SENSOR_PID}" ]] && kill -0 "${SENSOR_PID}" 2>/dev/null; then
        kill -INT "${SENSOR_PID}" 2>/dev/null || true
        for _ in $(seq 1 50); do
            kill -0 "${SENSOR_PID}" 2>/dev/null || break
            sleep 0.1
        done
        kill -TERM "${SENSOR_PID}" 2>/dev/null || true
        wait "${SENSOR_PID}" 2>/dev/null || true
    fi
    SENSOR_STOPPED=1
}

restore_service() {
    stop_sensor_probe
    if [[ "${MANAGE_SERVICE}" == "1" && "${SERVICE_WAS_ACTIVE}" == "1" && "${SERVICE_RESTORED}" != "1" ]]; then
        systemctl start "${SERVICE_NAME}" || true
        for _ in $(seq 1 30); do
            systemctl is-active --quiet "${SERVICE_NAME}" && break
            sleep 1
        done
        SERVICE_RESTORED=1
    fi
}

trap restore_service EXIT INT TERM

print_section "HMI Link Stress Configuration"
echo "output_dir=${OUTPUT_DIR}"
echo "cycles=${CYCLES}"
echo "side=${SIDE}"
echo "duration_sec=${DURATION_SEC}"
echo "beep_ms=${BEEP_MS}"
echo "gripper_hmi_bin=${GRIPPER_HMI_BIN}"
echo "sensor_recorder_bin=${SENSOR_RECORDER_BIN:-disabled}"
echo "encoder_max_gap_ms=${ENCODER_MAX_GAP_MS}"

collect_snapshot before

if systemctl is-active --quiet "${SERVICE_NAME}"; then
    SERVICE_WAS_ACTIVE=1
fi
if [[ "${MANAGE_SERVICE}" == "1" ]]; then
    print_section "Stop Service"
    systemctl stop "${SERVICE_NAME}"
    for _ in $(seq 1 30); do
        systemctl is-active --quiet "${SERVICE_NAME}" || break
        sleep 1
    done
fi
if systemctl is-active --quiet "${SERVICE_NAME}"; then
    echo "service is still active; refusing direct hardware access" >&2
    exit 1
fi

for path in /dev/left_gripper /dev/right_gripper; do
    [[ -e "${path}" ]] || { echo "missing HMI port: ${path}" >&2; exit 1; }
done

if [[ "${SKIP_ENCODER_PROBE}" != "1" ]]; then
    print_section "Start Concurrent Encoder Probe"
    SENSOR_DIR="${OUTPUT_DIR}/sensor_probe"
    mkdir -p "${SENSOR_DIR}"
    "${SENSOR_RECORDER_BIN}" "${SENSOR_DIR}" > "${OUTPUT_DIR}/sensor_recorder.log" 2>&1 &
    SENSOR_PID=$!
    sleep 2
    if ! kill -0 "${SENSOR_PID}" 2>/dev/null; then
        echo "SensorRecorder exited during startup" >&2
        cat "${OUTPUT_DIR}/sensor_recorder.log" >&2
        exit 1
    fi
fi

declare -a SIDES=()
if [[ "${SIDE}" == "both" ]]; then
    SIDES=(left right)
else
    SIDES=("${SIDE}")
fi

HMI_FAILURES=0
INFRA_FAILURES=0
TOTAL=0
for cycle in $(seq 1 "${CYCLES}"); do
    for current_side in "${SIDES[@]}"; do
        TOTAL=$((TOTAL + 1))
        port="/dev/${current_side}_gripper"
        log="${OUTPUT_DIR}/cycles/$(printf '%04d' "${cycle}")_${current_side}.log"
        red=$((20 + (cycle * 17) % 180))
        green=$((20 + (cycle * 29) % 180))
        blue=$((20 + (cycle * 43) % 180))

        set +e
        timeout "$((DURATION_SEC + 8))" "${GRIPPER_HMI_BIN}" \
            --port "${port}" \
            --duration "${DURATION_SEC}" \
            --poll-ms 20 \
            --rgb "${red}" "${green}" "${blue}" \
            --beep-duty 30 \
            --beep-freq 1000 \
            --beep-ms "${BEEP_MS}" \
            > "${log}" 2>&1
        exit_code=$?
        set -e

        connected=$(grep -c '^Connected 1 gripper HMI device' "${log}" || true)
        active_replies=$(grep -c 'active=yes' "${log}" || true)
        beep_on_replies=$(grep -Ec 'beep_duty=30[[:space:]]+beep_freq=1000' "${log}" || true)
        beep_off_replies=$(grep -Ec 'beep_duty=0[[:space:]]+beep_freq=[0-9]+' "${log}" || true)
        tx_state_req=$(sed -n 's/.*tx_state_req=\([0-9][0-9]*\).*/\1/p' "${log}" | tail -n 1)
        rx_beep_states=$(sed -n 's/.*rx_beep_states=\([0-9][0-9]*\).*/\1/p' "${log}" | tail -n 1)
        io_failures=$(sed -n 's/.*io_failures=\([0-9][0-9]*\).*/\1/p' "${log}" | tail -n 1)
        tx_state_req="${tx_state_req:-0}"
        rx_beep_states="${rx_beep_states:-0}"
        io_failures="${io_failures:-999}"
        max_age_ms=$(sed -n 's/.*age_ms=\([0-9][0-9]*\).*/\1/p' "${log}" | sort -nr | head -n 1)
        max_age_ms="${max_age_ms:-0}"

        result=pass
        if [[ "${exit_code}" != "0" || "${connected}" -lt 1 || "${active_replies}" -lt 1 || \
              "${beep_on_replies}" -lt 1 || "${beep_off_replies}" -lt 1 || \
              "${tx_state_req}" -lt 2 || "${rx_beep_states}" -lt 2 || "${io_failures}" -ne 0 || \
              $(grep -Ec 'Failed to (connect|enable beep|silence beep|set LED)' "${log}" || true) -ne 0 ]]; then
            result=fail
            HMI_FAILURES=$((HMI_FAILURES + 1))
            collect_snapshot "failure_$(printf '%04d' "${cycle}")_${current_side}"
        fi

        printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
            "${cycle}" "${current_side}" "${exit_code}" "${connected}" "${active_replies}" \
            "${beep_on_replies}" "${beep_off_replies}" "${tx_state_req}" "${rx_beep_states}" \
            "${io_failures}" "${max_age_ms}" "${result}" \
            >> "${SUMMARY_TSV}"
        printf 'cycle=%s side=%s result=%s active=%s beep_on=%s beep_off=%s tx_state=%s rx_state=%s io_failures=%s max_age_ms=%s\n' \
            "${cycle}" "${current_side}" "${result}" "${active_replies}" \
            "${beep_on_replies}" "${beep_off_replies}" "${tx_state_req}" "${rx_beep_states}" \
            "${io_failures}" "${max_age_ms}"

        if [[ "${INTER_CYCLE_MS}" -gt 0 ]]; then
            sleep "$(awk "BEGIN { printf \"%.3f\", ${INTER_CYCLE_MS}/1000 }")"
        fi
    done
done

stop_sensor_probe

SENSOR_RESULT=skipped
ENCODER_GAP_REPORT="${OUTPUT_DIR}/encoder_gap_report.tsv"
if [[ "${SKIP_ENCODER_PROBE}" != "1" ]]; then
    SENSOR_RESULT=pass
    for sensor_side in left right; do
        if ! grep -Eq "\[SensorStats-encoder_${sensor_side}\].*received_samples=[1-9][0-9]*.*emitted_messages=[1-9][0-9]*" \
            "${OUTPUT_DIR}/sensor_recorder.log"; then
            SENSOR_RESULT=fail
        fi
        if [[ ! -s "${OUTPUT_DIR}/sensor_probe/sensor_${sensor_side}.mcap" ]]; then
            SENSOR_RESULT=fail
        fi
    done
    set +e
    "${PYTHON_BIN}" - "${OUTPUT_DIR}/sensor_probe" "${ENCODER_MAX_GAP_MS}" > "${ENCODER_GAP_REPORT}" <<'PY'
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
max_allowed_gap_ns = int(sys.argv[2]) * 1_000_000
failed = False
print("side\tcount\tspan_ns\tmax_gap_ns\tmax_gap_ms\tresult")
for side in ("left", "right"):
    path = sensor_dir / f"sensor_{side}.mcap"
    count = 0
    first_time = None
    previous_time = None
    last_time = None
    max_gap_ns = 0
    for _channel, message in iter_messages(path):
        log_time_ns = int(getattr(message, "log_time", getattr(message, "logTime", 0)))
        if first_time is None:
            first_time = log_time_ns
        if previous_time is not None:
            max_gap_ns = max(max_gap_ns, log_time_ns - previous_time)
        previous_time = log_time_ns
        last_time = log_time_ns
        count += 1
    span_ns = 0 if first_time is None or last_time is None else last_time - first_time
    result = "pass" if count > 0 and max_gap_ns <= max_allowed_gap_ns else "fail"
    failed = failed or result == "fail"
    print(f"{side}\t{count}\t{span_ns}\t{max_gap_ns}\t{max_gap_ns / 1e6:.3f}\t{result}")
raise SystemExit(1 if failed else 0)
PY
    encoder_gap_exit=$?
    set -e
    if [[ "${encoder_gap_exit}" != "0" ]]; then
        SENSOR_RESULT=fail
    fi
    if [[ "${SENSOR_RESULT}" != "pass" ]]; then
        INFRA_FAILURES=$((INFRA_FAILURES + 1))
        collect_snapshot encoder_probe_failure
    fi
fi

collect_snapshot after_direct_test
restore_service
collect_snapshot after_service_restore
journalctl -u "${SERVICE_NAME}" --since "${START_TIME}" --no-pager -o short-precise -l \
    > "${OUTPUT_DIR}/service.log" 2>&1 || true

cat > "${OUTPUT_DIR}/summary.txt" <<EOF
start_time=${START_TIME}
end_time=$(date '+%Y-%m-%d %H:%M:%S')
total_hmi_cycles=${TOTAL}
hmi_failures=${HMI_FAILURES}
infrastructure_failures=${INFRA_FAILURES}
encoder_probe=${SENSOR_RESULT}
encoder_max_gap_ms=${ENCODER_MAX_GAP_MS}
service_was_active=${SERVICE_WAS_ACTIVE}
service_restored=${SERVICE_RESTORED}
EOF

print_section "Summary"
cat "${OUTPUT_DIR}/summary.txt"
echo "cycle_summary=${SUMMARY_TSV}"
echo "result_dir=${OUTPUT_DIR}"

[[ "${HMI_FAILURES}" == "0" && "${INFRA_FAILURES}" == "0" ]]
