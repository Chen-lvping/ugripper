#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

resolve_repo_root() {
    local candidate=""
    local candidates=()

    if [ -n "${UGRIPPER_ROOT:-}" ]; then
        candidates+=("$UGRIPPER_ROOT")
    fi

    candidates+=(
        "$PWD"
        "/opt/ugripper"
        "$script_dir/.."
    )

    for candidate in "${candidates[@]}"; do
        [ -n "$candidate" ] || continue
        candidate="$(cd "$candidate" 2>/dev/null && pwd)" || continue

        if [ -x "$candidate/bin/GripperHmiTool/GripperHmiTool" ] && [ -d "$candidate/bin" ]; then
            printf '%s\n' "$candidate"
            return 0
        fi
    done

    return 1
}

repo_root="$(resolve_repo_root || true)"
if [ -z "$repo_root" ]; then
    echo "Failed to resolve ugripper repo root. Set UGRIPPER_ROOT or run from /opt/ugripper." >&2
    exit 1
fi

timestamp="$(date +%Y%m%d_%H%M%S)"
log_path="${LOG_PATH:-/tmp/ugripper_calib_roundtrip_smoke_${timestamp}.log}"
rollback_dir="${ROLLBACK_DIR:-/tmp/ugripper_calib_rollback_${timestamp}}"
usb_calib_root="${USB_CALIB_ROOT:-/mnt/data_disk/ugripper_calib}"
gripper_tool="${GRIPPER_HMI_TOOL:-$repo_root/bin/GripperHmiTool/GripperHmiTool}"
timeout_sec="${GRIPPER_TIMEOUT_SEC:-8}"

service_was_active=0
service_stopped=0
rollback_armed=0
restore_done=0

usage() {
    cat <<'EOF'
Usage: scripts/run_ugripper_calib_roundtrip_smoke.sh [options]

Run a board-side ugripper_calib round-trip smoke:
1. Stop ugripper.service
2. Backup host calibration.json and current left/right gripper calibration bins
3. Build a temporary ugripper_calib payload from the currently active calibration
4. Trigger usb-auto-update@<device> to import it
5. Restore the original host/gripper calibration immediately
6. Verify byte-for-byte rollback, then restart ugripper.service if it was active

Options:
  --log-path <path>       Write combined output log to this file
  --rollback-dir <path>   Store backup and verification artifacts in this directory
  --usb-calib-root <dir>  Temporary U-disk calibration payload root
  --timeout-sec <sec>     Timeout for GripperHmiTool commands. Default: 8
  -h, --help              Show this help

Environment overrides:
  LOG_PATH
  ROLLBACK_DIR
  USB_CALIB_ROOT
  GRIPPER_HMI_TOOL
  GRIPPER_TIMEOUT_SEC
EOF
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --log-path)
            log_path="$2"
            shift 2
            ;;
        --rollback-dir)
            rollback_dir="$2"
            shift 2
            ;;
        --usb-calib-root)
            usb_calib_root="$2"
            shift 2
            ;;
        --timeout-sec)
            timeout_sec="$2"
            shift 2
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "Unknown option: $1" >&2
            usage >&2
            exit 1
            ;;
    esac
done

mkdir -p "$(dirname "$log_path")" "$rollback_dir"
exec > >(tee -a "$log_path") 2>&1

info() {
    echo "[INFO] $*"
}

warn() {
    echo "[WARN] $*" >&2
}

require_cmd() {
    local cmd="$1"
    if ! command -v "$cmd" >/dev/null 2>&1; then
        echo "Required command not found: $cmd" >&2
        exit 1
    fi
}

run() {
    echo
    echo "\$ $*"
    "$@"
}

run_allow_fail() {
    local status=0
    echo
    echo "\$ $*"
    set +e
    "$@"
    status=$?
    set -e
    echo "[exit_code] $status"
    return 0
}

run_privileged() {
    if [ "$(id -u)" -eq 0 ]; then
        run "$@"
    else
        run sudo "$@"
    fi
}

capture_imported_dirs() {
    local out="$1"
    : > "$out"

    if [ "$(id -u)" -eq 0 ]; then
        find "/etc/ugripper/config/calibration/imported/$device_sn" \
            -mindepth 1 \
            -maxdepth 1 \
            -type d \
            2>/dev/null | sort > "$out" || true
    else
        sudo find "/etc/ugripper/config/calibration/imported/$device_sn" \
            -mindepth 1 \
            -maxdepth 1 \
            -type d \
            2>/dev/null | sort > "$out" || true
    fi
}

stop_service_if_active() {
    if systemctl is-active --quiet ugripper.service; then
        run_privileged systemctl stop ugripper.service
        service_stopped=1
        run sleep 2
    fi
}

resume_service_if_needed() {
    if [ "$service_was_active" -eq 1 ]; then
        run_privileged systemctl start ugripper.service
        service_stopped=0
        run sleep 3
    fi
}

