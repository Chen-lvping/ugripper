#!/bin/bash

LOCK_FILE="${LOCK_FILE:-/tmp/umi_recording.lock}"
CHECK_TARGETS=(8.8.8.8 1.1.1.1 223.5.5.5)
NTP_SERVER_LIST="${NTP_SERVERS:-ntp.ubuntu.com 0.ubuntu.pool.ntp.org 1.ubuntu.pool.ntp.org 223.5.5.5}"
read -r -a NTP_SERVERS_ARRAY <<< "$NTP_SERVER_LIST"
CONTINUOUS_NTP_SERVICES=(ntp.service chrony.service systemd-timesyncd.service)
SNTP_LOG="/tmp/umi_sntp_sync_$$.log"
NTPD_LOG="/tmp/umi_ntpd_sync_$$.log"
MAX_NETWORK_WAIT="${MAX_NETWORK_WAIT:-45}"
MAX_RECORDING_WAIT="${MAX_RECORDING_WAIT:-0}"
MAX_SYNC_WAIT="${MAX_SYNC_WAIT:-60}"
POLL_INTERVAL_SEC="${POLL_INTERVAL_SEC:-2}"
NTP_ENABLED_BY_SCRIPT=0

log() {
    echo "[NTP-Service] $1"
}

disable_ntp_if_needed() {
    if [ "$NTP_ENABLED_BY_SCRIPT" -eq 1 ]; then
        if command -v timedatectl >/dev/null 2>&1; then
            timedatectl set-ntp false >/dev/null 2>&1 || true
        fi
        stop_continuous_ntp_services
        NTP_ENABLED_BY_SCRIPT=0
    fi
}

cleanup() {
    disable_ntp_if_needed
    rm -f "$SNTP_LOG" "$NTPD_LOG" >/dev/null 2>&1 || true
}
trap cleanup EXIT
trap 'cleanup; exit 0' INT TERM

recording_lock_active() {
    if [ ! -f "$LOCK_FILE" ]; then
        return 1
    fi

    local pid=""
    if command -v python3 >/dev/null 2>&1; then
        pid="$(python3 - "$LOCK_FILE" <<'PY' 2>/dev/null || true
import json
import sys

try:
    with open(sys.argv[1], "r", encoding="utf-8") as f:
        data = json.load(f)
    pid = data.get("pid")
    print(pid if isinstance(pid, int) and pid > 0 else "")
except Exception:
    print("")
PY
)"
    fi

    if [ -n "$pid" ]; then
        if kill -0 "$pid" >/dev/null 2>&1; then
            return 0
        fi
        log "Stale recording lock found for pid=$pid. Removing it."
        rm -f "$LOCK_FILE" || true
        return 1
    fi

    # Older lock format or partially-written file: treat as active to avoid
    # changing the system clock while recording state is ambiguous.
    return 0
}

stop_continuous_ntp_services() {
    if ! command -v systemctl >/dev/null 2>&1; then
        return 0
    fi

    local service=""
    for service in "${CONTINUOUS_NTP_SERVICES[@]}"; do
        if systemctl list-unit-files "$service" --no-legend >/dev/null 2>&1; then
            systemctl stop "$service" >/dev/null 2>&1 || true
        fi
    done
}

wait_for_idle() {
    local start_time
    local current_time
    local elapsed

    start_time=$(date +%s)
    while recording_lock_active; do
        if [ "$MAX_RECORDING_WAIT" -le 0 ]; then
            log "Recording lock is active. Skipping NTP sync."
            return 1
        fi

        current_time=$(date +%s)
        elapsed=$((current_time - start_time))
        if [ "$elapsed" -ge "$MAX_RECORDING_WAIT" ]; then
            log "Recording lock still active after ${MAX_RECORDING_WAIT}s. Skipping NTP sync."
            return 1
        fi

        log "System is RECORDING (lock found). Waiting for idle..."
        sleep "$POLL_INTERVAL_SEC"
    done
    return 0
}

