#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "${SCRIPT_DIR}/board_test_common.sh"

usage() {
    cat <<'EOF'
Usage: board_gripper_hmi_fault_soak.sh [options]

Options:
  --output-dir DIR              Output directory for per-cycle artifacts and soak summary
  --cycles N                    Expected number of manual recording cycles (default: 5)
  --timeout-sec N               Max wait time per cycle in seconds (default: 180)
  --episode-root DIR            Episode root, default auto-detects /mnt/data_disk/*/data
  --loop-script PATH            Override board_service_episode_loop_check.sh path
  --summary-script PATH         Override summarize_runtime_health_soak.py path
  --max-health-fault KEY=N      Allow at most N occurrences of a health fault key; repeatable
  --max-total-health-faults N   Allow at most N total health faults
  --help                        Show this help

This script is intended for manual soak:
start the script, then repeat button-driven start/stop cycles on the device.
EOF
}

OUTPUT_DIR=""
CYCLES=5
TIMEOUT_SEC=180
EPISODE_ROOT=""
LOOP_SCRIPT_OVERRIDE=""
SUMMARY_SCRIPT_OVERRIDE=""
MAX_TOTAL_HEALTH_FAULTS=""
declare -a MAX_HEALTH_FAULT_ARGS=()

while [[ $# -gt 0 ]]; do
    case "$1" in
        --output-dir)
            OUTPUT_DIR="$2"
            shift 2
            ;;
        --cycles)
            CYCLES="$2"
            shift 2
            ;;
        --timeout-sec)
            TIMEOUT_SEC="$2"
            shift 2
            ;;
        --episode-root)
            EPISODE_ROOT="$2"
            shift 2
            ;;
        --loop-script)
            LOOP_SCRIPT_OVERRIDE="$2"
            shift 2
            ;;
        --summary-script)
            SUMMARY_SCRIPT_OVERRIDE="$2"
            shift 2
            ;;
        --max-health-fault)
            MAX_HEALTH_FAULT_ARGS+=("$2")
            shift 2
            ;;
        --max-total-health-faults)
            MAX_TOTAL_HEALTH_FAULTS="$2"
            shift 2
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
    OUTPUT_DIR="/tmp/pp_board_gripper_hmi_fault_soak_$(timestamp_slug)"
fi
if [[ -z "${EPISODE_ROOT}" ]]; then
    EPISODE_ROOT="$(find_default_episode_root)"
fi
rm -rf "${OUTPUT_DIR}"
mkdir -p "${OUTPUT_DIR}"

LOOP_SCRIPT="$(resolve_file "${LOOP_SCRIPT_OVERRIDE}" \
    "${PPMAIN_ROOT}/test/scripts/board_service_episode_loop_check.sh")"
SUMMARY_SCRIPT="$(resolve_file "${SUMMARY_SCRIPT_OVERRIDE}" \
    "${PPMAIN_ROOT}/test/scripts/summarize_runtime_health_soak.py")"

JOURNAL_SINCE="$(date '+%Y-%m-%d %H:%M:%S')"

print_section "Gripper/HMI Fault Soak"
echo "output_dir=${OUTPUT_DIR}"
echo "cycles=${CYCLES}"
echo "timeout_sec=${TIMEOUT_SEC}"
echo "episode_root=${EPISODE_ROOT}"
echo "journal_since=${JOURNAL_SINCE}"
echo "Please perform ${CYCLES} button-driven recording cycles now."

declare -a LOOP_ARGS=(
    --output-dir "${OUTPUT_DIR}/loop"
    --episode-root "${EPISODE_ROOT}"
    --cycles "${CYCLES}"
    --timeout-sec "${TIMEOUT_SEC}"
    --validator-journal-since "${JOURNAL_SINCE}"
)

"${LOOP_SCRIPT}" "${LOOP_ARGS[@]}"

SERVICE_LOG="${OUTPUT_DIR}/ugripper_service.log"
journalctl -u ugripper.service --since "${JOURNAL_SINCE}" --no-pager -o short-precise -l > "${SERVICE_LOG}"

declare -a SUMMARY_ARGS=(
    --input "${SERVICE_LOG}"
    --json-out "${OUTPUT_DIR}/health_soak_summary.json"
    --min-validations "${CYCLES}"
    --min-recording-starts "${CYCLES}"
)
if [[ -n "${MAX_TOTAL_HEALTH_FAULTS}" ]]; then
    SUMMARY_ARGS+=(--max-total-health-faults "${MAX_TOTAL_HEALTH_FAULTS}")
fi
for item in "${MAX_HEALTH_FAULT_ARGS[@]}"; do
    SUMMARY_ARGS+=(--max-health-fault "${item}")
done

python3 "${SUMMARY_SCRIPT}" "${SUMMARY_ARGS[@]}"

print_section "Done"
echo "loop_dir=${OUTPUT_DIR}/loop"
echo "service_log=${SERVICE_LOG}"
echo "health_soak_summary=${OUTPUT_DIR}/health_soak_summary.json"
