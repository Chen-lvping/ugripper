#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "${SCRIPT_DIR}/board_test_common.sh"

usage() {
    cat <<'EOF'
Usage: board_gripper_hmi_log_check.sh [options]

Options:
  --output-dir DIR           Output directory for reports
  --log-file PATH            Existing service log to analyze
  --journal-since TEXT       journalctl --since value when log-file is omitted
  --gripper-script PATH      Override check_gripper_ack_log.py path
  --hmi-script PATH          Override check_hmi_event_log.py path
  --require-command CSV      Required gripper commands, comma-separated
  --require-event CSV        Required HMI button events, comma-separated
  --require-led-state CSV    Required HMI led states, comma-separated
  --require-io-total-at-least FIELD:COUNT
                             Require aggregated gripper io summary FIELD >= COUNT; repeatable
  --allow-timeouts           Allow gripper command timeouts
  --allow-failures           Allow gripper command failures
  --help                     Show this help
EOF
}

OUTPUT_DIR="/tmp/pp_board_gripper_hmi_log_check_$(timestamp_slug)"
LOG_FILE=""
JOURNAL_SINCE="15 min ago"
GRIPPER_SCRIPT_OVERRIDE=""
HMI_SCRIPT_OVERRIDE=""
REQUIRE_COMMAND_CSV=""
REQUIRE_EVENT_CSV=""
REQUIRE_LED_STATE_CSV=""
declare -a REQUIRE_IO_TOTAL_ARGS=()
ALLOW_TIMEOUTS=0
ALLOW_FAILURES=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --output-dir)
            OUTPUT_DIR="$2"
            shift 2
            ;;
        --log-file)
            LOG_FILE="$2"
            shift 2
            ;;
        --journal-since)
            JOURNAL_SINCE="$2"
            shift 2
            ;;
        --gripper-script)
            GRIPPER_SCRIPT_OVERRIDE="$2"
            shift 2
            ;;
        --hmi-script)
            HMI_SCRIPT_OVERRIDE="$2"
            shift 2
            ;;
        --require-command)
            REQUIRE_COMMAND_CSV="$2"
            shift 2
            ;;
        --require-event)
            REQUIRE_EVENT_CSV="$2"
            shift 2
            ;;
        --require-led-state)
            REQUIRE_LED_STATE_CSV="$2"
            shift 2
            ;;
        --require-io-total-at-least)
            REQUIRE_IO_TOTAL_ARGS+=("$2")
            shift 2
            ;;
        --allow-timeouts)
            ALLOW_TIMEOUTS=1
            shift
            ;;
        --allow-failures)
            ALLOW_FAILURES=1
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

mkdir -p "${OUTPUT_DIR}"

GRIPPER_SCRIPT="$(resolve_file "${GRIPPER_SCRIPT_OVERRIDE}" \
    "${PPMAIN_ROOT}/test/scripts/check_gripper_ack_log.py")"
HMI_SCRIPT="$(resolve_file "${HMI_SCRIPT_OVERRIDE}" \
    "${PPMAIN_ROOT}/test/scripts/check_hmi_event_log.py")"

if [[ -z "${LOG_FILE}" ]]; then
    LOG_FILE="${OUTPUT_DIR}/ugripper_service.log"
    print_section "Collect Service Log"
    journalctl -u ugripper.service --since "${JOURNAL_SINCE}" --no-pager -o short-precise -l > "${LOG_FILE}"
fi

[[ -f "${LOG_FILE}" ]] || { echo "missing log file: ${LOG_FILE}" >&2; exit 1; }

declare -a GRIPPER_ARGS=(
    --input "${LOG_FILE}"
    --json-out "${OUTPUT_DIR}/gripper_report.json"
)
declare -a HMI_ARGS=(
    --input "${LOG_FILE}"
    --json-out "${OUTPUT_DIR}/hmi_button_report.json"
)

if [[ "${ALLOW_TIMEOUTS}" == "1" ]]; then
    GRIPPER_ARGS+=(--allow-timeouts)
fi
if [[ "${ALLOW_FAILURES}" == "1" ]]; then
    GRIPPER_ARGS+=(--allow-failures)
fi

while IFS= read -r item; do
    [[ -n "${item}" ]] || continue
    GRIPPER_ARGS+=(--require-command "${item}")
done < <(split_csv_to_args "${REQUIRE_COMMAND_CSV}" "")

while IFS= read -r item; do
    [[ -n "${item}" ]] || continue
    HMI_ARGS+=(--require-event "${item}")
done < <(split_csv_to_args "${REQUIRE_EVENT_CSV}" "")

while IFS= read -r item; do
    [[ -n "${item}" ]] || continue
    HMI_ARGS+=(--require-led-state "${item}")
done < <(split_csv_to_args "${REQUIRE_LED_STATE_CSV}" "")

for item in "${REQUIRE_IO_TOTAL_ARGS[@]}"; do
    [[ -n "${item}" ]] || continue
    GRIPPER_ARGS+=(--require-io-total-at-least "${item}")
done

print_section "Check Gripper Diagnostic Log"
python3 "${GRIPPER_SCRIPT}" "${GRIPPER_ARGS[@]}"

print_section "Check HMI Diagnostic Log"
python3 "${HMI_SCRIPT}" "${HMI_ARGS[@]}"

print_section "Done"
echo "log_file=${LOG_FILE}"
echo "gripper_report=${OUTPUT_DIR}/gripper_report.json"
echo "hmi_button_report=${OUTPUT_DIR}/hmi_button_report.json"