check_network() {
    if ! command -v ping >/dev/null 2>&1; then
        log "ping not found. Skipping explicit network probe."
        return 0
    fi

    for target in "${CHECK_TARGETS[@]}"; do
        if ping -c 1 -W 2 "$target" >/dev/null 2>&1; then
            return 0
        fi
    done
    return 1
}

wait_for_network() {
    local start_time
    local current_time
    local elapsed

    start_time=$(date +%s)
    while ! check_network; do
        current_time=$(date +%s)
        elapsed=$((current_time - start_time))
        if [ "$elapsed" -ge "$MAX_NETWORK_WAIT" ]; then
            log "Network probe timeout after ${MAX_NETWORK_WAIT}s. Skipping NTP sync."
            return 1
        fi
        sleep "$POLL_INTERVAL_SEC"
    done
    log "Network is UP."
    return 0
}

sync_time_with_sntp() {
    if ! command -v sntp >/dev/null 2>&1; then
        return 1
    fi

    local server=""
    for server in "${NTP_SERVERS_ARRAY[@]}"; do
        if recording_lock_active; then
            log "INTERRUPT: Recording started before SNTP sync. Aborting."
            return 2
        fi
        log "Trying one-shot SNTP sync via ${server}..."
        if timeout "$MAX_SYNC_WAIT" sntp -t 5 -S "$server" >"$SNTP_LOG" 2>&1; then
            log "SNTP sync SUCCESS via ${server}."
            log "Current time: $(date)"
            return 0
        fi
        log "SNTP sync failed via ${server}: $(tail -n 1 "$SNTP_LOG" 2>/dev/null || true)"
    done
    return 1
}

sync_time_with_ntpd() {
    if ! command -v ntpd >/dev/null 2>&1; then
        return 1
    fi

    log "Trying one-shot ntpd sync via /etc/ntp.conf..."
    if timeout "$MAX_SYNC_WAIT" ntpd -q -g -n >"$NTPD_LOG" 2>&1; then
        log "ntpd one-shot sync SUCCESS."
        log "Current time: $(date)"
        return 0
    fi
    log "ntpd one-shot sync failed: $(tail -n 1 "$NTPD_LOG" 2>/dev/null || true)"
    return 1
}

sync_time_with_timedatectl() {
    if ! command -v timedatectl >/dev/null 2>&1; then
        return 1
    fi

    timedatectl set-ntp false >/dev/null 2>&1 || true
    if ! timedatectl set-ntp true >/dev/null 2>&1; then
        log "timedatectl set-ntp true failed."
        return 1
    fi
    NTP_ENABLED_BY_SCRIPT=1

    log "Waiting for timedatectl sync status..."

    local start_time
    local current_time
    local elapsed
    local status
    start_time=$(date +%s)

    while true; do
        status=$(timedatectl show -p NTPSynchronized --value 2>/dev/null)
        if [ "$status" = "yes" ]; then
            log "timedatectl sync SUCCESS."
            log "Current time: $(date)"
            return 0
        fi

        if recording_lock_active; then
            log "INTERRUPT: Recording started during timedatectl sync wait. Aborting."
            return 2
        fi

        current_time=$(date +%s)
        elapsed=$((current_time - start_time))
        if [ "$elapsed" -ge "$MAX_SYNC_WAIT" ]; then
            log "Timeout waiting for timedatectl sync."
            return 1
        fi

        sleep 1
    done
}

log "Service started (Backend: one-shot sntp, fallback timedatectl)."

stop_continuous_ntp_services

if ! wait_for_idle; then
    exit 0
fi

if ! wait_for_network; then
    exit 0
fi

if ! wait_for_idle; then
    exit 0
fi

log "System IDLE. Running one-shot time sync..."

if sync_time_with_sntp; then
    log "Time sync SUCCESS."
elif sync_time_with_ntpd; then
    log "Time sync SUCCESS."
elif sync_time_with_timedatectl; then
    log "Time sync SUCCESS."
else
    log "Time sync FAILED (no usable one-shot backend or all servers failed)."
fi

log "Stopping continuous NTP services to protect future recordings."
disable_ntp_if_needed
stop_continuous_ntp_services

exit 0
