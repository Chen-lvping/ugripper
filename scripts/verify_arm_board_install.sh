#!/usr/bin/env bash
set -euo pipefail

DEB_PATH="${1:-/tmp/ugripper_1.2.8_arm64.deb}"
LOG_PATH="${LOG_PATH:-/tmp/ugripper_board_verify_$(date +%Y%m%d_%H%M%S).log}"

run() {
    echo
    echo "\$ $*"
    "$@" 2>&1 | tee -a "${LOG_PATH}"
}

run_allow_fail() {
    echo
    echo "\$ $*"
    set +e
    "$@" 2>&1 | tee -a "${LOG_PATH}"
    local status=$?
    set -e
    echo "[exit_code] ${status}" | tee -a "${LOG_PATH}"
    return 0
}

require_sudo() {
    if [ "$(id -u)" -eq 0 ]; then
        return 0
    fi

    if ! command -v sudo >/dev/null 2>&1; then
        echo "sudo is required to install packages and inspect systemd state." | tee -a "${LOG_PATH}"
        exit 1
    fi
}

run_privileged() {
    if [ "$(id -u)" -eq 0 ]; then
        run "$@"
    else
        run sudo "$@"
    fi
}

run_privileged_allow_fail() {
    if [ "$(id -u)" -eq 0 ]; then
        run_allow_fail "$@"
    else
        run_allow_fail sudo "$@"
    fi
}

echo "ugripper ARM board install verification" | tee "${LOG_PATH}"
echo "log: ${LOG_PATH}" | tee -a "${LOG_PATH}"
echo "deb: ${DEB_PATH}" | tee -a "${LOG_PATH}"

if [ ! -f "${DEB_PATH}" ]; then
    echo "Package not found: ${DEB_PATH}" | tee -a "${LOG_PATH}"
    exit 1
fi

require_sudo

run uname -m
run dpkg --print-architecture
run ps -p 1 -o comm=
run ls -l "${DEB_PATH}"

run_privileged apt-get update
run_privileged apt-get install -y systemd udev kmod pulseaudio-utils sox alsa-utils libusb-1.0-0

run_privileged_allow_fail dpkg -i "${DEB_PATH}"
run_privileged_allow_fail apt --fix-broken install -y
run_privileged_allow_fail dpkg --configure ugripper

run_allow_fail dpkg -s ugripper
run_allow_fail grep '^User=' /etc/systemd/system/ugripper.service
run_privileged_allow_fail systemctl status ugripper.service --no-pager -l
run_privileged_allow_fail systemctl status umi-shutdown-trigger.path --no-pager -l
run_privileged_allow_fail journalctl -u ugripper.service -n 80 --no-pager -l

run_allow_fail ls -la /opt/ugripper
run_allow_fail find /opt/ugripper/bin -maxdepth 2 -type f | sort
run_allow_fail file /opt/ugripper/bin/UgripperRuntime/UgripperRuntime
run_allow_fail file /opt/ugripper/bin/CameraRecorder/CameraRecorder

echo
echo "Verification log saved to: ${LOG_PATH}" | tee -a "${LOG_PATH}"
