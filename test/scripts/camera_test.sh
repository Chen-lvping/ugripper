#!/bin/bash
set -euo pipefail

RUN_NAME="${RUN_NAME:-camera_test_$(date +%Y%m%d_%H%M%S)}"
BASE_DIR="${BASE_DIR:-$PWD/$RUN_NAME}"
LOG_DIR="$BASE_DIR/logs"
CAM_DIR="$BASE_DIR/cams"
STAT_DIR="$BASE_DIR/stats"
DURATION="${DURATION:-10}"
CAMERA_SET="${CAMERA_SET:-non_main}"
START_INTERVAL="${START_INTERVAL:-0}"
CAPTURE_KERNEL_LOG="${CAPTURE_KERNEL_LOG:-0}"

mkdir -p "$LOG_DIR" "$CAM_DIR" "$STAT_DIR"

pids=()
monitor_pids=()

log_note() {
    local msg="$1"
    echo "[$(date '+%F %T')] $msg" | tee -a "$STAT_DIR/run_info.txt"
    sync || true
}

cleanup() {
    local code=$?
    for pid in "${pids[@]:-}"; do
        if kill -0 "$pid" 2>/dev/null; then
            kill -INT "$pid" 2>/dev/null || true
        fi
    done
    sleep 1
    for pid in "${pids[@]:-}"; do
        if kill -0 "$pid" 2>/dev/null; then
            kill -TERM "$pid" 2>/dev/null || true
        fi
    done
    for pid in "${monitor_pids[@]:-}"; do
        if kill -0 "$pid" 2>/dev/null; then
            kill "$pid" 2>/dev/null || true
        fi
    done
    wait || true
    exit "$code"
}
trap cleanup EXIT INT TERM

timestamp() {
    date +"%Y%m%d_%H%M%S"
}

run_cam() {
    local name="$1"
    local dev="$2"
    local format="$3"
    local size="$4"
    local in_fps="$5"
    local out_fps="$6"
    local qp="$7"
    local queue_size="$8"

    local out_dir="$CAM_DIR/$name"
    local out_file="$out_dir/${name}_$(timestamp).mkv"
    local log_file="$LOG_DIR/${name}_$(timestamp).log"

    mkdir -p "$out_dir"
    log_note "start $name dev=$dev out=$out_file"

    ffmpeg -hide_banner -loglevel info -y \
        -thread_queue_size "$queue_size" \
        -f v4l2 -input_format "$format" \
        -framerate "$in_fps" \
        -video_size "$size" \
        -copyts -use_wallclock_as_timestamps 1 \
        -i "$dev" \
        -vf "fps=$out_fps,format=nv12" \
        -an \
        -c:v hevc_rkmpp \
        -rc_mode CQP \
        -qp_init "$qp" -qp_max 40 -qp_min 20 \
        -qp_max_i 40 -qp_min_i 20 \
        -fps_mode passthrough \
        -muxpreload 0 -muxdelay 0 \
        "$out_file" \
        >"$log_file" 2>&1 &

    local pid=$!
    pids+=("$pid")
    echo "$pid $name $dev $out_file" >> "$STAT_DIR/pids.txt"
    log_note "launched pid=$pid name=$name"
}

run_cam_pair_merge() {
    local name="$1"
    local dev_a="$2"
    local dev_b="$3"
    local size="$4"
    local in_fps="$5"
    local out_fps="$6"
    local qp="$7"
    local queue_size="$8"

    local width="${size%x*}"
    local height="${size#*x}"
    local out_dir="$CAM_DIR/$name"
    local out_file="$out_dir/${name}_$(timestamp).mkv"
    local log_file="$LOG_DIR/${name}_$(timestamp).log"

    mkdir -p "$out_dir"
    log_note "start $name dev_a=$dev_a dev_b=$dev_b out=$out_file"

    ffmpeg -hide_banner -loglevel info -y \
        -thread_queue_size "$queue_size" \
        -f v4l2 -input_format mjpeg \
        -framerate "$in_fps" \
        -video_size "$size" \
        -copyts -use_wallclock_as_timestamps 1 \
        -i "$dev_a" \
        -thread_queue_size "$queue_size" \
        -f v4l2 -input_format mjpeg \
        -framerate "$in_fps" \
        -video_size "$size" \
        -copyts -use_wallclock_as_timestamps 1 \
        -i "$dev_b" \
        -filter_complex "[0:v]fps=$out_fps,scale=${width}:${height},format=nv12[left];[1:v]fps=$out_fps,scale=${width}:${height},format=nv12[right];[left][right]hstack=inputs=2,format=nv12[v]" \
        -map '[v]' \
        -an \
        -c:v hevc_rkmpp \
        -rc_mode CQP \
        -qp_init "$qp" -qp_max 40 -qp_min 20 \
        -qp_max_i 40 -qp_min_i 20 \
        -fps_mode passthrough \
        -muxpreload 0 -muxdelay 0 \
        "$out_file" \
        >"$log_file" 2>&1 &

    local pid=$!
    pids+=("$pid")
    echo "$pid $name $dev_a+$dev_b $out_file" >> "$STAT_DIR/pids.txt"
    log_note "launched pid=$pid name=$name"
}

