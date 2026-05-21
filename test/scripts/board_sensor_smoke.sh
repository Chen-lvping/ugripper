#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "${SCRIPT_DIR}/board_test_common.sh"

usage() {
    cat <<'EOF'
Usage: board_sensor_smoke.sh [options]

Options:
  --output-dir DIR         Output directory for smoke artifacts
  --duration-sec N         Recording duration in seconds (default: 6)
  --ugripper-root DIR      Installed ugripper root, default prefers /opt/ugripper
  --sensor-bin PATH        Override SensorRecorder binary path
  --checker-bin PATH       Override check_sensor_mcap binary path
  --summary-script PATH    Override summarize_episode_reports.py path
  --max-sensor-span-gap-ns N
                           Optional allowed left/right sensor span gap
  --keep-output            Keep existing output directory if present
  --help                   Show this help
EOF
}

OUTPUT_DIR=""
DURATION_SEC=6
MAX_SENSOR_SPAN_GAP_NS=""
KEEP_OUTPUT=0
UGRIPPER_ROOT="${UGRIPPER_ROOT:-$(default_ugripper_root)}"
SENSOR_BIN_OVERRIDE=""
CHECKER_BIN_OVERRIDE=""
SUMMARY_SCRIPT_OVERRIDE=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --output-dir)
            OUTPUT_DIR="$2"
            shift 2
            ;;
        --duration-sec)
            DURATION_SEC="$2"
            shift 2
            ;;
        --ugripper-root)
            UGRIPPER_ROOT="$2"
            shift 2
            ;;
        --sensor-bin)
            SENSOR_BIN_OVERRIDE="$2"
            shift 2
            ;;
        --checker-bin)
            CHECKER_BIN_OVERRIDE="$2"
            shift 2
            ;;
        --summary-script)
            SUMMARY_SCRIPT_OVERRIDE="$2"
            shift 2
            ;;
        --max-sensor-span-gap-ns)
            MAX_SENSOR_SPAN_GAP_NS="$2"
            shift 2
            ;;
        --keep-output)
            KEEP_OUTPUT=1
            shift
            ;;
        --help|-h)
            usage
            exit 0
            ;;
        *)
            echo "unknown argument: $1" >&2
            usage
            exit 2
            ;;
    esac
done

if [[ -z "${OUTPUT_DIR}" ]]; then
    OUTPUT_DIR="/tmp/pp_board_sensor_smoke_$(timestamp_slug)"
fi

SENSOR_BIN="$(resolve_executable "${SENSOR_BIN_OVERRIDE}" \
    "${UGRIPPER_ROOT}/bin/SensorRecorder/SensorRecorder" \
    "${PPMAIN_ROOT}/build/x86/standalone_ros2/SensorRecorder/SensorRecorder")"
CHECKER_BIN="$(default_check_sensor_mcap_bin "${CHECKER_BIN_OVERRIDE}")"
SUMMARY_SCRIPT="$(resolve_file "${SUMMARY_SCRIPT_OVERRIDE}" \
    "${PPMAIN_ROOT}/test/scripts/summarize_episode_reports.py")"

if [[ "${KEEP_OUTPUT}" != "1" ]]; then
    rm -rf "${OUTPUT_DIR}"
fi
mkdir -p "${OUTPUT_DIR}"

RUN_LOG="${OUTPUT_DIR}/sensor_smoke_run.log"

print_section "Sensor Smoke"
echo "output_dir=${OUTPUT_DIR}"
echo "sensor_bin=${SENSOR_BIN}"
echo "checker_bin=${CHECKER_BIN}"
echo "summary_script=${SUMMARY_SCRIPT}"
echo "duration_sec=${DURATION_SEC}"

set +e
timeout -s INT "${DURATION_SEC}s" "${SENSOR_BIN}" "${OUTPUT_DIR}" >"${RUN_LOG}" 2>&1
SENSOR_EXIT=$?
set -e

if ! accept_timeout_exit "${SENSOR_EXIT}"; then
    echo "SensorRecorder exited unexpectedly: ${SENSOR_EXIT}" >&2
    tail -n 50 "${RUN_LOG}" >&2 || true
    exit "${SENSOR_EXIT}"
fi

LEFT_MCAP="${OUTPUT_DIR}/sensor_left.mcap"
RIGHT_MCAP="${OUTPUT_DIR}/sensor_right.mcap"
[[ -f "${LEFT_MCAP}" ]] || { echo "missing ${LEFT_MCAP}" >&2; exit 1; }
[[ -f "${RIGHT_MCAP}" ]] || { echo "missing ${RIGHT_MCAP}" >&2; exit 1; }

print_section "Check Left MCAP"
"${CHECKER_BIN}" \
    --expect-topic encoder_left \
    --min-message-count 2 \
    --json-out "${OUTPUT_DIR}/sensor_report_left.json" \
    "${LEFT_MCAP}"

print_section "Check Right MCAP"
"${CHECKER_BIN}" \
    --expect-topic encoder_right \
    --min-message-count 2 \
    --json-out "${OUTPUT_DIR}/sensor_report_right.json" \
    "${RIGHT_MCAP}"

print_section "Summarize Reports"
declare -a SUMMARY_ARGS=(
    --sensor-report "${OUTPUT_DIR}/sensor_report_left.json"
    --sensor-report "${OUTPUT_DIR}/sensor_report_right.json"
    --json-out "${OUTPUT_DIR}/test_summary.json"
)
if [[ -n "${MAX_SENSOR_SPAN_GAP_NS}" ]]; then
    SUMMARY_ARGS+=(--max-sensor-span-gap-ns "${MAX_SENSOR_SPAN_GAP_NS}")
fi
python3 "${SUMMARY_SCRIPT}" "${SUMMARY_ARGS[@]}"

print_section "Done"
echo "run_log=${RUN_LOG}"
echo "left_report=${OUTPUT_DIR}/sensor_report_left.json"
echo "right_report=${OUTPUT_DIR}/sensor_report_right.json"
echo "test_summary=${OUTPUT_DIR}/test_summary.json"
