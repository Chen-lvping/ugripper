#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "${SCRIPT_DIR}/board_test_common.sh"

usage() {
    cat <<'EOF'
Usage: board_tactile_ffmpeg_parallel_soak.sh [options]

Repeatedly opens all tactile cameras at the same time, captures briefly, then
closes all streams together to stress concurrent UVC stream-on/stream-off.

Options:
  --devices CSV          Devices to open concurrently
                         (default: all four /dev/tcam_* nodes)
  --duration-sec N       Total soak duration (default: 1800)
  --cycle-sec SEC        Capture duration per cycle (default: 1)
  --min-frame-ratio R    Low-frame threshold versus 120 fps (default: 0.70)
  --output-dir DIR       Result directory under /dev/shm by default
  --service-name NAME    Service stopped during direct access (default: ugripper.service)
  --ffmpeg-bin PATH      Override ffmpeg binary
  --help                 Show this help
EOF
}

DEVICES_CSV="/dev/tcam_left_l,/dev/tcam_left_r,/dev/tcam_right_l,/dev/tcam_right_r"
DURATION_SEC=1800
CYCLE_SEC=1
MIN_FRAME_RATIO=0.70
OUTPUT_DIR=""
SERVICE_NAME="ugripper.service"
FFMPEG_BIN=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --devices) DEVICES_CSV="$2"; shift 2 ;;
        --duration-sec) DURATION_SEC="$2"; shift 2 ;;
        --cycle-sec) CYCLE_SEC="$2"; shift 2 ;;
        --min-frame-ratio) MIN_FRAME_RATIO="$2"; shift 2 ;;
        --output-dir) OUTPUT_DIR="$2"; shift 2 ;;
        --service-name) SERVICE_NAME="$2"; shift 2 ;;
        --ffmpeg-bin) FFMPEG_BIN="$2"; shift 2 ;;
        --help|-h) usage; exit 0 ;;
        *) echo "unknown argument: $1" >&2; usage; exit 2 ;;
    esac
done

[[ "${DURATION_SEC}" =~ ^[1-9][0-9]*$ ]] || { echo "duration-sec must be a positive integer" >&2; exit 2; }
awk -v value="${CYCLE_SEC}" 'BEGIN { exit !(value > 0) }' || { echo "cycle-sec must be positive" >&2; exit 2; }
awk -v value="${MIN_FRAME_RATIO}" 'BEGIN { exit !(value > 0 && value <= 1) }' || {
    echo "min-frame-ratio must be in (0, 1]" >&2
    exit 2
}

IFS=',' read -r -a DEVICES <<< "${DEVICES_CSV}"
[[ "${#DEVICES[@]}" -gt 0 ]] || { echo "no devices specified" >&2; exit 2; }
for device in "${DEVICES[@]}"; do
    [[ -e "${device}" ]] || { echo "device not found: ${device}" >&2; exit 1; }
done

if [[ -z "${OUTPUT_DIR}" ]]; then
    OUTPUT_DIR="/dev/shm/ugripper_tactile_parallel_soak_$(timestamp_slug)"
fi
if [[ -z "${FFMPEG_BIN}" ]]; then
    FFMPEG_BIN="$(command -v ffmpeg || true)"
fi
[[ -x "${FFMPEG_BIN}" ]] || { echo "ffmpeg not found: ${FFMPEG_BIN}" >&2; exit 1; }

mkdir -p "${OUTPUT_DIR}/cycles" "${OUTPUT_DIR}/failures"
REPORT_FILE="${OUTPUT_DIR}/cycle_report.tsv"
SUMMARY_FILE="${OUTPUT_DIR}/summary.txt"
TEST_START_TEXT="$(date '+%Y-%m-%d %H:%M:%S')"
TEST_START_EPOCH="$(date +%s)"
EXPECTED_FRAMES="$(awk -v seconds="${CYCLE_SEC}" 'BEGIN { printf "%d", 120 * seconds + 0.5 }')"
MIN_FRAMES="$(awk -v expected="${EXPECTED_FRAMES}" -v ratio="${MIN_FRAME_RATIO}" 'BEGIN { printf "%d", expected * ratio }')"
COMMAND_TIMEOUT="$(awk -v seconds="${CYCLE_SEC}" 'BEGIN { printf "%.3f", seconds + 10 }')s"
SERVICE_WAS_ACTIVE=0
SERVICE_RESTORED=0

