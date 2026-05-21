#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "${SCRIPT_DIR}/board_test_common.sh"

usage() {
    cat <<'EOF'
Usage: board_service_episode_loop_check.sh [options]

Options:
  --output-dir DIR            Output directory for per-episode reports
  --episode-root DIR          Episode root, default auto-detects /mnt/data_disk/*/data
  --cycles N                  Expected number of new episodes to observe (default: 3)
  --timeout-sec N             Max wait time per episode in seconds (default: 180)
  --validator-script PATH     Override board_service_integration_check.sh path
  --validator-journal-since T journalctl --since value for each validation run (default: 10 min ago)
  --checker-bin PATH          Override check_sensor_mcap binary path passed to validator
  --skip-validate             Only observe new episodes, do not run validator
  --keep-output               Keep output directory if present
  --help                      Show this help

This script is intended for manual start/stop loop validation:
start the script, then perform button-driven recording cycles on the device.
EOF
}

OUTPUT_DIR=""
EPISODE_ROOT=""
CYCLES=3
TIMEOUT_SEC=180
VALIDATOR_SCRIPT_OVERRIDE=""
VALIDATOR_JOURNAL_SINCE="10 min ago"
CHECKER_BIN_OVERRIDE=""
SKIP_VALIDATE=0
KEEP_OUTPUT=0

wait_for_episode_ready() {
    local episode_dir="$1"
    local timeout_sec="$2"
    local interval_sec=1
    local waited=0
    local required=(
        calibration.json
        info.json
        metadata.json
        cam_left.mkv
        cam_right.mkv
        stereo_left.mkv
        stereo_right.mkv
        tcam_left_l.mkv
        tcam_left_r.mkv
        tcam_right_l.mkv
        tcam_right_r.mkv
        sensor_left.mcap
        sensor_right.mcap
    )

    while (( waited < timeout_sec )); do
        local missing=0
        local name
        for name in "${required[@]}"; do
            if [[ ! -s "${episode_dir}/${name}" ]]; then
                missing=1
                break
            fi
        done
        if [[ "${missing}" == "0" ]]; then
            return 0
        fi
        sleep "${interval_sec}"
        waited=$((waited + interval_sec))
    done

    return 1
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --output-dir)
            OUTPUT_DIR="$2"
            shift 2
            ;;
        --episode-root)
            EPISODE_ROOT="$2"
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
        --validator-script)
            VALIDATOR_SCRIPT_OVERRIDE="$2"
            shift 2
            ;;
        --validator-journal-since)
            VALIDATOR_JOURNAL_SINCE="$2"
            shift 2
            ;;
        --checker-bin)
            CHECKER_BIN_OVERRIDE="$2"
            shift 2
            ;;
        --skip-validate)
            SKIP_VALIDATE=1
            shift
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
    OUTPUT_DIR="/tmp/pp_board_service_episode_loop_$(timestamp_slug)"
fi
if [[ -z "${EPISODE_ROOT}" ]]; then
    EPISODE_ROOT="$(find_default_episode_root)"
fi

if [[ "${KEEP_OUTPUT}" != "1" ]]; then
    rm -rf "${OUTPUT_DIR}"
fi
mkdir -p "${OUTPUT_DIR}"

VALIDATOR_SCRIPT="$(resolve_file "${VALIDATOR_SCRIPT_OVERRIDE}" \
    "${PPMAIN_ROOT}/test/scripts/board_service_integration_check.sh")"

print_section "Service Episode Loop Check"
echo "output_dir=${OUTPUT_DIR}"
echo "episode_root=${EPISODE_ROOT}"
echo "cycles=${CYCLES}"
echo "timeout_sec=${TIMEOUT_SEC}"
echo "skip_validate=${SKIP_VALIDATE}"

declare -A KNOWN_EPISODES=()
while IFS= read -r existing; do
    [[ -n "${existing}" ]] || continue
    KNOWN_EPISODES["${existing}"]=1
done < <(find "${EPISODE_ROOT}" -maxdepth 1 -mindepth 1 -type d -name 'episode_*' | sort)

SUMMARY_TXT="${OUTPUT_DIR}/loop_summary.txt"
: > "${SUMMARY_TXT}"

for ((cycle = 1; cycle <= CYCLES; ++cycle)); do
    print_section "Wait For Episode ${cycle}"
    echo "Please trigger one recording cycle on device now."
    new_episode=""
    start_ts="$(date +%s)"
    while [[ -z "${new_episode}" ]]; do
        while IFS= read -r candidate; do
            [[ -n "${candidate}" ]] || continue
            if [[ -z "${KNOWN_EPISODES[${candidate}]+x}" ]]; then
                new_episode="${candidate}"
                KNOWN_EPISODES["${candidate}"]=1
                break
            fi
        done < <(find "${EPISODE_ROOT}" -maxdepth 1 -mindepth 1 -type d -name 'episode_*' | sort)

        if [[ -n "${new_episode}" ]]; then
            break
        fi
        now_ts="$(date +%s)"
        if (( now_ts - start_ts >= TIMEOUT_SEC )); then
            echo "timed out waiting for new episode in cycle ${cycle}" >&2
            exit 1
        fi
        sleep 1
    done

    echo "cycle=${cycle} episode=${new_episode}" | tee -a "${SUMMARY_TXT}"

    if [[ "${SKIP_VALIDATE}" != "1" ]]; then
        print_section "Wait For Episode ${cycle} Artifacts"
        if ! wait_for_episode_ready "${new_episode}" 60; then
            echo "timed out waiting for episode artifacts: ${new_episode}" >&2
            exit 1
        fi
        per_output="${OUTPUT_DIR}/cycle_$(printf '%02d' "${cycle}")"
        declare -a VALIDATOR_ARGS=(
            --episode-dir "${new_episode}" \
            --output-dir "${per_output}" \
            --journal-since "${VALIDATOR_JOURNAL_SINCE}"
        )
        if [[ -n "${CHECKER_BIN_OVERRIDE}" ]]; then
            VALIDATOR_ARGS+=(--checker-bin "${CHECKER_BIN_OVERRIDE}")
        fi
        "${VALIDATOR_SCRIPT}" "${VALIDATOR_ARGS[@]}"
        echo "cycle=${cycle} validation_output=${per_output}" >> "${SUMMARY_TXT}"
    fi
done

print_section "Done"
echo "summary=${SUMMARY_TXT}"
