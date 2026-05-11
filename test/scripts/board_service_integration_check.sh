#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "${SCRIPT_DIR}/board_test_common.sh"

usage() {
    cat <<'EOF'
Usage: board_service_integration_check.sh [options]

Options:
  --output-dir DIR            Output directory for collected artifacts and reports
  --episode-dir DIR           Episode directory to validate, default picks latest episode_*
  --episode-root DIR          Episode root, default auto-detects /mnt/data_disk/*/data
  --log-file PATH             Existing ugripper service log, default collects via journalctl
  --journal-since TEXT        journalctl --since value when log-file is omitted
  --stereo-status PATH        Stereo status json path (default: /tmp/umi_stereo_camera_status.json)
  --checker-bin PATH          Override check_sensor_mcap binary path
  --video-script PATH         Override check_video_windows.py path
  --gripper-script PATH       Override check_gripper_ack_log.py path
  --hmi-script PATH           Override check_hmi_event_log.py path
  --summary-script PATH       Override summarize_episode_reports.py path
  --window-sec N              Video check window size in seconds (default: 2)
  --min-frame-ratio R         Minimum allowed frame ratio (default: 0.5)
  --max-sensor-span-gap-ns N  Optional allowed left/right sensor span gap
  --max-video-span-gap-sec N  Allowed main-camera span gap in seconds (default: 5.0)
  --require-gripper-io-total-at-least FIELD:COUNT
                              Require aggregated gripper io summary FIELD >= COUNT; repeatable
  --skip-systemctl            Skip systemctl status/is-active checks
  --skip-sensor-check         Skip sensor MCAP analyzer execution
  --skip-video-check          Skip main camera video analyzer execution
  --skip-gripper-hmi          Skip gripper/HMI log analysis
  --allow-validation-failed   Do not fail on validation_failed in service log
  --keep-output               Keep output directory if present
  --help                      Show this help
EOF
}

OUTPUT_DIR=""
EPISODE_DIR=""
EPISODE_ROOT=""
LOG_FILE=""
JOURNAL_SINCE="20 min ago"
STEREO_STATUS_FILE="/tmp/umi_stereo_camera_status.json"
WINDOW_SEC=2
MIN_FRAME_RATIO=0.5
MAX_SENSOR_SPAN_GAP_NS=""
MAX_VIDEO_SPAN_GAP_SEC=5.0
declare -a REQUIRE_GRIPPER_IO_TOTAL_ARGS=()
SKIP_SYSTEMCTL=0
SKIP_SENSOR_CHECK=0
SKIP_VIDEO_CHECK=0
SKIP_GRIPPER_HMI=0
ALLOW_VALIDATION_FAILED=0
KEEP_OUTPUT=0
CHECKER_BIN_OVERRIDE=""
VIDEO_SCRIPT_OVERRIDE=""
GRIPPER_SCRIPT_OVERRIDE=""
HMI_SCRIPT_OVERRIDE=""
SUMMARY_SCRIPT_OVERRIDE=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --output-dir)
            OUTPUT_DIR="$2"
            shift 2
            ;;
        --episode-dir)
            EPISODE_DIR="$2"
            shift 2
            ;;
        --episode-root)
            EPISODE_ROOT="$2"
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
        --stereo-status)
            STEREO_STATUS_FILE="$2"
            shift 2
            ;;
        --checker-bin)
            CHECKER_BIN_OVERRIDE="$2"
            shift 2
            ;;
        --video-script)
            VIDEO_SCRIPT_OVERRIDE="$2"
            shift 2
            ;;
        --summary-script)
            SUMMARY_SCRIPT_OVERRIDE="$2"
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
        --window-sec)
            WINDOW_SEC="$2"
            shift 2
            ;;
        --min-frame-ratio)
            MIN_FRAME_RATIO="$2"
            shift 2
            ;;
        --max-sensor-span-gap-ns)
            MAX_SENSOR_SPAN_GAP_NS="$2"
            shift 2
            ;;
        --max-video-span-gap-sec)
            MAX_VIDEO_SPAN_GAP_SEC="$2"
            shift 2
            ;;
        --require-gripper-io-total-at-least)
            REQUIRE_GRIPPER_IO_TOTAL_ARGS+=("$2")
            shift 2
            ;;
        --skip-systemctl)
            SKIP_SYSTEMCTL=1
            shift
            ;;
        --skip-sensor-check)
            SKIP_SENSOR_CHECK=1
            shift
            ;;
        --skip-video-check)
            SKIP_VIDEO_CHECK=1
            shift
            ;;
        --skip-gripper-hmi)
            SKIP_GRIPPER_HMI=1
            shift
            ;;
        --allow-validation-failed)
            ALLOW_VALIDATION_FAILED=1
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
    OUTPUT_DIR="/tmp/pp_board_service_integration_$(timestamp_slug)"
fi

if [[ "${KEEP_OUTPUT}" != "1" ]]; then
    rm -rf "${OUTPUT_DIR}"
fi
mkdir -p "${OUTPUT_DIR}"

CHECKER_BIN="$(default_check_sensor_mcap_bin "${CHECKER_BIN_OVERRIDE}")"
VIDEO_SCRIPT="$(resolve_file "${VIDEO_SCRIPT_OVERRIDE}" \
    "${PPMAIN_ROOT}/test/scripts/check_video_windows.py")"
GRIPPER_SCRIPT="$(resolve_file "${GRIPPER_SCRIPT_OVERRIDE}" \
    "${PPMAIN_ROOT}/test/scripts/check_gripper_ack_log.py")"
HMI_SCRIPT="$(resolve_file "${HMI_SCRIPT_OVERRIDE}" \
    "${PPMAIN_ROOT}/test/scripts/check_hmi_event_log.py")"
SUMMARY_SCRIPT="$(resolve_file "${SUMMARY_SCRIPT_OVERRIDE}" \
    "${PPMAIN_ROOT}/test/scripts/summarize_episode_reports.py")"

if [[ -z "${EPISODE_DIR}" ]]; then
    if [[ -z "${EPISODE_ROOT}" ]]; then
        EPISODE_ROOT="$(find_default_episode_root)"
    fi
    EPISODE_DIR="$(find_latest_episode_dir "${EPISODE_ROOT}")"
fi

[[ -d "${EPISODE_DIR}" ]] || { echo "missing episode_dir: ${EPISODE_DIR}" >&2; exit 1; }

if [[ -z "${LOG_FILE}" ]]; then
    LOG_FILE="${OUTPUT_DIR}/ugripper_service.log"
    journalctl -u ugripper.service --since "${JOURNAL_SINCE}" --no-pager -o short-precise -l > "${LOG_FILE}"
fi
[[ -f "${LOG_FILE}" ]] || { echo "missing log file: ${LOG_FILE}" >&2; exit 1; }

print_section "Service Integration Check"
echo "output_dir=${OUTPUT_DIR}"
echo "episode_dir=${EPISODE_DIR}"
echo "log_file=${LOG_FILE}"

if [[ "${SKIP_SYSTEMCTL}" != "1" ]]; then
    print_section "Collect Service Status"
    systemctl status ugripper.service --no-pager -l > "${OUTPUT_DIR}/systemctl_status.txt"
    systemctl is-active ugripper.service > "${OUTPUT_DIR}/systemctl_is_active.txt"
fi

if [[ -f "${STEREO_STATUS_FILE}" ]]; then
    cp "${STEREO_STATUS_FILE}" "${OUTPUT_DIR}/stereo_status.json"
fi

if [[ "${ALLOW_VALIDATION_FAILED}" != "1" ]]; then
    if grep -q 'validation_failed' "${LOG_FILE}"; then
        echo "validation_failed detected in service log" >&2
        exit 1
    fi
fi

if grep -q 'episode validation failed' "${LOG_FILE}"; then
    echo "episode validation failed detected in service log" >&2
    exit 1
fi

REQUIRED_FILES=(
    calibration.json
    info.json
    metadata.json
    left_cam_main.mkv
    right_cam_main.mkv
    left_stereo.mkv
    right_stereo.mkv
    left_tcam_l.mkv
    left_tcam_r.mkv
    right_tcam_l.mkv
    right_tcam_r.mkv
    sensor_data_left.mcap
    sensor_data_right.mcap
)

print_section "Check Episode Files"
for name in "${REQUIRED_FILES[@]}"; do
    path="${EPISODE_DIR}/${name}"
    if [[ ! -s "${path}" ]]; then
        echo "missing or empty episode artifact: ${path}" >&2
        exit 1
    fi
    printf '%s\n' "${path}" >> "${OUTPUT_DIR}/episode_files.txt"
done

if [[ "${SKIP_SENSOR_CHECK}" != "1" ]]; then
    print_section "Check Sensor MCAP"
    "${CHECKER_BIN}" \
        --expect-topic encoder_left \
        --min-message-count 2 \
        --json-out "${OUTPUT_DIR}/sensor_report_left.json" \
        "${EPISODE_DIR}/sensor_data_left.mcap"
    "${CHECKER_BIN}" \
        --expect-topic encoder_right \
        --min-message-count 2 \
        --json-out "${OUTPUT_DIR}/sensor_report_right.json" \
        "${EPISODE_DIR}/sensor_data_right.mcap"
fi

if [[ "${SKIP_VIDEO_CHECK}" != "1" ]]; then
    print_section "Check Main Camera Videos"
    python3 "${VIDEO_SCRIPT}" \
        --input "${EPISODE_DIR}/left_cam_main.mkv" \
        --window-sec "${WINDOW_SEC}" \
        --min-frame-ratio "${MIN_FRAME_RATIO}" \
        --json-out "${OUTPUT_DIR}/video_report_left_cam_main.json"
    python3 "${VIDEO_SCRIPT}" \
        --input "${EPISODE_DIR}/right_cam_main.mkv" \
        --window-sec "${WINDOW_SEC}" \
        --min-frame-ratio "${MIN_FRAME_RATIO}" \
        --json-out "${OUTPUT_DIR}/video_report_right_cam_main.json"
fi

if [[ "${SKIP_GRIPPER_HMI}" != "1" ]]; then
    print_section "Check Gripper/HMI Logs"
    declare -a GRIPPER_ARGS=(
        --input "${LOG_FILE}"
        --json-out "${OUTPUT_DIR}/gripper_report.json"
    )
    for item in "${REQUIRE_GRIPPER_IO_TOTAL_ARGS[@]}"; do
        [[ -n "${item}" ]] || continue
        GRIPPER_ARGS+=(--require-io-total-at-least "${item}")
    done
    python3 "${GRIPPER_SCRIPT}" "${GRIPPER_ARGS[@]}"
    python3 "${HMI_SCRIPT}" \
        --input "${LOG_FILE}" \
        --json-out "${OUTPUT_DIR}/hmi_button_report.json"
fi

print_section "Summarize Reports"
declare -a SUMMARY_ARGS=(
    --json-out "${OUTPUT_DIR}/episode_summary.json"
    --max-video-span-gap-sec "${MAX_VIDEO_SPAN_GAP_SEC}"
)
if [[ "${SKIP_SENSOR_CHECK}" != "1" ]]; then
    SUMMARY_ARGS+=(
        --sensor-report "${OUTPUT_DIR}/sensor_report_left.json"
        --sensor-report "${OUTPUT_DIR}/sensor_report_right.json"
    )
    if [[ -n "${MAX_SENSOR_SPAN_GAP_NS}" ]]; then
        SUMMARY_ARGS+=(--max-sensor-span-gap-ns "${MAX_SENSOR_SPAN_GAP_NS}")
    fi
fi
if [[ "${SKIP_VIDEO_CHECK}" != "1" ]]; then
    SUMMARY_ARGS+=(
        --video-report "${OUTPUT_DIR}/video_report_left_cam_main.json"
        --video-report "${OUTPUT_DIR}/video_report_right_cam_main.json"
    )
fi
if [[ "${SKIP_GRIPPER_HMI}" != "1" ]]; then
    SUMMARY_ARGS+=(
        --gripper-report "${OUTPUT_DIR}/gripper_report.json"
        --hmi-report "${OUTPUT_DIR}/hmi_button_report.json"
    )
fi
python3 "${SUMMARY_SCRIPT}" "${SUMMARY_ARGS[@]}"

print_section "Done"
echo "episode_files=${OUTPUT_DIR}/episode_files.txt"
if [[ "${SKIP_SENSOR_CHECK}" != "1" ]]; then
    echo "sensor_report_left=${OUTPUT_DIR}/sensor_report_left.json"
    echo "sensor_report_right=${OUTPUT_DIR}/sensor_report_right.json"
fi
if [[ "${SKIP_VIDEO_CHECK}" != "1" ]]; then
    echo "video_report_left=${OUTPUT_DIR}/video_report_left_cam_main.json"
    echo "video_report_right=${OUTPUT_DIR}/video_report_right_cam_main.json"
fi
if [[ "${SKIP_GRIPPER_HMI}" != "1" ]]; then
    echo "gripper_report=${OUTPUT_DIR}/gripper_report.json"
    echo "hmi_button_report=${OUTPUT_DIR}/hmi_button_report.json"
fi
echo "episode_summary=${OUTPUT_DIR}/episode_summary.json"
