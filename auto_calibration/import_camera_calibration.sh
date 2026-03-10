#!/bin/bash
set -euo pipefail

USB_ROOT="${1:-}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

log() {
    echo "[calib-import][$(date '+%Y-%m-%d %H:%M:%S')] $*"
}

read_env_value() {
    local key="$1"
    if [ ! -f /etc/environment ]; then
        return 0
    fi

    grep -E "^${key}=" /etc/environment \
        | head -n1 \
        | cut -d= -f2- \
        | tr -d '"' \
        | xargs
}

if [ -z "$USB_ROOT" ] || [ ! -d "$USB_ROOT" ]; then
    log "USB root invalid: $USB_ROOT"
    exit 1
fi

DEVICE_SN="${DEVICE_SN_OVERRIDE:-$(read_env_value "DEVICE_SN")}"
if [ -z "$DEVICE_SN" ]; then
    log "DEVICE_SN not found in /etc/environment"
    exit 1
fi

CALIB_ROOT="$USB_ROOT/ugripper_calib"
if [ ! -d "$CALIB_ROOT" ]; then
    log "No ugripper_calib directory under USB"
    exit 10
fi

TARGET_SN_DIR="$CALIB_ROOT/$DEVICE_SN"
if [ ! -d "$TARGET_SN_DIR" ]; then
    TARGET_SN_DIR=""
    while IFS= read -r dir; do
        base="$(basename "$dir")"
        if [ "${base,,}" = "${DEVICE_SN,,}" ]; then
            TARGET_SN_DIR="$dir"
            break
        fi
    done < <(find "$CALIB_ROOT" -mindepth 1 -maxdepth 1 -type d | sort)
fi

if [ -z "$TARGET_SN_DIR" ] || [ ! -d "$TARGET_SN_DIR" ]; then
    log "No calibration bundle for DEVICE_SN=$DEVICE_SN"
    exit 10
fi

CAMCHAIN_FILE="$(find "$TARGET_SN_DIR" -maxdepth 2 -type f \( -name '*camchain*.yaml' -o -name '*camchain*.yml' \) | sort | head -n1 || true)"
IMUCAM_FILE="$(find "$TARGET_SN_DIR" -maxdepth 2 -type f -name '*imucam*.txt' | sort | head -n1 || true)"

if [ -z "$CAMCHAIN_FILE" ] || [ -z "$IMUCAM_FILE" ]; then
    log "Missing camchain/imucam file in $TARGET_SN_DIR"
    exit 1
fi

PERSIST_CALIB_DIR="${CALIB_PERSIST_DIR_OVERRIDE:-/etc/ugripper/config/calibration}"
PERSIST_CALIB_FILE="$PERSIST_CALIB_DIR/calibration.json"
FALLBACK_CAM_JSON="$PROJECT_ROOT/config/fakeCamCalib.json"

if [ ! -f "$PERSIST_CALIB_FILE" ] && [ ! -f "$FALLBACK_CAM_JSON" ]; then
    log "Base calibration.json missing (persist + fallback)"
    exit 1
fi

mkdir -p "$PERSIST_CALIB_DIR"

python3 - "$PERSIST_CALIB_FILE" "$FALLBACK_CAM_JSON" "$CAMCHAIN_FILE" "$IMUCAM_FILE" "$PERSIST_CALIB_FILE" "$DEVICE_SN" <<'PY'
import json
import pathlib
import re
import sys
from datetime import datetime, timezone

base_json, fallback_json, camchain_file, imucam_file, output_json, device_sn = sys.argv[1:]
num_re = r"[-+]?\d*\.?\d+(?:[eE][-+]?\d+)?"


def load_text(path):
    return pathlib.Path(path).read_text(encoding="utf-8")


def parse_num_list(raw):
    return [float(x.strip()) for x in raw.split(",") if x.strip()]


def parse_line_list(text, key):
    m = re.search(rf"^\s*{re.escape(key)}:\s*\[([^\]]+)\]", text, re.M)
    if not m:
        return None
    return parse_num_list(m.group(1))