read_sn() {
    local port="$1"
    timeout "${timeout_sec}s" "$gripper_tool" --port "$port" --read-sn \
        | sed -n "s#^\\[$port\\] sn=##p" | tail -n1
}

read_calib_bin() {
    local port="$1"
    local output_bin="$2"
    timeout "${timeout_sec}s" "$gripper_tool" --port "$port" --read-calib --dump-calib-bin "$output_bin"
}

write_calib_bin() {
    local port="$1"
    local input_bin="$2"
    timeout "${timeout_sec}s" "$gripper_tool" --port "$port" --write-calib-bin "$input_bin"
}

restore_payload() {
    if [ "$restore_done" -eq 1 ]; then
        return 0
    fi

    info "Restoring original calibration payload..."
    stop_service_if_active

    if [ -f "$rollback_dir/calibration.json.before" ]; then
        run_privileged cp -f \
            "$rollback_dir/calibration.json.before" \
            /etc/ugripper/config/calibration/calibration.json
    fi

    if [ -f "$rollback_dir/left.before.bin" ]; then
        run write_calib_bin /dev/left_gripper "$rollback_dir/left.before.bin"
    fi

    if [ -f "$rollback_dir/right.before.bin" ]; then
        run write_calib_bin /dev/right_gripper "$rollback_dir/right.before.bin"
    fi

    if [ -f "$rollback_dir/imported_dirs.before.txt" ] && [ -f "$rollback_dir/imported_dirs.after_import.txt" ]; then
        while IFS= read -r dir; do
            [ -n "$dir" ] || continue
            run_privileged rm -rf "$dir"
        done < <(comm -13 "$rollback_dir/imported_dirs.before.txt" "$rollback_dir/imported_dirs.after_import.txt" || true)
    fi

    run rm -rf "$usb_calib_root"
    restore_done=1
}

verify_restored() {
    info "Verifying restored state..."
    run read_calib_bin /dev/left_gripper "$rollback_dir/left.after_restore.bin"
    run read_calib_bin /dev/right_gripper "$rollback_dir/right.after_restore.bin"
    run_privileged cp -a \
        /etc/ugripper/config/calibration/calibration.json \
        "$rollback_dir/calibration.json.after_restore"

    local left_ok="FAIL"
    local right_ok="FAIL"
    local host_ok="FAIL"

    if cmp -s "$rollback_dir/left.before.bin" "$rollback_dir/left.after_restore.bin"; then
        left_ok="OK"
    fi
    if cmp -s "$rollback_dir/right.before.bin" "$rollback_dir/right.after_restore.bin"; then
        right_ok="OK"
    fi
    if cmp -s "$rollback_dir/calibration.json.before" "$rollback_dir/calibration.json.after_restore"; then
        host_ok="OK"
    fi

    echo "left_calib_restored=$left_ok"
    echo "right_calib_restored=$right_ok"
    echo "host_calibration_restored=$host_ok"

    run sha256sum \
        "$rollback_dir/calibration.json.after_restore" \
        "$rollback_dir/left.after_restore.bin" \
        "$rollback_dir/right.after_restore.bin"

    if [ "$left_ok" != "OK" ] || [ "$right_ok" != "OK" ] || [ "$host_ok" != "OK" ]; then
        echo "Rollback verification failed" >&2
        exit 1
    fi
}

on_exit() {
    local status=$?

    if [ "$rollback_armed" -eq 1 ]; then
        set +e
        restore_payload
        resume_service_if_needed
        set -e
    elif [ "$service_was_active" -eq 1 ] && [ "$service_stopped" -eq 1 ]; then
        set +e
        resume_service_if_needed
        set -e
    fi

    exit "$status"
}

trap on_exit EXIT

require_cmd basename
require_cmd cmp
require_cmd comm
require_cmd findmnt
require_cmd grep
require_cmd python3
require_cmd sed
require_cmd sha256sum
require_cmd systemctl
require_cmd tee
require_cmd timeout

if [ ! -x "$gripper_tool" ]; then
    echo "GripperHmiTool not found or not executable: $gripper_tool" >&2
    exit 1
fi

cd "$repo_root"

device="$(basename "$(findmnt -rn --target /mnt/data_disk -o SOURCE)")"
device_sn="$(grep -E '^DEVICE_SN=' /etc/environment | cut -d= -f2- | tr -d '"' | xargs || true)"

if [ -z "$device" ] || [ -z "$device_sn" ]; then
    echo "Failed to resolve DEVICE or DEVICE_SN" >&2
    exit 1
fi

if [ -e "$usb_calib_root" ]; then
    echo "Refusing to proceed: $usb_calib_root already exists" >&2
    exit 1
fi

