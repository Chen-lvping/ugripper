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
if [ -z "$CAMCHAIN_FILE" ]; then
    log "Missing camchain file in $TARGET_SN_DIR"
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

python3 - "$PERSIST_CALIB_FILE" "$FALLBACK_CAM_JSON" "$CAMCHAIN_FILE" "$PERSIST_CALIB_FILE" "$DEVICE_SN" <<'PY'
import json
import pathlib
import re
import sys
from datetime import datetime, timezone

base_json, fallback_json, camchain_file, output_json, device_sn = sys.argv[1:]


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


def ensure_image_entry(existing, width, height, fx, fy, cx, cy, camera_model, distortion_model, distortion_coeffs):
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
        "fps": (existing or {}).get("fps", "unknown") if isinstance(existing, dict) else "unknown",
    }


src_json_path = pathlib.Path(base_json)
if src_json_path.exists():
    data = json.loads(src_json_path.read_text(encoding="utf-8"))
else:
    data = json.loads(pathlib.Path(fallback_json).read_text(encoding="utf-8"))

camchain = parse_camchain(camchain_file)
main_key = "observation.images.{{CAM_MAIN}}"
if main_key not in data:
    cam_keys = [k for k in data.keys() if k.startswith("observation.images.cam_")]
    if cam_keys:
        main_key = cam_keys[0]

width, height = camchain["resolution"]
fx, fy, cx, cy = camchain["intrinsics"]
data[main_key] = ensure_image_entry(
    data.get(main_key),
    width,
    height,
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

for key in list(data.keys()):
    if key == main_key:
        continue
    if key.startswith("observation.images.") or key.startswith("observation.imu."):
        data.pop(key, None)

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
cat > "$IMPORT_SAVE_DIR/import_meta.txt" <<META
import_time_utc=$(date -u '+%Y-%m-%dT%H:%M:%SZ')
device_sn=$DEVICE_SN
usb_source_dir=$TARGET_SN_DIR
camchain_file=$(basename "$CAMCHAIN_FILE")
output_calibration_json=$PERSIST_CALIB_FILE
META

log "Calibration import completed for DEVICE_SN=$DEVICE_SN"
log "Updated: $PERSIST_CALIB_FILE"
exit 0
