#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "${SCRIPT_DIR}/board_test_common.sh"

usage() {
    cat <<'EOF'
Usage: board_camera_stress.sh [options]

Options:
  --output-dir DIR         Output directory for stress artifacts
  --duration-sec N         Recording duration in seconds (default: 30)
  --cpu-workers N          Busy-loop worker count, default: max(nproc-1, 1)
  --ugripper-root DIR      Installed ugripper root, default prefers /opt/ugripper
  --camera-bin PATH        Override CameraRecorder binary path
  --config-yaml PATH       Override camera_recorder.yaml path
  --checker-script PATH    Override check_video_windows.py path
  --summary-script PATH    Override summarize_episode_reports.py path
  --window-sec N           Video check window size in seconds (default: 2)
  --min-frame-ratio R      Minimum allowed frame ratio (default: 0.5)
  --max-video-span-gap-sec N
                           Allowed main-camera span gap in seconds (default: 5.0)
  --only CSV               Camera names CSV (default: session cameras)
  --decode-check           Enable ffmpeg decode check for each window
  --keep-output            Keep existing output directory if present
  --help                   Show this help
EOF
}

OUTPUT_DIR=""
DURATION_SEC=30
DEFAULT_CPU_WORKERS="$(nproc 2>/dev/null || echo 4)"
if [[ "${DEFAULT_CPU_WORKERS}" -gt 1 ]]; then
    DEFAULT_CPU_WORKERS="$((DEFAULT_CPU_WORKERS - 1))"
fi
CPU_WORKERS="${DEFAULT_CPU_WORKERS}"
WINDOW_SEC=2
MIN_FRAME_RATIO=0.5
MAX_VIDEO_SPAN_GAP_SEC=5.0
ONLY_CSV="left_cam_main,right_cam_main,left_tcam_l,left_tcam_r,right_tcam_l,right_tcam_r"
DECODE_CHECK=0
KEEP_OUTPUT=0
UGRIPPER_ROOT="${UGRIPPER_ROOT:-$(default_ugripper_root)}"
CAMERA_BIN_OVERRIDE=""
CONFIG_YAML_OVERRIDE=""
CHECKER_SCRIPT_OVERRIDE=""
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
        --cpu-workers)
            CPU_WORKERS="$2"
            shift 2
            ;;
        --ugripper-root)
            UGRIPPER_ROOT="$2"
            shift 2
            ;;
        --camera-bin)
            CAMERA_BIN_OVERRIDE="$2"
            shift 2
            ;;
        --config-yaml)
            CONFIG_YAML_OVERRIDE="$2"
            shift 2
            ;;
        --checker-script)
            CHECKER_SCRIPT_OVERRIDE="$2"
            shift 2
            ;;
        --summary-script)
            SUMMARY_SCRIPT_OVERRIDE="$2"
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
        --max-video-span-gap-sec)
            MAX_VIDEO_SPAN_GAP_SEC="$2"
            shift 2
            ;;
        --only)
            ONLY_CSV="$2"
            shift 2
            ;;
        --decode-check)
            DECODE_CHECK=1
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
    OUTPUT_DIR="/tmp/pp_board_camera_stress_$(timestamp_slug)"
fi

CAMERA_BIN="$(resolve_executable "${CAMERA_BIN_OVERRIDE}" \
    "${UGRIPPER_ROOT}/bin/CameraRecorder/CameraRecorder" \
    "${PPMAIN_ROOT}/build/x86/standalone_ros2/CameraRecorder/CameraRecorder")"
CONFIG_YAML="$(resolve_file "${CONFIG_YAML_OVERRIDE}" \
    "${UGRIPPER_ROOT}/bin/CameraRecorder/config/camera_recorder.yaml" \
    "${PPMAIN_ROOT}/standalone/CameraRecorder/config/camera_recorder.yaml")"
CHECKER_SCRIPT="$(resolve_file "${CHECKER_SCRIPT_OVERRIDE}" \
    "${PPMAIN_ROOT}/test/scripts/check_video_windows.py")"
SUMMARY_SCRIPT="$(resolve_file "${SUMMARY_SCRIPT_OVERRIDE}" \
    "${PPMAIN_ROOT}/test/scripts/summarize_episode_reports.py")"