info "repo_root=$repo_root"
info "log_path=$log_path"
info "rollback_dir=$rollback_dir"
info "device=$device"
info "device_sn=$device_sn"

if systemctl is-active --quiet ugripper.service; then
    service_was_active=1
fi

capture_imported_dirs "$rollback_dir/imported_dirs.before.txt"
stop_service_if_active
run_allow_fail ps -ef

left_sn="$(read_sn /dev/left_gripper)"
right_sn="$(read_sn /dev/right_gripper)"

if [ -z "$left_sn" ] || [ -z "$right_sn" ]; then
    echo "Failed to read left/right gripper SN" >&2
    exit 1
fi

info "left_sn=$left_sn"
info "right_sn=$right_sn"

run_privileged cp -a \
    /etc/ugripper/config/calibration/calibration.json \
    "$rollback_dir/calibration.json.before"
run read_calib_bin /dev/left_gripper "$rollback_dir/left.before.bin"
run read_calib_bin /dev/right_gripper "$rollback_dir/right.before.bin"
run sha256sum \
    "$rollback_dir/calibration.json.before" \
    "$rollback_dir/left.before.bin" \
    "$rollback_dir/right.before.bin"

rollback_armed=1

export DEVICE_SN="$device_sn"
export LEFT_SN="$left_sn"
export RIGHT_SN="$right_sn"
export USB_CALIB_ROOT="$usb_calib_root"

run python3 - <<'PY'
import json
import os
import pathlib
import textwrap

device_sn = os.environ["DEVICE_SN"]
left_sn = os.environ["LEFT_SN"]
right_sn = os.environ["RIGHT_SN"]
usb_calib_root = pathlib.Path(os.environ["USB_CALIB_ROOT"])
calib_path = pathlib.Path("/etc/ugripper/config/calibration/calibration.json")
data = json.loads(calib_path.read_text(encoding="utf-8"))


def first_intrinsics(node):
    intr_map = node.get("intrinsics")
    if not isinstance(intr_map, dict) or not intr_map:
        raise SystemExit(f"missing intrinsics in {node}")
    key = sorted(intr_map.keys())[0]
    intr = intr_map[key]
    width, height = [int(x) for x in key.split("x", 1)]
    return width, height, float(intr["fx"]), float(intr["fy"]), float(intr["ppx"]), float(intr["ppy"])


def pad4(values):
    vals = list(values or [])
    vals = vals[:4]
    while len(vals) < 4:
        vals.append(0.0)
    return [float(x) for x in vals]


def get_main(side):
    key = f"observation.images.{side}_cam_main"
    node = data.get(key)
    if not isinstance(node, dict):
        raise SystemExit(f"missing {key}")
    width, height, fx, fy, cx, cy = first_intrinsics(node)
    return {
        "camera_model": node.get("camera_model", "pinhole"),
        "distortion_model": node.get("distortion_model", "equidistant"),
        "distortion_coeffs": pad4(node.get("distortion_coeffs") or node.get("distortion_coefficients") or []),
        "width": width,
        "height": height,
        "fx": fx,
        "fy": fy,
        "cx": cx,
        "cy": cy,
    }


def get_stereo(side):
    stereo_node = data.get(f"observation.images.{side}_stereo")
    if isinstance(stereo_node, dict) and isinstance(stereo_node.get("stereo"), dict):
        return stereo_node["stereo"]
    calib_info = data.get("calibration_info")
    if isinstance(calib_info, dict):
        bundles = calib_info.get("stereo_imu_bundles")
        if isinstance(bundles, dict) and isinstance(bundles.get(side), dict):
            return bundles[side]
    raise SystemExit(f"missing stereo payload for side={side}")


def to_rows(matrix):
    if isinstance(matrix, list) and len(matrix) == 4 and all(isinstance(row, list) and len(row) == 4 for row in matrix):
        return [[float(x) for x in row] for row in matrix]
    if isinstance(matrix, list) and len(matrix) == 16:
        flat = [float(x) for x in matrix]
        return [flat[i:i + 4] for i in range(0, 16, 4)]
    raise SystemExit(f"unexpected matrix format: {matrix}")


def fmt_matrix(matrix):
    rows = to_rows(matrix)
    return "[[" + "],\n [".join(", ".join(str(x) for x in row) for row in rows) + "]]"


