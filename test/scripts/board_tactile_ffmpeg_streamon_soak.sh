#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "${SCRIPT_DIR}/board_test_common.sh"

usage() {
    cat <<'EOF'
Usage: board_tactile_ffmpeg_streamon_soak.sh [options]

Repeatedly opens one tactile V4L2 node with production input parameters,
captures for a short cycle, closes it, and records stream-start failures.

Options:
  --device PATH          V4L2 device (default: /dev/tcam_left_l)
  --duration-sec N       Total soak duration (default: 1800)
  --cycle-sec N          Capture duration per open/close cycle (default: 5)
  --min-frame-ratio R    Low-frame threshold versus 120 fps (default: 0.75)
  --output-dir DIR       Result directory under /dev/shm by default
  --service-name NAME    Service stopped during direct access (default: ugripper.service)
  --ffmpeg-bin PATH      Override ffmpeg binary
  --help                 Show this help
EOF
}

DEVICE="/dev/tcam_left_l"
DURATION_SEC=1800
CYCLE_SEC=5
MIN_FRAME_RATIO=0.75
OUTPUT_DIR=""
SERVICE_NAME="ugripper.service"
FFMPEG_BIN=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --device)
            DEVICE="$2"
            shift 2
            ;;
        --duration-sec)
            DURATION_SEC="$2"
            shift 2
            ;;
        --cycle-sec)
            CYCLE_SEC="$2"
            shift 2
            ;;
        --min-frame-ratio)
            MIN_FRAME_RATIO="$2"
            shift 2
            ;;
        --output-dir)
            OUTPUT_DIR="$2"
            shift 2
            ;;
        --service-name)
            SERVICE_NAME="$2"
            shift 2
            ;;
        --ffmpeg-bin)
            FFMPEG_BIN="$2"
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

[[ "${DURATION_SEC}" =~ ^[1-9][0-9]*$ ]] || { echo "duration-sec must be a positive integer" >&2; exit 2; }
[[ "${CYCLE_SEC}" =~ ^[1-9][0-9]*$ ]] || { echo "cycle-sec must be a positive integer" >&2; exit 2; }
awk -v value="${MIN_FRAME_RATIO}" 'BEGIN { exit !(value > 0 && value <= 1) }' || {
    echo "min-frame-ratio must be in (0, 1]" >&2
    exit 2
}

if [[ -z "${OUTPUT_DIR}" ]]; then
    OUTPUT_DIR="/dev/shm/ugripper_tactile_ffmpeg_soak_$(timestamp_slug)"
fi
if [[ -z "${FFMPEG_BIN}" ]]; then
    FFMPEG_BIN="$(command -v ffmpeg || true)"
fi
[[ -x "${FFMPEG_BIN}" ]] || { echo "ffmpeg not found: ${FFMPEG_BIN}" >&2; exit 1; }
[[ -e "${DEVICE}" ]] || { echo "device not found: ${DEVICE}" >&2; exit 1; }

mkdir -p "${OUTPUT_DIR}/cycles" "${OUTPUT_DIR}/failures"
REPORT_FILE="${OUTPUT_DIR}/cycle_report.tsv"
SUMMARY_FILE="${OUTPUT_DIR}/summary.txt"
TEST_START_TEXT="$(date '+%Y-%m-%d %H:%M:%S')"
TEST_START_EPOCH="$(date +%s)"
EXPECTED_FRAMES=$((120 * CYCLE_SEC))
MIN_FRAMES="$(awk -v expected="${EXPECTED_FRAMES}" -v ratio="${MIN_FRAME_RATIO}" 'BEGIN { printf "%d", expected * ratio }')"
SERVICE_WAS_ACTIVE=0
SERVICE_RESTORED=0

capture_snapshot() {
    local output_file="$1"
    local journal_since="$2"
    {
        echo "captured_at=$(date --iso-8601=ns)"
        echo "device=${DEVICE}"
        echo "resolved_device=$(readlink -f "${DEVICE}" 2>/dev/null || true)"
        echo
        echo "[service]"
        systemctl status "${SERVICE_NAME}" --no-pager || true
        echo
        echo "[hws]"
        if command -v hws >/dev/null 2>&1; then hws || true; else echo "hws unavailable"; fi
        echo
        echo "[tactile_links]"
        ls -l /dev/tcam_* 2>&1 || true
        echo
        echo "[device_users]"
        fuser -v "${DEVICE}" 2>&1 || true
        echo
        echo "[udevadm]"
        udevadm info --query=all --name="${DEVICE}" 2>&1 || true
        echo
        echo "[lsusb_tree]"
        lsusb -t 2>&1 || true
        echo
        echo "[kernel_since]"
        journalctl -k --since "${journal_since}" --no-pager 2>&1 || true
    } > "${output_file}" 2>&1
}