def parse_line_scalar(text, key):
    m = re.search(rf"^\s*{re.escape(key)}:\s*(.+?)\s*$", text, re.M)
    return m.group(1).strip() if m else None


def parse_camchain(path):
    text = load_text(path)
    intr = parse_line_list(text, "intrinsics")
    dist = parse_line_list(text, "distortion_coeffs") or []
    res = parse_line_list(text, "resolution")

    if not intr or len(intr) < 4 or not res or len(res) < 2:
        raise ValueError("camchain missing intrinsics/resolution")

    return {
        "camera_model": parse_line_scalar(text, "camera_model") or "pinhole",
        "distortion_model": parse_line_scalar(text, "distortion_model") or "equidistant",
        "intrinsics": intr[:4],
        "distortion_coeffs": dist,
        "resolution": [int(round(res[0])), int(round(res[1]))],
        "rostopic": parse_line_scalar(text, "rostopic"),
    }


def parse_matrix_after(text, marker):
    idx = text.find(marker)
    if idx < 0:
        return []

    rows = []
    for line in text[idx:].splitlines()[1:]:
        nums = re.findall(num_re, line)
        if nums:
            rows.append([float(x) for x in nums])
            if len(rows) == 4:
                break
        elif rows:
            break
    return rows


def find_float_after(text, pattern):
    m = re.search(pattern, text, re.S)
    return float(m.group(1)) if m else None


def parse_cam_cfg(text, label):
    m = re.search(
        rf"^\s*{re.escape(label)}\s*$\n^\s*-+\s*$\n(.*?)(?=^\s*cam\d\s*$|^\s*IMU configuration\s*$|\Z)",
        text,
        re.M | re.S,
    )
    if not m:
        raise ValueError(f"missing config block for {label}")

    block = m.group(1)

    def list_field(name):
        mm = re.search(rf"{re.escape(name)}:\s*\[([^\]]+)\]", block)
        if not mm:
            return None
        return parse_num_list(mm.group(1))

    def scalar_field(name):
        mm = re.search(rf"{re.escape(name)}:\s*(.+?)\s*$", block, re.M)
        return mm.group(1).strip() if mm else None

    fl = list_field("Focal length")
    pp = list_field("Principal point")
    dc = list_field("Distortion coefficients") or []
    if not fl or len(fl) < 2 or not pp or len(pp) < 2:
        raise ValueError(f"missing intrinsics in {label}")

    return {
        "camera_model": scalar_field("Camera model") or "pinhole",
        "distortion_model": scalar_field("Distortion model") or "equidistant",
        "intrinsics": [fl[0], fl[1], pp[0], pp[1]],
        "distortion_coeffs": dc,
    }


def parse_imu_cfg(text):
    m = re.search(r"^\s*IMU0:\s*$\n^\s*-+\s*$\n(.*?)(?=\Z)", text, re.M | re.S)
    if not m:
        return {}

    block = m.group(1)

    def pick(pattern):
        mm = re.search(pattern, block, re.M | re.S)
        return float(mm.group(1)) if mm else None

    model_m = re.search(r"^\s*Model:\s*(.+?)\s*$", block, re.M)

    return {
        "model": model_m.group(1).strip() if model_m else None,
        "update_rate_hz": pick(rf"Update rate:\s*({num_re})"),
        "acc_noise_density": pick(rf"Accelerometer:\s*\n\s*Noise density:\s*({num_re})"),
        "acc_noise_density_discrete": pick(rf"Accelerometer:.*?Noise density \(discrete\):\s*({num_re})"),
        "acc_random_walk": pick(rf"Accelerometer:.*?Random walk:\s*({num_re})"),
        "gyro_noise_density": pick(rf"Gyroscope:\s*\n\s*Noise density:\s*({num_re})"),
        "gyro_noise_density_discrete": pick(rf"Gyroscope:.*?Noise density \(discrete\):\s*({num_re})"),
        "gyro_random_walk": pick(rf"Gyroscope:.*?Random walk:\s*({num_re})"),
    }


