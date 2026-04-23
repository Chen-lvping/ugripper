#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "${SCRIPT_DIR}/board_test_common.sh"

usage() {
    cat <<'EOF'
Usage: board_gripper_hmi_active_check.sh [options]

Options:
  --output-dir DIR               Output directory for logs and reports
  --ugripper-root DIR            Ugripper install root (default: /opt/ugripper when present)
  --service-name NAME            systemd service name (default: ugripper.service)
  --gripper-hmi-bin PATH         Override GripperHmiTool binary
  --restart-check-script PATH    Override board_service_restart_check.sh path
  --log-check-script PATH        Override board_gripper_hmi_log_check.sh path
  --skip-direct-hmi              Skip direct HMI state/beep stimulus phase
  --skip-button-phase            Skip service-driven button interaction phase
  --non-interactive              Do not wait for Enter; use sleep windows instead
  --button-window-sec N          Wait window for button interactions in non-interactive mode (default: 20)
  --direct-step-sec N            Duration for each direct HMI state stimulus (default: 2)
  --beep-ms N                    Direct HMI beep duration in ms (default: 300)
  --require-command CSV          Required gripper commands for log analysis
  --require-event CSV            Required HMI button events for log analysis
  --require-led-state CSV        Required HMI led states for log analysis
  --require-io-total-at-least FIELD:COUNT
                               Require aggregated gripper io summary FIELD >= COUNT; repeatable
  --allow-timeouts               Allow gripper command timeouts in log analysis
  --allow-failures               Allow gripper command failures in log analysis
  --help                         Show this help
EOF
}

OUTPUT_DIR="/tmp/pp_board_gripper_hmi_active_check_$(timestamp_slug)"
UGRIPPER_ROOT="$(default_ugripper_root)"
SERVICE_NAME="ugripper.service"
GRIPPER_HMI_BIN_OVERRIDE=""
RESTART_CHECK_SCRIPT_OVERRIDE=""
LOG_CHECK_SCRIPT_OVERRIDE=""
SKIP_DIRECT_HMI=0
SKIP_BUTTON_PHASE=0
NON_INTERACTIVE=0
BUTTON_WINDOW_SEC=20
DIRECT_STEP_SEC=2
BEEP_MS=300
REQUIRE_COMMAND_CSV=""
REQUIRE_EVENT_CSV="ShortUpPressed,ShortDownPressed,ShutdownPromptRequested"
REQUIRE_LED_STATE_CSV="Ready,Recording"
declare -a REQUIRE_IO_TOTAL_ARGS=()
ALLOW_TIMEOUTS=0
ALLOW_FAILURES=0
SERVICE_STOPPED_FOR_DIRECT_HMI=0
SERVICE_RESTARTED_AFTER_DIRECT_HMI=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --output-dir)
            OUTPUT_DIR="$2"
            shift 2
            ;;
        --ugripper-root)
            UGRIPPER_ROOT="$2"
            shift 2
            ;;
        --service-name)
            SERVICE_NAME="$2"
            shift 2
            ;;
        --gripper-hmi-bin)
            GRIPPER_HMI_BIN_OVERRIDE="$2"
            shift 2
            ;;
        --restart-check-script)
            RESTART_CHECK_SCRIPT_OVERRIDE="$2"
            shift 2
            ;;
        --log-check-script)
            LOG_CHECK_SCRIPT_OVERRIDE="$2"
            shift 2
            ;;
        --skip-direct-hmi)
            SKIP_DIRECT_HMI=1
            shift
            ;;
        --skip-button-phase)
            SKIP_BUTTON_PHASE=1
            shift
            ;;
        --non-interactive)
            NON_INTERACTIVE=1
            shift
            ;;
        --button-window-sec)
            BUTTON_WINDOW_SEC="$2"
            shift 2
            ;;
        --direct-step-sec)
            DIRECT_STEP_SEC="$2"
            shift 2
            ;;
        --beep-ms)
            BEEP_MS="$2"
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

