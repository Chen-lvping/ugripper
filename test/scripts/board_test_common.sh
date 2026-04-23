#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PPMAIN_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

timestamp_slug() {
    date +"%Y%m%d_%H%M%S"
}

print_section() {
    printf '\n== %s ==\n' "$1"
}

default_ugripper_root() {
    if [[ -d /opt/ugripper ]]; then
        echo "/opt/ugripper"
        return
    fi
    echo "${PPMAIN_ROOT}/build/x86/standalone_ros2"
}

accept_timeout_exit() {
    local exit_code="$1"
    [[ "${exit_code}" == "0" || "${exit_code}" == "124" || "${exit_code}" == "130" || "${exit_code}" == "143" ]]
}

resolve_executable() {
    local explicit_path="$1"
    shift
    if [[ -n "${explicit_path}" ]]; then
        if [[ -x "${explicit_path}" ]]; then
            echo "${explicit_path}"
            return
        fi
        echo "missing executable: ${explicit_path}" >&2
        return 1
    fi

    local candidate
    for candidate in "$@"; do
        if [[ -x "${candidate}" ]]; then
            echo "${candidate}"
            return
        fi
    done

    echo "failed to resolve executable from candidates: $*" >&2
    return 1
}

resolve_file() {
    local explicit_path="$1"
    shift
    if [[ -n "${explicit_path}" ]]; then
        if [[ -f "${explicit_path}" ]]; then
            echo "${explicit_path}"
            return
        fi
        echo "missing file: ${explicit_path}" >&2
        return 1
    fi

    local candidate
    for candidate in "$@"; do
        if [[ -f "${candidate}" ]]; then
            echo "${candidate}"
            return
        fi
    done

    echo "failed to resolve file from candidates: $*" >&2
    return 1
}

default_sensor_recorder_bin() {
    local ugripper_root="${1}"
    resolve_executable "" \
        "${ugripper_root}/bin/SensorRecorder/SensorRecorder" \
        "${PPMAIN_ROOT}/build/x86/standalone_ros2/SensorRecorder/SensorRecorder"
}

default_camera_recorder_bin() {
    local ugripper_root="${1}"
    resolve_executable "" \
        "${ugripper_root}/bin/CameraRecorder/CameraRecorder" \
        "${PPMAIN_ROOT}/build/x86/standalone_ros2/CameraRecorder/CameraRecorder"
}

default_gripper_hmi_bin() {
    local ugripper_root="${1}"
    resolve_executable "" \
        "${ugripper_root}/bin/GripperHmiTool/GripperHmiTool" \
        "${PPMAIN_ROOT}/build/x86/standalone_ros2/GripperHmiTool/GripperHmiTool"
}

default_check_sensor_mcap_bin() {
    local explicit_path="${1:-}"
    resolve_executable "${explicit_path}" \
        "${PPMAIN_ROOT}/test/scripts/check_sensor_mcap" \
        "${PPMAIN_ROOT}/build/x86/test/src/sensor_recorder/check_sensor_mcap"
}

default_check_video_windows_script() {
    resolve_file "" \
        "${PPMAIN_ROOT}/test/scripts/check_video_windows.py"
}

default_check_gripper_ack_script() {
    resolve_file "" \
        "${PPMAIN_ROOT}/test/scripts/check_gripper_ack_log.py"
}

default_check_hmi_event_script() {
    resolve_file "" \
        "${PPMAIN_ROOT}/test/scripts/check_hmi_event_log.py"
}

default_camera_config_yaml() {
    local ugripper_root="${1}"
    resolve_file "" \
        "${ugripper_root}/bin/CameraRecorder/config/camera_recorder.yaml" \
        "${PPMAIN_ROOT}/standalone/CameraRecorder/config/camera_recorder.yaml"
}

find_default_episode_root() {
    local roots=()
    local candidate
    for candidate in /mnt/data_disk/*/data; do
        [[ -d "${candidate}" ]] || continue
        roots+=("${candidate}")
    done

    if [[ "${#roots[@]}" == "1" ]]; then
        echo "${roots[0]}"
        return
    fi

    if [[ "${#roots[@]}" -gt 1 ]]; then
        local latest_root=""
        local latest_score="-1"
        local latest_episode
        local score
        for candidate in "${roots[@]}"; do
            latest_episode="$(
                find "${candidate}" -maxdepth 1 -mindepth 1 -type d -name 'episode_*' \
                    -printf '%T@ %p\n' 2>/dev/null | sort -nr | head -n 1
            )"
            if [[ -n "${latest_episode}" ]]; then
                score="${latest_episode%% *}"
                if awk "BEGIN { exit !(${score} > ${latest_score}) }"; then
                    latest_score="${score}"
                    latest_root="${candidate}"
                fi
            fi
        done

        if [[ -n "${latest_root}" ]]; then
            echo "${latest_root}"
            return
        fi

        printf '%s\n' "${roots[@]}" | head -n 1
        return
    fi

    return 1
}

find_latest_episode_dir() {
    local episode_root="$1"
    local latest
    latest="$(find "${episode_root}" -maxdepth 1 -mindepth 1 -type d -name 'episode_*' -printf '%T@ %p\n' 2>/dev/null | sort -nr | head -n 1 | cut -d' ' -f2-)"
    [[ -n "${latest}" ]] || return 1
    echo "${latest}"
}

wait_for_path() {
    local path="$1"
    local timeout_sec="$2"
    local interval_sec="${3:-1}"
    local waited=0
    while [[ "${waited}" -lt "${timeout_sec}" ]]; do
        if [[ -e "${path}" ]]; then
            return 0
        fi
        sleep "${interval_sec}"
        waited=$((waited + interval_sec))
    done
    return 1
}

split_csv_to_args() {
    local csv="${1}"
    local prefix="${2}"
    local old_ifs="${IFS}"
    IFS=','
    read -r -a items <<< "${csv}"
    IFS="${old_ifs}"
    local item
    for item in "${items[@]}"; do
        if [[ -n "${item}" ]]; then
            printf '%s\n' "${prefix}" "${item}"
        fi
    done
}