def parse_imucam(path):
    text = load_text(path)

    cam0 = parse_cam_cfg(text, "cam0")
    cam1 = parse_cam_cfg(text, "cam1")

    return {
        "cam0": cam0,
        "cam1": cam1,
        "extrinsics": {
            "imu0_to_cam0_T_ci": parse_matrix_after(text, "T_ci:  (imu0 to cam0):"),
            "cam0_to_imu0_T_ic": parse_matrix_after(text, "T_ic:  (cam0 to imu0):"),
            "imu0_to_cam1_T_ci": parse_matrix_after(text, "T_ci:  (imu0 to cam1):"),
            "cam1_to_imu0_T_ic": parse_matrix_after(text, "T_ic:  (cam1 to imu0):"),
            "cam0_to_cam1_T_01": parse_matrix_after(text, "Baseline (cam0 to cam1):"),
            "baseline_norm_m": find_float_after(text, rf"baseline norm:\s*({num_re})"),
            "timeshift_cam0_to_imu0_sec": find_float_after(text, rf"timeshift cam0 to imu0:.*?\n\s*({num_re})"),
            "timeshift_cam1_to_imu0_sec": find_float_after(text, rf"timeshift cam1 to imu0:.*?\n\s*({num_re})"),
        },
        "residuals": {
            "reprojection_error_cam0_mean_px": find_float_after(text, rf"Reprojection error \(cam0\):\s*mean\s*({num_re})"),
            "reprojection_error_cam1_mean_px": find_float_after(text, rf"Reprojection error \(cam1\):\s*mean\s*({num_re})"),
            "gyro_error_mean_rad_s": find_float_after(text, rf"Gyroscope error \(imu0\):\s*mean\s*({num_re})"),
            "acc_error_mean_m_s2": find_float_after(text, rf"Accelerometer error \(imu0\):\s*mean\s*({num_re})"),
        },
        "imu0": parse_imu_cfg(text),
    }


def resolve_shape(existing, default_h, default_w):
    shape = existing.get("shape") if isinstance(existing, dict) else None
    if isinstance(shape, list) and len(shape) >= 2 and all(isinstance(x, (int, float)) for x in shape[:2]):
        return int(shape[0]), int(shape[1])
    return default_h, default_w


def ensure_image_entry(existing, width, height, fx, fy, cx, cy, camera_model, distortion_model, distortion_coeffs):
    base = existing if isinstance(existing, dict) else {}
    fps = base.get("fps", 60)

    return {
        "shape": [height, width, 3],
        "names": ["height", "width", "channels"],
        "info": None,
        "intrinsics": {
            f"{width}x{height}": {
                "fx": fx,
                "fy": fy,
                "ppx": cx,
                "ppy": cy,
            }
        },
        "camera_model": camera_model,
        "distortion_model": distortion_model,
        "distortion_coeffs": distortion_coeffs,
        "dtype": "video",
        "fps": fps,
    }


src_json_path = pathlib.Path(base_json)
if src_json_path.exists():
    data = json.loads(src_json_path.read_text(encoding="utf-8"))
else:
    data = json.loads(pathlib.Path(fallback_json).read_text(encoding="utf-8"))

camchain = parse_camchain(camchain_file)
imucam = parse_imucam(imucam_file)

main_key = "observation.images.{{CAM_MAIN}}"
if main_key not in data:
    cam_keys = [k for k in data.keys() if k.startswith("observation.images.cam_")]
    if cam_keys:
        main_key = cam_keys[0]

w_main, h_main = camchain["resolution"]
fx, fy, cx, cy = camchain["intrinsics"]
data[main_key] = ensure_image_entry(
    data.get(main_key),
    w_main,
    h_main,
    fx,
    fy,
    cx,
    cy,
    camchain["camera_model"],
    camchain["distortion_model"],
    camchain["distortion_coeffs"],
)
if camchain.get("rostopic"):
    data[main_key]["rostopic"] = camchain["rostopic"]

for idx in (0, 1):
    key = f"observation.images.fays_cam{idx}"
    cam_cfg = imucam[f"cam{idx}"]
    fx_i, fy_i, cx_i, cy_i = cam_cfg["intrinsics"]
    h, w = resolve_shape(data.get(key, {}), 480, 640)
    data[key] = ensure_image_entry(
        data.get(key),
        w,
        h,
        fx_i,
        fy_i,
        cx_i,
        cy_i,
        cam_cfg["camera_model"],
        cam_cfg["distortion_model"],
        cam_cfg["distortion_coeffs"],
    )