restore_service() {
    if [[ "${SERVICE_WAS_ACTIVE}" == "1" && "${SERVICE_RESTORED}" != "1" ]]; then
        echo "[INFO] restoring ${SERVICE_NAME}"
        systemctl start "${SERVICE_NAME}" || true
        if systemctl is-active --quiet "${SERVICE_NAME}"; then
            SERVICE_RESTORED=1
        fi
    fi
}

cleanup() {
    local exit_code=$?
    restore_service
    exit "${exit_code}"
}
trap cleanup EXIT INT TERM

capture_snapshot "${OUTPUT_DIR}/before.txt" "${TEST_START_TEXT}"
if systemctl is-active --quiet "${SERVICE_NAME}"; then
    SERVICE_WAS_ACTIVE=1
    print_section "Stop Service"
    systemctl stop "${SERVICE_NAME}"
    for _ in $(seq 1 30); do
        systemctl is-active --quiet "${SERVICE_NAME}" || break
        sleep 1
    done
    if systemctl is-active --quiet "${SERVICE_NAME}"; then
        echo "failed to stop ${SERVICE_NAME}" >&2
        exit 1
    fi
fi

sleep 1
if fuser "${DEVICE}" >/dev/null 2>&1; then
    capture_snapshot "${OUTPUT_DIR}/device_busy.txt" "${TEST_START_TEXT}"
    echo "device remains occupied after service stop: ${DEVICE}" >&2
    exit 1
fi

printf 'cycle\tstart_time\tend_time\telapsed_ms\texit_code\tframes\texpected_frames\tstatus\terror_category\tout_time_us\tframe_source\tlog\n' > "${REPORT_FILE}"

print_section "Tactile FFmpeg Stream-On Soak"
echo "device=${DEVICE}"
echo "duration_sec=${DURATION_SEC}"
echo "cycle_sec=${CYCLE_SEC}"
echo "expected_frames=${EXPECTED_FRAMES}"
echo "min_frames=${MIN_FRAMES}"
echo "output_dir=${OUTPUT_DIR}"

cycle=0
failure_count=0
zero_frame_count=0
low_frame_count=0
timeout_count=0
deadline=$((TEST_START_EPOCH + DURATION_SEC))

while [[ "$(date +%s)" -lt "${deadline}" ]]; do
    cycle=$((cycle + 1))
    cycle_id="$(printf '%06d' "${cycle}")"
    cycle_log="${OUTPUT_DIR}/cycles/${cycle_id}.log"
    cycle_start_text="$(date '+%Y-%m-%d %H:%M:%S')"
    cycle_start_ns="$(date +%s%N)"

    set +e
    timeout --signal=INT --kill-after=5 "$((CYCLE_SEC + 10))s" \
        "${FFMPEG_BIN}" \
        -hide_banner -loglevel info -nostats \
        -f v4l2 -input_format mjpeg -framerate 120 -video_size 640x480 \
        -i "${DEVICE}" -t "${CYCLE_SEC}" -c:v copy -f null - \
        -progress pipe:1 > "${cycle_log}" 2>&1
    exit_code=$?
    set -e

    cycle_end_ns="$(date +%s%N)"
    cycle_end_text="$(date '+%Y-%m-%d %H:%M:%S')"
    elapsed_ms=$(((cycle_end_ns - cycle_start_ns) / 1000000))
    frames="$(awk -F= '$1 == "frame" { value=$2 } END { gsub(/[[:space:]]/, "", value); print value + 0 }' "${cycle_log}")"
    out_time_us="$(awk -F= '$1 == "out_time_us" { value=$2 } END { gsub(/[[:space:]]/, "", value); print value + 0 }' "${cycle_log}")"
    frame_source="ffmpeg_progress"
    if [[ "${frames}" -eq 0 && "${out_time_us}" -gt 0 ]]; then
        frames=$(((out_time_us * 120 + 500000) / 1000000))
        frame_source="out_time_estimate"
    fi
    status="pass"
    error_category="none"

    if [[ "${exit_code}" == "124" || "${exit_code}" == "137" ]]; then
        status="fail"
        error_category="process_timeout"
        timeout_count=$((timeout_count + 1))
    elif grep -q 'VIDIOC_STREAMON' "${cycle_log}"; then
        status="fail"
        error_category="vidioc_streamon"
    elif grep -q 'Protocol error' "${cycle_log}"; then
        status="fail"
        error_category="protocol_error"
    elif grep -q 'No such device' "${cycle_log}"; then
        status="fail"
        error_category="no_such_device"
    elif grep -q 'Input/output error' "${cycle_log}"; then
        status="fail"
        error_category="io_error"
    elif [[ "${exit_code}" != "0" ]]; then
        status="fail"
        error_category="ffmpeg_exit"
    elif [[ "${frames}" -eq 0 ]]; then
        status="fail"
        error_category="zero_frames"
    elif [[ "${frames}" -lt "${MIN_FRAMES}" ]]; then
        status="low_frames"
        error_category="low_frames"
    fi

    if [[ "${frames}" -eq 0 ]]; then
        zero_frame_count=$((zero_frame_count + 1))
    fi
    if [[ "${status}" == "low_frames" ]]; then
        low_frame_count=$((low_frame_count + 1))
    fi
    if [[ "${status}" == "fail" ]]; then
        failure_count=$((failure_count + 1))
        failure_dir="${OUTPUT_DIR}/failures/${cycle_id}_${error_category}"
        mkdir -p "${failure_dir}"
        cp "${cycle_log}" "${failure_dir}/ffmpeg.log"
        capture_snapshot "${failure_dir}/snapshot.txt" "${cycle_start_text}"
    fi

    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
        "${cycle_id}" "${cycle_start_text}" "${cycle_end_text}" "${elapsed_ms}" \
        "${exit_code}" "${frames}" "${EXPECTED_FRAMES}" "${status}" \
        "${error_category}" "${out_time_us}" "${frame_source}" "${cycle_log}" >> "${REPORT_FILE}"
    printf '[%s] cycle=%s rc=%s frames=%s status=%s category=%s elapsed_ms=%s\n' \
        "$(date '+%H:%M:%S')" "${cycle_id}" "${exit_code}" "${frames}" \
        "${status}" "${error_category}" "${elapsed_ms}"