GRIPPER_HMI_BIN="$(resolve_executable "${GRIPPER_HMI_BIN_OVERRIDE}" \
    "${UGRIPPER_ROOT}/bin/GripperHmiTool/GripperHmiTool" \
    "${PPMAIN_ROOT}/build/x86/standalone_ros2/GripperHmiTool/GripperHmiTool")"
RESTART_CHECK_SCRIPT=""
LOG_CHECK_SCRIPT=""

if [[ "${SKIP_BUTTON_PHASE}" != "1" ]]; then
    RESTART_CHECK_SCRIPT="$(resolve_file "${RESTART_CHECK_SCRIPT_OVERRIDE}" \
        "${PPMAIN_ROOT}/test/scripts/board_service_restart_check.sh")"
    LOG_CHECK_SCRIPT="$(resolve_file "${LOG_CHECK_SCRIPT_OVERRIDE}" \
        "${PPMAIN_ROOT}/test/scripts/board_gripper_hmi_log_check.sh")"
fi

VISUAL_REPORT="${OUTPUT_DIR}/direct_hmi_visual.txt"
: > "${VISUAL_REPORT}"

cleanup_service_restart() {
    if [[ "${SERVICE_STOPPED_FOR_DIRECT_HMI}" == "1" && "${SERVICE_RESTARTED_AFTER_DIRECT_HMI}" != "1" ]]; then
        echo "[INFO] restoring ${SERVICE_NAME} after direct HMI phase"
        systemctl start "${SERVICE_NAME}" >/dev/null 2>&1 || true
    fi
}

trap cleanup_service_restart EXIT

record_visual_result() {
    local step="$1"
    local status="$2"
    printf 'step=%s status=%s\n' "${step}" "${status}" >> "${VISUAL_REPORT}"
}

confirm_visual_step() {
    local step="$1"
    local prompt_text="$2"

    if [[ "${NON_INTERACTIVE}" == "1" ]]; then
        echo "[INFO] non-interactive mode: visual confirmation skipped for ${step}"
        record_visual_result "${step}" "not_checked"
        return 0
    fi

    local answer
    read -r -p "${prompt_text} [y/N]: " answer
    case "${answer}" in
        y|Y|yes|YES)
            record_visual_result "${step}" "confirmed"
            ;;
        *)
            record_visual_result "${step}" "failed"
            echo "visual confirmation failed for ${step}" >&2
            exit 1
            ;;
    esac
}

run_direct_hmi_step() {
    local name="$1"
    local prompt_text="$2"
    shift 2

    local output_file="${OUTPUT_DIR}/direct_${name}.log"
    print_section "Direct HMI ${name}"
    "${GRIPPER_HMI_BIN}" "$@" > "${output_file}" 2>&1
    confirm_visual_step "${name}" "${prompt_text}"
}

if [[ "${SKIP_DIRECT_HMI}" != "1" ]]; then
    print_section "Stop Service For Direct HMI"
    systemctl stop "${SERVICE_NAME}"
    SERVICE_STOPPED_FOR_DIRECT_HMI=1

    run_direct_hmi_step \
        "ready" \
        "Did both grippers show READY during the last ${DIRECT_STEP_SEC}s?" \
        --state READY \
        --led-only \
        --duration "${DIRECT_STEP_SEC}" \
        --poll-ms 100

    run_direct_hmi_step \
        "recording" \
        "Did both grippers show RECORDING during the last ${DIRECT_STEP_SEC}s?" \
        --state RECORDING \
        --led-only \
        --duration "${DIRECT_STEP_SEC}" \
        --poll-ms 100

    run_direct_hmi_step \
        "error_1" \
        "Did both grippers show ERROR_1 during the last ${DIRECT_STEP_SEC}s?" \
        --state ERROR_1 \
        --led-only \
        --duration "${DIRECT_STEP_SEC}" \
        --poll-ms 100

    run_direct_hmi_step \
        "beep" \
        "Did both grippers produce the expected beep?" \
        --beep \
        --beep-ms "${BEEP_MS}" \
        --duration 1 \
        --poll-ms 100