start_monitors() {
    free -m > "$STAT_DIR/free_before.txt"
    cat /proc/meminfo > "$STAT_DIR/meminfo_before.txt"
    vmstat 1 > "$STAT_DIR/vmstat.log" 2>&1 &
    monitor_pids+=("$!")
    top -b -d 1 > "$STAT_DIR/top.log" 2>&1 &
    monitor_pids+=("$!")
    ps -eo pid,ppid,cmd > "$STAT_DIR/ps_before.txt"

    if [ "$CAPTURE_KERNEL_LOG" = "1" ]; then
        if sudo -n true 2>/dev/null; then
            stdbuf -oL sudo -n dmesg -wT > "$STAT_DIR/dmesg_live.log" 2>&1 &
            monitor_pids+=("$!")
            log_note "kernel log capture started"
        else
            log_note "kernel log capture skipped: sudo credential unavailable"
        fi
    fi
}

stop_monitors() {
    for pid in "${monitor_pids[@]:-}"; do
        if kill -0 "$pid" 2>/dev/null; then
            kill "$pid" 2>/dev/null || true
        fi
    done
    free -m > "$STAT_DIR/free_after.txt"
    cat /proc/meminfo > "$STAT_DIR/meminfo_after.txt"
    ps -eo pid,ppid,cmd > "$STAT_DIR/ps_after.txt"
}

run_sequence() {
    local -n specs_ref=$1
    local index=0
    local total=${#specs_ref[@]}
    local spec kind name dev_a dev_b format size in_fps out_fps qp queue_size
    for spec in "${specs_ref[@]}"; do
        IFS='|' read -r kind name dev_a dev_b format size in_fps out_fps qp queue_size <<< "$spec"
        index=$((index + 1))
        log_note "launch_seq=$index/$total name=$name kind=$kind"
        if [ "$kind" = 'pair' ]; then
            run_cam_pair_merge "$name" "$dev_a" "$dev_b" "$size" "$in_fps" "$out_fps" "$qp" "$queue_size"
        else
            run_cam "$name" "$dev_a" "$format" "$size" "$in_fps" "$out_fps" "$qp" "$queue_size"
        fi
        if [ "$START_INTERVAL" != "0" ] && [ "$index" -lt "$total" ]; then
            sleep "$START_INTERVAL"
        fi
    done
}

run_non_main() {
    local specs=(
        'single|left_stereo|/dev/left_stereo||mjpeg|1280x800|120|60|30|128'
        'single|right_stereo|/dev/right_stereo||mjpeg|1280x800|120|60|30|128'
        'single|left_tcam_l|/dev/left_tcam_l||mjpeg|640x480|120|60|30|64'
        'single|left_tcam_r|/dev/left_tcam_r||mjpeg|640x480|120|60|30|64'
        'single|right_tcam_l|/dev/right_tcam_l||mjpeg|640x480|120|60|30|64'
        'single|right_tcam_r|/dev/right_tcam_r||mjpeg|640x480|120|60|30|64'
    )
    run_sequence specs
}

run_non_main_merge_tactile() {
    local specs=(
        'single|left_stereo|/dev/left_stereo||mjpeg|1280x800|120|60|30|128'
        'single|right_stereo|/dev/right_stereo||mjpeg|1280x800|120|60|30|128'
        'pair|left_tact_pair|/dev/left_tcam_l|/dev/left_tcam_r|mjpeg|640x480|120|60|30|64'
        'pair|right_tact_pair|/dev/right_tcam_l|/dev/right_tcam_r|mjpeg|640x480|120|60|30|64'
    )
    run_sequence specs
}

: > "$STAT_DIR/run_info.txt"
log_note "run_dir=$BASE_DIR"
log_note "duration=$DURATION"
log_note "camera_set=$CAMERA_SET"
log_note "start_interval=$START_INTERVAL"
log_note "capture_kernel_log=$CAPTURE_KERNEL_LOG"
log_note "starting monitors"
start_monitors

case "$CAMERA_SET" in
    non_main)
        run_non_main
        ;;
    non_main_merge_tactile)
        run_non_main_merge_tactile
        ;;
    *)
        echo "unsupported CAMERA_SET=$CAMERA_SET" >&2
        exit 1
        ;;
esac

log_note "all requested cameras started"
sleep "$DURATION"

log_note "stopping encoders"
for pid in "${pids[@]}"; do
    if kill -0 "$pid" 2>/dev/null; then
        kill -INT "$pid" 2>/dev/null || true
    fi
done
wait || true
stop_monitors
log_note "completed"
trap - EXIT INT TERM
exit 0