done

capture_snapshot "${OUTPUT_DIR}/after_before_service_restore.txt" "${TEST_START_TEXT}"

min_frames_seen="$(awk -F'\t' 'NR > 1 { if (!seen || $6 < min) min=$6; seen=1 } END { print seen ? min : 0 }' "${REPORT_FILE}")"
max_frames_seen="$(awk -F'\t' 'NR > 1 { if (!seen || $6 > max) max=$6; seen=1 } END { print seen ? max : 0 }' "${REPORT_FILE}")"
avg_frames_seen="$(awk -F'\t' 'NR > 1 { sum += $6; count++ } END { printf "%.2f", count ? sum / count : 0 }' "${REPORT_FILE}")"
actual_duration_sec=$(($(date +%s) - TEST_START_EPOCH))

{
    echo "device=${DEVICE}"
    echo "test_start=${TEST_START_TEXT}"
    echo "actual_duration_sec=${actual_duration_sec}"
    echo "cycle_sec=${CYCLE_SEC}"
    echo "cycles=${cycle}"
    echo "failures=${failure_count}"
    echo "zero_frame_cycles=${zero_frame_count}"
    echo "low_frame_cycles=${low_frame_count}"
    echo "process_timeouts=${timeout_count}"
    echo "expected_frames_per_cycle=${EXPECTED_FRAMES}"
    echo "minimum_accepted_frames=${MIN_FRAMES}"
    echo "min_frames_seen=${min_frames_seen}"
    echo "max_frames_seen=${max_frames_seen}"
    echo "average_frames=${avg_frames_seen}"
    echo "frame_count_note=uses ffmpeg frame progress when available, otherwise estimates frames from final out_time_us at 120 fps"
    echo "error_categories:"
    awk -F'\t' 'NR > 1 && $9 != "none" { count[$9]++ } END { for (key in count) print "  " key "=" count[key] }' "${REPORT_FILE}" | sort
} > "${SUMMARY_FILE}"

restore_service
sleep 3
capture_snapshot "${OUTPUT_DIR}/after_service_restore.txt" "${TEST_START_TEXT}"
{
    echo "service_active=$(systemctl is-active "${SERVICE_NAME}" 2>/dev/null || true)"
    if command -v hws >/dev/null 2>&1; then hws || true; fi
} > "${OUTPUT_DIR}/restore_check.txt" 2>&1

cat "${SUMMARY_FILE}"
echo "report=${REPORT_FILE}"
echo "summary=${SUMMARY_FILE}"

if [[ "${failure_count}" -gt 0 ]]; then
    exit 1
fi