fi

if [[ "${SKIP_BUTTON_PHASE}" != "1" ]]; then
    print_section "Restart Service Before Button Phase"
    bash "${RESTART_CHECK_SCRIPT}" \
        --output-dir "${OUTPUT_DIR}/service_restart" \
        --service-name "${SERVICE_NAME}" \
        --journal-since "5 min ago"
    SERVICE_RESTARTED_AFTER_DIRECT_HMI=1

    BUTTON_LOG_SINCE="$(date '+%Y-%m-%d %H:%M:%S')"
    BUTTON_INSTRUCTIONS="${OUTPUT_DIR}/button_instructions.txt"
    cat > "${BUTTON_INSTRUCTIONS}" <<'EOF'
Button interaction sequence:
1. Short press UP once to trigger start recording.
2. Wait for recording indication.
3. Short press DOWN once to trigger stop recording.
4. Hold both buttons until shutdown prompt is emitted, then release before actual shutdown.
EOF

    print_section "Button Interaction"
    cat "${BUTTON_INSTRUCTIONS}"
    if [[ "${NON_INTERACTIVE}" == "1" ]]; then
        echo "[INFO] waiting ${BUTTON_WINDOW_SEC}s for manual button interaction"
        sleep "${BUTTON_WINDOW_SEC}"
    else
        read -r -p "Perform the sequence now, then press Enter when complete." _
    fi

    LOG_CHECK_ARGS=(
        --output-dir "${OUTPUT_DIR}/log_check"
        --journal-since "${BUTTON_LOG_SINCE}"
    )
    if [[ -n "${REQUIRE_COMMAND_CSV}" ]]; then
        LOG_CHECK_ARGS+=(--require-command "${REQUIRE_COMMAND_CSV}")
    fi
    if [[ -n "${REQUIRE_EVENT_CSV}" ]]; then
        LOG_CHECK_ARGS+=(--require-event "${REQUIRE_EVENT_CSV}")
    fi
    if [[ -n "${REQUIRE_LED_STATE_CSV}" ]]; then
        LOG_CHECK_ARGS+=(--require-led-state "${REQUIRE_LED_STATE_CSV}")
    fi
    if [[ "${ALLOW_TIMEOUTS}" == "1" ]]; then
        LOG_CHECK_ARGS+=(--allow-timeouts)
    fi
    if [[ "${ALLOW_FAILURES}" == "1" ]]; then
        LOG_CHECK_ARGS+=(--allow-failures)
    fi
    for item in "${REQUIRE_IO_TOTAL_ARGS[@]}"; do
        [[ -n "${item}" ]] || continue
        LOG_CHECK_ARGS+=(--require-io-total-at-least "${item}")
    done

    print_section "Analyze Service Log"
    bash "${LOG_CHECK_SCRIPT}" "${LOG_CHECK_ARGS[@]}"
elif [[ "${SERVICE_STOPPED_FOR_DIRECT_HMI}" == "1" ]]; then
    print_section "Restore Service After Direct HMI"
    systemctl start "${SERVICE_NAME}"
    SERVICE_RESTARTED_AFTER_DIRECT_HMI=1
fi

print_section "Done"
echo "output_dir=${OUTPUT_DIR}"
echo "direct_visual_report=${VISUAL_REPORT}"
if [[ "${SKIP_BUTTON_PHASE}" != "1" ]]; then
    echo "service_restart_dir=${OUTPUT_DIR}/service_restart"
    echo "gripper_report=${OUTPUT_DIR}/log_check/gripper_report.json"
    echo "hmi_button_report=${OUTPUT_DIR}/log_check/hmi_button_report.json"
fi