if [[ "${KEEP_OUTPUT}" != "1" ]]; then
    rm -rf "${OUTPUT_DIR}"
fi
mkdir -p "${OUTPUT_DIR}"

RUN_LOG="${OUTPUT_DIR}/camera_stress_run.log"
declare -a CPU_PIDS=()

cleanup() {
    local pid
    for pid in "${CPU_PIDS[@]:-}"; do
        kill "${pid}" >/dev/null 2>&1 || true
    done
}
trap cleanup EXIT

print_section "Camera Stress"
echo "output_dir=${OUTPUT_DIR}"
echo "camera_bin=${CAMERA_BIN}"
echo "config_yaml=${CONFIG_YAML}"
echo "checker_script=${CHECKER_SCRIPT}"
echo "summary_script=${SUMMARY_SCRIPT}"
echo "duration_sec=${DURATION_SEC}"
echo "cpu_workers=${CPU_WORKERS}"
echo "only=${ONLY_CSV}"

if [[ "${CPU_WORKERS}" -gt 0 ]]; then
    print_section "Start CPU Load"
    for ((i = 0; i < CPU_WORKERS; ++i)); do
        bash -lc 'while :; do :; done' &
        CPU_PIDS+=("$!")
    done
    echo "started_cpu_workers=${#CPU_PIDS[@]}"
fi

set +e
timeout -s INT "${DURATION_SEC}s" "${CAMERA_BIN}" \
    --output-dir "${OUTPUT_DIR}" \
    --config-yaml "${CONFIG_YAML}" \
    --codec h265 \
    --only "${ONLY_CSV}" >"${RUN_LOG}" 2>&1
CAMERA_EXIT=$?
set -e

if ! accept_timeout_exit "${CAMERA_EXIT}"; then
    echo "CameraRecorder exited unexpectedly: ${CAMERA_EXIT}" >&2
    tail -n 50 "${RUN_LOG}" >&2 || true
    exit "${CAMERA_EXIT}"
fi

LEFT_MAIN="${OUTPUT_DIR}/left_cam_main.mkv"
RIGHT_MAIN="${OUTPUT_DIR}/right_cam_main.mkv"
[[ -f "${LEFT_MAIN}" ]] || { echo "missing ${LEFT_MAIN}" >&2; exit 1; }
[[ -f "${RIGHT_MAIN}" ]] || { echo "missing ${RIGHT_MAIN}" >&2; exit 1; }

declare -a EXTRA_FLAGS=()
if [[ "${DECODE_CHECK}" == "1" ]]; then
    EXTRA_FLAGS+=(--decode-check)
fi

print_section "Check Left Main Camera"
python3 "${CHECKER_SCRIPT}" \
    --input "${LEFT_MAIN}" \
    --window-sec "${WINDOW_SEC}" \
    --min-frame-ratio "${MIN_FRAME_RATIO}" \
    --json-out "${OUTPUT_DIR}/video_report_left_cam_main.json" \
    "${EXTRA_FLAGS[@]}"

print_section "Check Right Main Camera"
python3 "${CHECKER_SCRIPT}" \
    --input "${RIGHT_MAIN}" \
    --window-sec "${WINDOW_SEC}" \
    --min-frame-ratio "${MIN_FRAME_RATIO}" \
    --json-out "${OUTPUT_DIR}/video_report_right_cam_main.json" \
    "${EXTRA_FLAGS[@]}"

print_section "Summarize Reports"
python3 "${SUMMARY_SCRIPT}" \
    --video-report "${OUTPUT_DIR}/video_report_left_cam_main.json" \
    --video-report "${OUTPUT_DIR}/video_report_right_cam_main.json" \
    --max-video-span-gap-sec "${MAX_VIDEO_SPAN_GAP_SEC}" \
    --json-out "${OUTPUT_DIR}/test_summary.json"

print_section "Done"
echo "run_log=${RUN_LOG}"
echo "left_report=${OUTPUT_DIR}/video_report_left_cam_main.json"
echo "right_report=${OUTPUT_DIR}/video_report_right_cam_main.json"
echo "test_summary=${OUTPUT_DIR}/test_summary.json"