capture_snapshot() {
    local output_file="$1"
    local journal_since="$2"
    {
        echo "captured_at=$(date --iso-8601=ns)"
        echo "devices=${DEVICES_CSV}"
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
        for device in "${DEVICES[@]}"; do fuser -v "${device}" 2>&1 || true; done
        echo
        echo "[udevadm]"
        for device in "${DEVICES[@]}"; do
            echo "--- ${device} ---"
            udevadm info --query=all --name="${device}" 2>&1 || true
        done
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
        if systemctl is-active --quiet "${SERVICE_NAME}"; then SERVICE_RESTORED=1; fi
    fi
}

cleanup() {
    local exit_code=$?
    jobs -pr | xargs -r kill >/dev/null 2>&1 || true
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
    systemctl is-active --quiet "${SERVICE_NAME}" && { echo "failed to stop ${SERVICE_NAME}" >&2; exit 1; }
fi

sleep 1
for device in "${DEVICES[@]}"; do
    if fuser "${device}" >/dev/null 2>&1; then
        capture_snapshot "${OUTPUT_DIR}/device_busy.txt" "${TEST_START_TEXT}"
        echo "device remains occupied after service stop: ${device}" >&2
        exit 1
    fi
done

printf 'cycle\tdevice\tstart_time\tend_time\tcycle_elapsed_ms\texit_code\tframes\texpected_frames\tstatus\terror_category\tout_time_us\tframe_source\tlog\n' > "${REPORT_FILE}"

print_section "Parallel Tactile FFmpeg Stream-On Soak"
echo "devices=${DEVICES_CSV}"
echo "duration_sec=${DURATION_SEC}"
echo "cycle_sec=${CYCLE_SEC}"
echo "expected_frames=${EXPECTED_FRAMES}"
echo "min_frames=${MIN_FRAMES}"
echo "output_dir=${OUTPUT_DIR}"

cycle=0
failed_rows=0
failed_cycles=0
deadline=$((TEST_START_EPOCH + DURATION_SEC))

while [[ "$(date +%s)" -lt "${deadline}" ]]; do
    cycle=$((cycle + 1))
    cycle_id="$(printf '%06d' "${cycle}")"
    cycle_dir="${OUTPUT_DIR}/cycles/${cycle_id}"
    mkdir -p "${cycle_dir}"
    cycle_start_text="$(date '+%Y-%m-%d %H:%M:%S')"
    cycle_start_ns="$(date +%s%N)"
    declare -a PIDS=()

    for device in "${DEVICES[@]}"; do
        device_name="${device##*/}"
        cycle_log="${cycle_dir}/${device_name}.log"
        (
            set +e
            timeout --signal=INT --kill-after=5 "${COMMAND_TIMEOUT}" \
                "${FFMPEG_BIN}" \
                -hide_banner -loglevel info -nostats \
                -f v4l2 -input_format mjpeg -framerate 120 -video_size 640x480 \
                -i "${device}" -t "${CYCLE_SEC}" -c:v copy -f null - \
                -progress pipe:1 > "${cycle_log}" 2>&1
            echo "$?" > "${cycle_dir}/${device_name}.rc"
        ) &
        PIDS+=("$!")
    done

    for pid in "${PIDS[@]}"; do wait "${pid}" || true; done
    cycle_end_ns="$(date +%s%N)"
    cycle_end_text="$(date '+%Y-%m-%d %H:%M:%S')"
    elapsed_ms=$(((cycle_end_ns - cycle_start_ns) / 1000000))
    cycle_failed=0
    cycle_line=""

    for device in "${DEVICES[@]}"; do
        device_name="${device##*/}"
        cycle_log="${cycle_dir}/${device_name}.log"
        exit_code="$(cat "${cycle_dir}/${device_name}.rc" 2>/dev/null || echo 255)"
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
            status="fail"; error_category="process_timeout"
        elif grep -q 'VIDIOC_STREAMON' "${cycle_log}"; then
            status="fail"; error_category="vidioc_streamon"
        elif grep -q 'Protocol error' "${cycle_log}"; then
            status="fail"; error_category="protocol_error"
        elif grep -q 'No such device' "${cycle_log}"; then
            status="fail"; error_category="no_such_device"
        elif grep -q 'Input/output error' "${cycle_log}"; then
            status="fail"; error_category="io_error"
        elif [[ "${exit_code}" != "0" ]]; then
            status="fail"; error_category="ffmpeg_exit"
        elif [[ "${frames}" -eq 0 ]]; then
            status="fail"; error_category="zero_frames"
        elif [[ "${frames}" -lt "${MIN_FRAMES}" ]]; then
            status="low_frames"; error_category="low_frames"
        fi

        if [[ "${status}" != "pass" ]]; then
            failed_rows=$((failed_rows + 1))
            cycle_failed=1
        fi
        printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
            "${cycle_id}" "${device}" "${cycle_start_text}" "${cycle_end_text}" "${elapsed_ms}" \
            "${exit_code}" "${frames}" "${EXPECTED_FRAMES}" "${status}" "${error_category}" \
            "${out_time_us}" "${frame_source}" "${cycle_log}" >> "${REPORT_FILE}"
        cycle_line+=" ${device_name}=${frames}/${status}"
    done

    if [[ "${cycle_failed}" == "1" ]]; then
        failed_cycles=$((failed_cycles + 1))
        failure_dir="${OUTPUT_DIR}/failures/${cycle_id}"
        mkdir -p "${failure_dir}"
        cp -a "${cycle_dir}/." "${failure_dir}/"
        capture_snapshot "${failure_dir}/snapshot.txt" "${cycle_start_text}"
    fi
    printf '[%s] cycle=%s elapsed_ms=%s%s\n' "$(date '+%H:%M:%S')" "${cycle_id}" "${elapsed_ms}" "${cycle_line}"
done

capture_snapshot "${OUTPUT_DIR}/after_before_service_restore.txt" "${TEST_START_TEXT}"
actual_duration_sec=$(($(date +%s) - TEST_START_EPOCH))
{
    echo "devices=${DEVICES_CSV}"
    echo "test_start=${TEST_START_TEXT}"
    echo "actual_duration_sec=${actual_duration_sec}"
    echo "cycle_sec=${CYCLE_SEC}"
    echo "cycles=${cycle}"
    echo "total_stream_opens=$((cycle * ${#DEVICES[@]}))"
    echo "failed_cycles=${failed_cycles}"
    echo "failed_or_low_rows=${failed_rows}"
    echo "expected_frames_per_device_cycle=${EXPECTED_FRAMES}"
    echo "minimum_accepted_frames=${MIN_FRAMES}"
    echo "per_device:"
    awk -F'\t' 'NR > 1 { count[$2]++; sum[$2]+=$7; if (!seen[$2] || $7<min[$2]) min[$2]=$7; if (!seen[$2] || $7>max[$2]) max[$2]=$7; seen[$2]=1; if ($9!="pass") bad[$2]++ } END { for (key in count) printf "  %s cycles=%d failures_or_low=%d min_frames=%d max_frames=%d average_frames=%.2f\n", key, count[key], bad[key]+0, min[key], max[key], sum[key]/count[key] }' "${REPORT_FILE}" | sort
    echo "error_categories:"
    awk -F'\t' 'NR > 1 && $10 != "none" { count[$2 FS $10]++ } END { for (key in count) print "  " key "=" count[key] }' "${REPORT_FILE}" | sort
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
[[ "${failed_rows}" -eq 0 ]]
