#!/usr/bin/env bash

set -u

episode_dir="${1:-}"

section() {
  printf '\n== %s ==\n' "$1"
}

show_file() {
  local path="$1"
  if [ -e "$path" ]; then
    printf '%s\n' "[exists] $path"
    if [ -f "$path" ]; then
      sed -n '1,80p' "$path" 2>/dev/null || true
    fi
  else
    printf '%s\n' "[missing] $path"
  fi
}

section "Environment"
printf 'pwd=%s\n' "$(pwd)"
printf 'time=%s\n' "$(date -Is 2>/dev/null || date)"
uname -a 2>/dev/null || true

section "Service"
systemctl is-active ugripper.service 2>/dev/null || true
systemctl is-enabled ugripper.service 2>/dev/null || true
systemctl status ugripper.service --no-pager -l 2>/dev/null | sed -n '1,80p' || true

section "Processes"
ps -ef | egrep 'run_record.sh|triple_camera_record.py|sensor_recorder|camera_recorder' | grep -v grep || true

section "Key files"
show_file /etc/environment
show_file /tmp/umi_recording.lock
show_file /dev/shm/umi_ptp_status


section "Recent service logs"
journalctl -u ugripper.service -n 120 --no-pager 2>/dev/null || true

section "Recent kernel USB logs"
journalctl -k -n 200 --no-pager 2>/dev/null | egrep 'usb|uvcvideo|xhci|reset|disconnect|error -71' || true

section "Episode"
if [ -n "$episode_dir" ]; then
  if [ -d "$episode_dir" ]; then
    printf 'episode_dir=%s\n' "$episode_dir"
    find "$episode_dir" -maxdepth 1 -type f | sort 2>/dev/null || true
    if [ -f "$episode_dir/validation_error.log" ]; then
      printf '\n-- validation_error.log --\n'
      sed -n '1,120p' "$episode_dir/validation_error.log" 2>/dev/null || true
    fi
  else
    printf 'episode_dir_missing=%s\n' "$episode_dir"
  fi
fi