imu_key = "observation.imu.fays_imu0"
existing_imu = data.get(imu_key, {}) if isinstance(data.get(imu_key), dict) else {}
imu_cfg = imucam.get("imu0", {})
data[imu_key] = {
    "dtype": "imu",
    "model": imu_cfg.get("model") or existing_imu.get("model") or "calibrated",
    "update_rate_hz": imu_cfg.get("update_rate_hz") or existing_imu.get("update_rate_hz"),
    "acc_noise_density": imu_cfg.get("acc_noise_density") or existing_imu.get("acc_noise_density"),
    "acc_noise_density_discrete": imu_cfg.get("acc_noise_density_discrete") or existing_imu.get("acc_noise_density_discrete"),
    "acc_random_walk": imu_cfg.get("acc_random_walk") or existing_imu.get("acc_random_walk"),
    "gyro_noise_density": imu_cfg.get("gyro_noise_density") or existing_imu.get("gyro_noise_density"),
    "gyro_noise_density_discrete": imu_cfg.get("gyro_noise_density_discrete") or existing_imu.get("gyro_noise_density_discrete"),
    "gyro_random_walk": imu_cfg.get("gyro_random_walk") or existing_imu.get("gyro_random_walk"),
}

metadata = data.get("metadata", {}) if isinstance(data.get("metadata"), dict) else {}
metadata["format_version"] = metadata.get("format_version", "1.0")
metadata["generation_date"] = datetime.now(timezone.utc).strftime("%Y-%m-%d")
metadata["description"] = "Camera calibration parameters"
metadata["calibration_status"] = "calibrated"
data["metadata"] = metadata

calib_info = data.get("calibration_info", {}) if isinstance(data.get("calibration_info"), dict) else {}
calib_info["calibration_date"] = datetime.now(timezone.utc).strftime("%Y-%m-%d")
calib_info["calibration_status"] = "calibrated"
calib_info["device_sn"] = device_sn
calib_info["main_camera"] = {
    "source": pathlib.Path(camchain_file).name,
    "camera_model": camchain["camera_model"],
    "distortion_model": camchain["distortion_model"],
    "resolution": camchain["resolution"],
}
calib_info["fays_imu_bundle"] = {
    "source": pathlib.Path(imucam_file).name,
    "extrinsics": imucam.get("extrinsics", {}),
    "residuals": imucam.get("residuals", {}),
}
calib_info["notes"] = (
    "Auto-imported from USB by device SN. "
    "Runtime will still align {{CAM_MAIN}} and tactile serial values."
)
data["calibration_info"] = calib_info

out_path = pathlib.Path(output_json)
out_path.parent.mkdir(parents=True, exist_ok=True)
out_path.write_text(json.dumps(data, indent=4, ensure_ascii=False) + "\n", encoding="utf-8")
PY

IMPORT_STAMP="$(date +%Y%m%d_%H%M%S)"
IMPORT_SAVE_DIR="$PERSIST_CALIB_DIR/imported/${DEVICE_SN}/${IMPORT_STAMP}"
mkdir -p "$IMPORT_SAVE_DIR"
cp -f "$CAMCHAIN_FILE" "$IMPORT_SAVE_DIR/"
cp -f "$IMUCAM_FILE" "$IMPORT_SAVE_DIR/"
cat > "$IMPORT_SAVE_DIR/import_meta.txt" <<META
import_time_utc=$(date -u '+%Y-%m-%dT%H:%M:%SZ')
device_sn=$DEVICE_SN
usb_source_dir=$TARGET_SN_DIR
camchain_file=$(basename "$CAMCHAIN_FILE")
imucam_file=$(basename "$IMUCAM_FILE")
output_calibration_json=$PERSIST_CALIB_FILE
META

log "Calibration import completed for DEVICE_SN=$DEVICE_SN"
log "Updated: $PERSIST_CALIB_FILE"
exit 0
