#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
CAMERA_TEST_SCRIPT="${CAMERA_TEST_SCRIPT:-$SCRIPT_DIR/camera_test.sh}"
RUN_NAME="${RUN_NAME:-camera_crash_capture_$(date +%Y%m%d_%H%M%S)}"
BASE_DIR="${BASE_DIR:-$PWD/$RUN_NAME}"
CAPTURE_DIR="$BASE_DIR/capture"
STATE_DIR="$BASE_DIR/state"
TEST_STDOUT="$BASE_DIR/camera_test.stdout.log"
TEST_STDERR="$BASE_DIR/camera_test.stderr.log"
DURATION="${DURATION:-10}"
CAMERA_SET="${CAMERA_SET:-non_main_merge_tactile}"
START_INTERVAL="${START_INTERVAL:-1}"
CAPTURE_KERNEL_LOG="${CAPTURE_KERNEL_LOG:-1}"

mkdir -p "$CAPTURE_DIR" "$STATE_DIR"
monitors=()

if [ ! -f "$CAMERA_TEST_SCRIPT" ]; then
  echo "camera_test script not found: $CAMERA_TEST_SCRIPT" >&2
  exit 1
fi

log_note() {
  local msg="$1"
  echo "[$(date '+%F %T')] $msg" | tee -a "$STATE_DIR/run_info.txt"
}

flush_loop() {
  while true; do
    sync
    sleep 1
  done
}

stop_monitors() {
  for pid in "${monitors[@]:-}"; do
    if kill -0 "$pid" 2>/dev/null; then
      kill "$pid" 2>/dev/null || true
    fi
  done
  wait || true
}

trap 'stop_monitors' EXIT INT TERM

log_note "run_dir=$BASE_DIR"
log_note "camera_set=$CAMERA_SET"
log_note "duration=$DURATION"
log_note "start_interval=$START_INTERVAL"
log_note "capture_kernel_log=$CAPTURE_KERNEL_LOG"
log_note "camera_test_script=$CAMERA_TEST_SCRIPT"

{
  echo '--- date ---'; date '+%F %T %Z'
  echo '--- uname ---'; uname -a
  echo '--- cmdline ---'; cat /proc/cmdline
  echo '--- meminfo ---'; cat /proc/meminfo
  echo '--- lsusb -t ---'; lsusb -t || true
  echo '--- devices ---'
  for n in left_stereo right_stereo left_tcam_l left_tcam_r right_tcam_l right_tcam_r; do
    ls -l "/dev/$n" || true
  done
} > "$STATE_DIR/precheck.txt"

vmstat 1 > "$CAPTURE_DIR/vmstat.log" 2>&1 &
monitors+=("$!")
top -b -d 1 > "$CAPTURE_DIR/top.log" 2>&1 &
monitors+=("$!")
while true; do
  {
    echo "=== $(date '+%F %T') ==="
    rg 'MemFree|MemAvailable|CmaTotal|CmaAllocated|CmaReleased|CmaFree|SwapFree' /proc/meminfo || true
  } >> "$CAPTURE_DIR/mem_watch.log"
  sleep 1
done &
monitors+=("$!")
while true; do
  {
    echo "=== $(date '+%F %T') ==="
    cat /proc/interrupts | rg 'xhci|usb|fiq|dwc|irq/' || true
  } >> "$CAPTURE_DIR/interrupts.log"
  sleep 1
done &
monitors+=("$!")
while true; do
  {
    echo "=== $(date '+%F %T') ==="
    ps -eo pid,ppid,stat,pcpu,pmem,rss,cmd | rg 'ffmpeg|camera_test|gst-launch|dmesg|journalctl' || true
  } >> "$CAPTURE_DIR/procs.log"
  sleep 1
done &
monitors+=("$!")
flush_loop &
monitors+=("$!")

if [ "$CAPTURE_KERNEL_LOG" = '1' ]; then
  stdbuf -oL sudo -n dmesg -wT > "$CAPTURE_DIR/dmesg_live.log" 2>&1 &
  monitors+=("$!")
  stdbuf -oL sudo -n journalctl -kf -o short-monotonic > "$CAPTURE_DIR/journal_k.log" 2>&1 &
  monitors+=("$!")
fi

log_note 'starting camera_test.sh'
RUN_NAME="${RUN_NAME}_camera" \
DURATION="$DURATION" \
CAMERA_SET="$CAMERA_SET" \
START_INTERVAL="$START_INTERVAL" \
CAPTURE_KERNEL_LOG=0 \
bash "$CAMERA_TEST_SCRIPT" > "$TEST_STDOUT" 2> "$TEST_STDERR"
status=$?
log_note "camera_test_exit=$status"

{
  echo '--- date ---'; date '+%F %T %Z'
  echo '--- uptime ---'; uptime -s || true
  echo '--- meminfo ---'; cat /proc/meminfo
} > "$STATE_DIR/postcheck.txt"

exit "$status"