def render_side(side, sn):
    main = get_main(side)
    stereo = get_stereo(side)
    side_dir = usb_calib_root / device_sn / sn
    side_dir.mkdir(parents=True, exist_ok=True)

    camchain = textwrap.dedent(
        f"""\
        cam0:
          camera_model: {main['camera_model']}
          distortion_model: {main['distortion_model']}
          intrinsics: [{main['fx']}, {main['fy']}, {main['cx']}, {main['cy']}]
          distortion_coeffs: [{", ".join(str(x) for x in main['distortion_coeffs'])}]
          resolution: [{main['width']}, {main['height']}]
          rostopic: /{side}_cam_main
        """
    )
    (side_dir / "rgb_video_ros-camchain.yaml").write_text(camchain, encoding="utf-8")

    cam0 = stereo["cam0"]
    cam1 = stereo["cam1"]
    ex = stereo["extrinsics"]
    imu = stereo["imu0"]
    res = stereo["residuals"]

    imucam = textwrap.dedent(
        f"""\
        Residuals
        -----
        Reprojection error (cam0) [px]: mean {res['reprojection_error_cam0_px']['mean']}, median {res['reprojection_error_cam0_px']['median']}, std: {res['reprojection_error_cam0_px']['std']}
        Reprojection error (cam1) [px]: mean {res['reprojection_error_cam1_px']['mean']}, median {res['reprojection_error_cam1_px']['median']}, std: {res['reprojection_error_cam1_px']['std']}
        Gyroscope error (imu0) [rad/s]: mean {res['gyroscope_error_imu0_rad_s']['mean']}, median {res['gyroscope_error_imu0_rad_s']['median']}, std: {res['gyroscope_error_imu0_rad_s']['std']}
        Accelerometer error (imu0) [m/s^2]: mean {res['accelerometer_error_imu0_m_s2']['mean']}, median {res['accelerometer_error_imu0_m_s2']['median']}, std: {res['accelerometer_error_imu0_m_s2']['std']}

        Transformation (cam0):
        -----
        T_ic:  (cam0 to imu0):
        {fmt_matrix(ex['T_ic_cam0_to_imu0'])}
        timeshift cam0 to imu0: [s] (t_imu = t_cam + shift)
        {float(ex['timeshift_cam0_to_imu0'])}

        Transformation (cam1):
        -----
        T_ic:  (cam1 to imu0):
        {fmt_matrix(ex['T_ic_cam1_to_imu0'])}
        timeshift cam1 to imu0: [s] (t_imu = t_cam + shift)
        {float(ex['timeshift_cam1_to_imu0'])}

        Baselines:
        -----
        baseline norm: {float(ex['baseline_norm'])}

        Gravity vector in target coords:
        -----
        [0, 0, -9.81]

        cam0
        -----
        Camera model: {cam0['camera_model']}
        Distortion model: {cam0['distortion_model']}
        Focal length: [{float(cam0['focal_length'][0])}, {float(cam0['focal_length'][1])}]
        Principal point: [{float(cam0['principal_point'][0])}, {float(cam0['principal_point'][1])}]
        Distortion coefficients: [{", ".join(str(float(x)) for x in pad4(cam0.get('distortion_coefficients') or []))}]

        cam1
        -----
        Camera model: {cam1['camera_model']}
        Distortion model: {cam1['distortion_model']}
        Focal length: [{float(cam1['focal_length'][0])}, {float(cam1['focal_length'][1])}]
        Principal point: [{float(cam1['principal_point'][0])}, {float(cam1['principal_point'][1])}]
        Distortion coefficients: [{", ".join(str(float(x)) for x in pad4(cam1.get('distortion_coefficients') or []))}]


        IMU configuration
        IMU0:
        -----
        Model: {imu.get('model', 'calibrated')}
        Update rate: {float(imu['update_rate_hz'])}
        Accelerometer:
          Noise density (discrete): {float(imu['accelerometer']['noise_density_discrete'])}
          Random walk: {float(imu['accelerometer']['random_walk'])}
        Gyroscope:
          Noise density (discrete): {float(imu['gyroscope']['noise_density_discrete'])}
          Random walk: {float(imu['gyroscope']['random_walk'])}
        """
    )
    (side_dir / "output-results-imucam.txt").write_text(imucam, encoding="utf-8")


render_side("left", left_sn)
render_side("right", right_sn)

print(str(usb_calib_root / device_sn))
PY

run find "$usb_calib_root/$device_sn" -maxdepth 2 -type f
run_privileged systemctl start "usb-auto-update@$device"

for _ in $(seq 1 30); do
    if ! systemctl is-active --quiet "usb-auto-update@$device"; then
        break
    fi
    sleep 1
done

run_privileged journalctl -u "usb-auto-update@$device" -n 120 --no-pager -l
run_privileged tail -n 120 /var/log/ugripper/usb_auto_update.log

capture_imported_dirs "$rollback_dir/imported_dirs.after_import.txt"
run_privileged cp -a \
    /etc/ugripper/config/calibration/calibration.json \
    "$rollback_dir/calibration.json.after_import"

restore_payload
verify_restored
resume_service_if_needed

if [ "$service_was_active" -eq 1 ]; then
    run_allow_fail systemctl status ugripper.service --no-pager -l
    run_allow_fail cat /tmp/umi_stereo_camera_status.json
fi

rollback_armed=0
info "rollback_dir=$rollback_dir"
info "done"
