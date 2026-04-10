#!/bin/bash
set -euo pipefail

USB_ROOT="${1:-}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
DEFAULT_INSTALL_ROOT="/opt/ugripper"
HMI_HELPER_BIN="${HMI_HELPER_BIN_OVERRIDE:-$PROJECT_ROOT/build/src/gripper_hmi/gripper_hmi_test}"
GRIPPER_CALIB_GENERATOR="${GRIPPER_CALIB_GENERATOR_OVERRIDE:-$PROJECT_ROOT/auto_calibration/generate_gripper_calibration_bin.py}"
HMI_RUN_USER="${HMI_RUN_USER_OVERRIDE:-ubuntu}"

log() {
    echo "[calib-import][$(date '+%Y-%m-%d %H:%M:%S')] $*"
}

resolve_import_sides() {
    local raw="${IMPORT_SIDES_OVERRIDE:-left,right}"
    local cleaned="${raw// /}"
    local part=""
    local result=()

    IFS=',' read -r -a parts <<< "$cleaned"
    for part in "${parts[@]}"; do
        case "$part" in
            left|right)
                result+=("$part")
                ;;
            "")
                ;;
            *)
                log "Unsupported IMPORT_SIDES_OVERRIDE entry: $part"
                return 1
                ;;
        esac
    done

    if [ "${#result[@]}" -eq 0 ]; then
        log "No valid import sides resolved from IMPORT_SIDES_OVERRIDE=$raw"
        return 1
    fi

    printf '%s\n' "${result[@]}"
}

detect_camera_side() {
    local path="$1"
    local filename stem

    filename="$(basename "$path")"
    stem="${filename%.*}"
    stem="${stem,,}"

    case "$stem" in
        *_left)
            printf 'left'
            return 0
            ;;
        *_right)
            printf 'right'
            return 0
            ;;
    esac

    return 1
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

resolve_existing_path() {
    local candidate="$1"
    local fallback="$2"

    if [ -x "$candidate" ] || [ -f "$candidate" ]; then
        printf '%s' "$candidate"
        return 0
    fi

    if [ -x "$fallback" ] || [ -f "$fallback" ]; then
        printf '%s' "$fallback"
        return 0
    fi

    printf '%s' "$candidate"
    return 0
}

HMI_HELPER_BIN="$(resolve_existing_path "$HMI_HELPER_BIN" "$DEFAULT_INSTALL_ROOT/build/src/gripper_hmi/gripper_hmi_test")"
GRIPPER_CALIB_GENERATOR="$(resolve_existing_path "$GRIPPER_CALIB_GENERATOR" "$DEFAULT_INSTALL_ROOT/auto_calibration/generate_gripper_calibration_bin.py")"

run_hmi_helper() {
    if [ ! -x "$HMI_HELPER_BIN" ]; then
        log "HMI helper not found or not executable: $HMI_HELPER_BIN"
        return 1
    fi

    if [ "$(id -u)" -eq 0 ] && id "$HMI_RUN_USER" >/dev/null 2>&1; then
        runuser -u "$HMI_RUN_USER" -- "$HMI_HELPER_BIN" "$@"
    else
        "$HMI_HELPER_BIN" "$@"
    fi
}

read_gripper_sn() {
    local port="$1"
    local output=""
    local sn=""

    if ! output="$(run_hmi_helper --port "$port" --read-sn 2>&1)"; then
        log "读取夹爪 SN 失败：port=$port output=$(printf '%s' "$output" | tr '\n' ' ')"
        return 1
    fi

    sn="$(printf '%s\n' "$output" | sed -n "s#^\\[$port\\] sn=##p" | tail -n1 | tr -d '\r')"
    if ! printf '%s' "$sn" | grep -Eq '^[A-Za-z0-9]{16}$'; then
        log "读取夹爪 SN 未得到有效 16-char 文本：port=$port output=$(printf '%s' "$output" | tr '\n' ' ')"
        return 1
    fi

    printf '%s' "$sn"
    return 0
}

find_gripper_calibration_source() {
    local root="$1"
    local serial="$2"

    python3 - "$root" "$serial" <<'PY'
import pathlib
import sys

root = pathlib.Path(sys.argv[1])
serial = sys.argv[2].lower()
candidates = []

for path in sorted(root.rglob("*")):
    if path.is_dir():
        if path.name.lower() != serial:
            continue
        camchains = [child for child in path.iterdir() if child.is_file() and child.suffix.lower() in {".yaml", ".yml"} and "camchain" in child.name.lower()]
        imucam = path / "output-results-imucam.txt"
        if len(camchains) == 1 and imucam.is_file():
            candidates.append((2, str(path)))
        continue

    if not path.is_file():
        continue
    name = path.name.lower()
    if serial not in name:
        continue
    suffix = path.suffix.lower()
    if suffix == ".bin":
        score = 0
    elif suffix == ".md":
        score = 1
    else:
        continue

    if suffix == ".md" and "summary" not in name and "imucam" not in name:
        continue
    candidates.append((score, str(path)))

if not candidates:
    sys.exit(10)

best_score = min(score for score, _ in candidates)
best = [path for score, path in candidates if score == best_score]
if len(best) != 1:
    print("multiple candidates: " + ", ".join(best), file=sys.stderr)
    sys.exit(11)

print(best[0])
PY
}

resolve_source_camchain() {
    local source_path="$1"

    python3 - "$source_path" <<'PY'
import pathlib
import sys

source = pathlib.Path(sys.argv[1])

def find_camchain(directory: pathlib.Path) -> pathlib.Path | None:
    matches = sorted(
        path for path in directory.iterdir()
        if path.is_file() and path.suffix.lower() in {".yaml", ".yml"} and "camchain" in path.name.lower()
    )
    if len(matches) == 1:
        return matches[0]
    preferred = [path for path in matches if path.name.lower() == "rgb_video_ros-camchain.yaml"]
    if len(preferred) == 1:
        return preferred[0]
    return None

if source.is_dir():
    resolved = find_camchain(source)
elif source.is_file() and "camchain" in source.name.lower():
    resolved = source
else:
    resolved = find_camchain(source.parent)

if resolved is None:
    sys.exit(20)

print(resolved)
PY
}

resolve_source_imucam() {
    local source_path="$1"

    python3 - "$source_path" <<'PY'
import pathlib
import sys

source = pathlib.Path(sys.argv[1])

def find_imucam(directory: pathlib.Path) -> pathlib.Path | None:
    matches = sorted(
        path for path in directory.iterdir()
        if path.is_file() and path.name.lower() == "output-results-imucam.txt"
    )
    if len(matches) == 1:
        return matches[0]
    return None

if source.is_dir():
    resolved = find_imucam(source)
elif source.is_file() and source.name.lower() == "output-results-imucam.txt":
    resolved = source
else:
    resolved = find_imucam(source.parent)

if resolved is None:
    sys.exit(21)

print(resolved)
PY
}

prepare_gripper_payload_bin() {
    local source_path="$1"
    local output_bin="$2"

    if [ -d "$source_path" ]; then
        if [ ! -f "$GRIPPER_CALIB_GENERATOR" ]; then
            log "夹爪 payload 生成脚本不存在：$GRIPPER_CALIB_GENERATOR"
            return 1
        fi
        python3 "$GRIPPER_CALIB_GENERATOR" --input "$source_path" --output-bin "$output_bin"
        return $?
    fi

    case "${source_path##*.}" in
        bin|BIN)
            local size=""
            size="$(stat -c '%s' "$source_path")"
            if [ "$size" != "1024" ]; then
                log "夹爪 payload 二进制大小非法：$source_path size=$size expected=1024"
                return 1
            fi
            cp -f "$source_path" "$output_bin"
            return 0
            ;;
        md|MD)
            if [ ! -f "$GRIPPER_CALIB_GENERATOR" ]; then
                log "夹爪 payload 生成脚本不存在：$GRIPPER_CALIB_GENERATOR"
                return 1
            fi
            python3 "$GRIPPER_CALIB_GENERATOR" --input "$source_path" --output-bin "$output_bin"
            return $?
            ;;
    esac

    log "不支持的夹爪标定源文件类型：$source_path"
    return 1
}

write_gripper_calibration() {
    local port="$1"
    local payload_bin="$2"
    local output=""

    if ! output="$(run_hmi_helper --port "$port" --write-calib-bin "$payload_bin" 2>&1)"; then
        log "写入夹爪标定失败：port=$port payload=$payload_bin output=$(printf '%s' "$output" | tr '\n' ' ')"
        return 1
    fi

    log "写入夹爪标定成功：port=$port payload=$payload_bin"
    return 0
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

SYSTEM_SN_DIR="$CALIB_ROOT/$DEVICE_SN"
if [ ! -d "$SYSTEM_SN_DIR" ]; then
    SYSTEM_SN_DIR=""
    while IFS= read -r dir; do
        base="$(basename "$dir")"
        if [ "${base,,}" = "${DEVICE_SN,,}" ]; then
            SYSTEM_SN_DIR="$dir"
            break
        fi
    done < <(find "$CALIB_ROOT" -mindepth 1 -maxdepth 1 -type d | sort)
fi

SEARCH_ROOT="$CALIB_ROOT"
if [ -n "$SYSTEM_SN_DIR" ] && [ -d "$SYSTEM_SN_DIR" ]; then
    SEARCH_ROOT="$SYSTEM_SN_DIR"
    log "Using DEVICE_SN-specific calibration root: $SEARCH_ROOT"
else
    log "No DEVICE_SN-specific calibration root for DEVICE_SN=$DEVICE_SN, fallback to recursive gripper SN matching under $CALIB_ROOT"
fi

declare -A SIDE_CAMCHAIN_FILES=()
while IFS= read -r file; do
    side="$(detect_camera_side "$file" || true)"
    if [ -z "$side" ]; then
        log "Ignoring camchain without _left/_right suffix: $(basename "$file")"
        continue
    fi

    if [ -n "${SIDE_CAMCHAIN_FILES[$side]:-}" ]; then
        log "Multiple camchain files found for side=$side: $(basename "${SIDE_CAMCHAIN_FILES[$side]}"), $(basename "$file")"
        exit 1
    fi

    SIDE_CAMCHAIN_FILES[$side]="$file"
done < <(find "$SEARCH_ROOT" -maxdepth 2 -type f \( -name '*camchain*.yaml' -o -name '*camchain*.yml' \) | sort)

for side in left right; do
    if [ -n "${SIDE_CAMCHAIN_FILES[$side]:-}" ]; then
        log "Detected ${side} camchain: $(basename "${SIDE_CAMCHAIN_FILES[$side]}")"
    fi
done

PERSIST_CALIB_DIR="${CALIB_PERSIST_DIR_OVERRIDE:-/etc/ugripper/config/calibration}"
PERSIST_CALIB_FILE="$PERSIST_CALIB_DIR/calibration.json"
FALLBACK_CAM_JSON="$PROJECT_ROOT/config/fakeCamCalib.json"

if [ ! -f "$PERSIST_CALIB_FILE" ] && [ ! -f "$FALLBACK_CAM_JSON" ]; then
    log "Base calibration.json missing (persist + fallback)"
    exit 1
fi

mkdir -p "$PERSIST_CALIB_DIR"
TMP_STAGE_DIR="$(mktemp -d "$PERSIST_CALIB_DIR/.import_stage.XXXXXX")"
# HMI helper may run as ubuntu via runuser, so the staging directory must be traversable.
chmod 755 "$TMP_STAGE_DIR"
cleanup_stage_dir() {
    rm -rf "$TMP_STAGE_DIR"
}
trap cleanup_stage_dir EXIT
TEMP_CALIB_JSON="$TMP_STAGE_DIR/calibration.json"

LEFT_GRIPPER_PORT="${LEFT_GRIPPER_PORT_OVERRIDE:-/dev/left_gripper}"
RIGHT_GRIPPER_PORT="${RIGHT_GRIPPER_PORT_OVERRIDE:-/dev/right_gripper}"
mapfile -t IMPORT_SIDES < <(resolve_import_sides)

declare -A SIDE_GRIPPER_PORTS=(
    [left]="$LEFT_GRIPPER_PORT"
    [right]="$RIGHT_GRIPPER_PORT"
)
declare -A SIDE_TARGETED=()
declare -A SIDE_GRIPPER_SN=()
declare -A SIDE_GRIPPER_SOURCE=()
declare -A SIDE_GRIPPER_BIN=()
declare -A SIDE_IMUCAM_FILES=()

for side in "${IMPORT_SIDES[@]}"; do
    port="${SIDE_GRIPPER_PORTS[$side]}"
    gripper_sn="$(read_gripper_sn "$port")" || exit 1
    SIDE_GRIPPER_SN[$side]="$gripper_sn"

    if source_path="$(find_gripper_calibration_source "$SEARCH_ROOT" "$gripper_sn" 2>/tmp/gripper_find_${side}.log)"; then
        SIDE_GRIPPER_SOURCE[$side]="$source_path"
        SIDE_TARGETED[$side]=1
        log "Matched gripper calibration source: side=$side port=$port gripper_sn=$gripper_sn source=$source_path"
    else
        rc=$?
        if [ "$rc" -ne 10 ]; then
            log "匹配夹爪标定文件失败：side=$side port=$port gripper_sn=$gripper_sn rc=$rc err=$(tr '\n' ' ' < /tmp/gripper_find_${side}.log)"
            rm -f /tmp/gripper_find_${side}.log
            exit 1
        fi
        log "No gripper calibration matched for side=$side port=$port gripper_sn=$gripper_sn"
    fi
    rm -f /tmp/gripper_find_${side}.log

    if [ -n "${SIDE_CAMCHAIN_FILES[$side]:-}" ]; then
        SIDE_TARGETED[$side]=1
    fi
done

for side in "${IMPORT_SIDES[@]}"; do
    if [ -z "${SIDE_TARGETED[$side]:-}" ]; then
        continue
    fi

    if [ -z "${SIDE_GRIPPER_SOURCE[$side]:-}" ]; then
        log "目标侧缺少匹配的夹爪标定源，拒绝导入：side=$side gripper_sn=${SIDE_GRIPPER_SN[$side]}"
        exit 1
    fi

    if [ -z "${SIDE_CAMCHAIN_FILES[$side]:-}" ]; then
        if ! SIDE_CAMCHAIN_FILES[$side]="$(resolve_source_camchain "${SIDE_GRIPPER_SOURCE[$side]}")"; then
            log "无法从夹爪标定源解析主相机 camchain：side=$side source=${SIDE_GRIPPER_SOURCE[$side]}"
            exit 1
        fi
        log "Resolved ${side} camchain from matched gripper source: $(basename "${SIDE_CAMCHAIN_FILES[$side]}")"
    fi

    if ! SIDE_IMUCAM_FILES[$side]="$(resolve_source_imucam "${SIDE_GRIPPER_SOURCE[$side]}")"; then
        log "无法从夹爪标定源解析 imucam：side=$side source=${SIDE_GRIPPER_SOURCE[$side]}"
        exit 1
    fi
    log "Resolved ${side} imucam from matched gripper source: $(basename "${SIDE_IMUCAM_FILES[$side]}")"
done

if [ "${#SIDE_TARGETED[@]}" -eq 0 ]; then
    log "No calibration source matched any current gripper SN under $SEARCH_ROOT"
    exit 10
fi

IMPORT_SIDE_ARGS=()
for side in "${IMPORT_SIDES[@]}"; do
    if [ -n "${SIDE_TARGETED[$side]:-}" ] && [ -n "${SIDE_CAMCHAIN_FILES[$side]:-}" ] && [ -n "${SIDE_IMUCAM_FILES[$side]:-}" ]; then
        IMPORT_SIDE_ARGS+=("camchain:${side}=${SIDE_CAMCHAIN_FILES[$side]}")
        IMPORT_SIDE_ARGS+=("imucam:${side}=${SIDE_IMUCAM_FILES[$side]}")
    fi
done

python3 - "$PERSIST_CALIB_FILE" "$FALLBACK_CAM_JSON" "$TEMP_CALIB_JSON" "$DEVICE_SN" "${IMPORT_SIDE_ARGS[@]}" <<'PY'
import json
import pathlib
import re
import sys
from datetime import datetime, timezone

base_json, fallback_json, output_json, device_sn, *side_args = sys.argv[1:]
FLOAT_RE = r"[-+]?(?:\d+\.\d*|\.\d+|\d+)(?:[eE][-+]?\d+)?"
PAYLOAD_FORMAT_VERSION = "0x00010000"

TARGET_KEYS = {
    "left": "observation.images.left_cam_main",
    "right": "observation.images.right_cam_main",
}

STEREO_IMAGE_KEYS = {
    "left": "observation.images.left_stereo",
    "right": "observation.images.right_stereo",
}

IMU_KEYS = {
    "left": "observation.imu.left_imu",
    "right": "observation.imu.right_imu",
}

LEGACY_KEYS = {
    "left": [
        "observation.images.left_cam_main",
        "observation.images.cam_left",
        "observation.images.left_main",
        "observation.images.{{CAM_MAIN}}",
    ],
    "right": [
        "observation.images.right_cam_main",
        "observation.images.cam_right",
        "observation.images.right_main",
        "observation.images.{{CAM_MAIN}}",
    ],
}


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


def parse_named_block(text, block_name):
    m = re.search(
        rf"^\s*{re.escape(block_name)}:\s*$([\s\S]*?)(?=^\S|\Z)",
        text,
        re.M,
    )
    return m.group(1) if m else None


def parse_camchain(path):
    text = load_text(path)
    block = parse_named_block(text, "cam0") or text
    intr = parse_line_list(block, "intrinsics")
    dist = parse_line_list(block, "distortion_coeffs") or []
    res = parse_line_list(block, "resolution")

    if not intr or len(intr) < 4 or not res or len(res) < 2:
        raise ValueError("camchain missing intrinsics/resolution")

    return {
        "camera_model": parse_line_scalar(block, "camera_model") or "pinhole",
        "distortion_model": parse_line_scalar(block, "distortion_model") or "equidistant",
        "intrinsics": intr[:4],
        "distortion_coeffs": dist,
        "resolution": [int(round(res[0])), int(round(res[1]))],
        "rostopic": parse_line_scalar(block, "rostopic"),
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


def parse_prefixed_side_args(arguments, prefix):
    parsed = {}
    for item in arguments:
        if not item.startswith(prefix):
            continue
        payload = item[len(prefix):]
        if "=" not in payload:
            raise ValueError(f"invalid {prefix} argument: {item}")
        side, path = payload.split("=", 1)
        if side not in TARGET_KEYS:
            raise ValueError(f"unsupported side: {side}")
        parsed[side] = path
    return parsed


def pop_existing_image_entry(data, side):
    for key in LEGACY_KEYS[side]:
        value = data.get(key)
        if isinstance(value, dict):
            if key != TARGET_KEYS[side]:
                data.pop(key, None)
            return value
    return None


def parse_matrix_after(text, marker):
    idx = text.find(marker)
    if idx < 0:
        raise ValueError(f"missing matrix marker: {marker}")

    rows = []
    for line in text[idx:].splitlines()[1:]:
        nums = re.findall(FLOAT_RE, line)
        if nums:
            rows.append([float(x) for x in nums])
            if len(rows) == 4:
                break
        elif rows:
            break

    if len(rows) != 4 or any(len(row) != 4 for row in rows):
        raise ValueError(f"invalid matrix block after marker: {marker}")
    return rows


def find_float_after(text, pattern):
    match = re.search(pattern, text, re.S)
    if not match:
        raise ValueError(f"missing float pattern: {pattern}")
    return float(match.group(1))


def camera_model_enum_from_strings(camera_model, distortion_model):
    camera_model = (camera_model or "").strip().lower()
    distortion_model = (distortion_model or "").strip().lower()
    if camera_model == "pinhole" and distortion_model == "equidistant":
        return 1
    return 1


def parse_statistics_triplet(text, pattern):
    match = re.search(pattern, text, re.M)
    if not match:
        raise ValueError(f"missing residual pattern: {pattern}")
    return {
        "mean": float(match.group(1)),
        "median": float(match.group(2)),
        "std": float(match.group(3)),
    }


def parse_cam_cfg(text, label):
    match = re.search(
        rf"^\s*{re.escape(label)}\s*$\n^\s*-+\s*$\n(.*?)(?=^\s*cam\d\s*$|^\s*IMU configuration\s*$|\Z)",
        text,
        re.M | re.S,
    )
    if not match:
        raise ValueError(f"missing config block for {label}")

    block = match.group(1)

    def list_field(name):
        mm = re.search(rf"{re.escape(name)}:\s*\[([^\]]+)\]", block)
        if not mm:
            return None
        return parse_num_list(mm.group(1))

    def scalar_field(name):
        mm = re.search(rf"{re.escape(name)}:\s*(.+?)\s*$", block, re.M)
        return mm.group(1).strip() if mm else None

    focal_length = list_field("Focal length")
    principal_point = list_field("Principal point")
    distortion_coefficients = list_field("Distortion coefficients") or []
    if not focal_length or len(focal_length) < 2 or not principal_point or len(principal_point) < 2:
        raise ValueError(f"missing intrinsics in {label}")

    camera_model = scalar_field("Camera model") or "pinhole"
    distortion_model = scalar_field("Distortion model") or "equidistant"
    return {
        "camera_model_enum": camera_model_enum_from_strings(camera_model, distortion_model),
        "camera_model": camera_model,
        "distortion_model": distortion_model,
        "focal_length": focal_length[:2],
        "principal_point": principal_point[:2],
        "distortion_coefficients": distortion_coefficients,
    }


def parse_imu_cfg(text):
    match = re.search(r"^\s*IMU0:\s*$\n^\s*-+\s*$\n(.*?)(?=\Z)", text, re.M | re.S)
    if not match:
        raise ValueError("missing IMU0 block")

    block = match.group(1)

    def pick(pattern):
        mm = re.search(pattern, block, re.M | re.S)
        if not mm:
            raise ValueError(f"missing IMU field: {pattern}")
        return float(mm.group(1))

    model_match = re.search(r"^\s*Model:\s*(.+?)\s*$", block, re.M)
    return {
        "model": model_match.group(1).strip() if model_match else "calibrated",
        "update_rate_hz": pick(rf"Update rate:\s*({FLOAT_RE})"),
        "accelerometer": {
            "noise_density_discrete": pick(rf"Accelerometer:.*?Noise density \(discrete\):\s*({FLOAT_RE})"),
            "random_walk": pick(rf"Accelerometer:.*?Random walk:\s*({FLOAT_RE})"),
        },
        "gyroscope": {
            "noise_density_discrete": pick(rf"Gyroscope:.*?Noise density \(discrete\):\s*({FLOAT_RE})"),
            "random_walk": pick(rf"Gyroscope:.*?Random walk:\s*({FLOAT_RE})"),
        },
    }


def parse_imucam(path):
    text = load_text(path)
    cam0 = parse_cam_cfg(text, "cam0")
    cam1 = parse_cam_cfg(text, "cam1")
    return {
        "calibration_data_format_version": PAYLOAD_FORMAT_VERSION,
        "cam0": cam0,
        "cam1": cam1,
        "extrinsics": {
            "T_ic_cam0_to_imu0": parse_matrix_after(text, "T_ic:  (cam0 to imu0):"),
            "timeshift_cam0_to_imu0": find_float_after(text, rf"timeshift cam0 to imu0:.*?\n\s*({FLOAT_RE})"),
            "T_ic_cam1_to_imu0": parse_matrix_after(text, "T_ic:  (cam1 to imu0):"),
            "timeshift_cam1_to_imu0": find_float_after(text, rf"timeshift cam1 to imu0:.*?\n\s*({FLOAT_RE})"),
            "baseline_norm": find_float_after(text, rf"baseline norm:\s*({FLOAT_RE})"),
        },
        "imu0": parse_imu_cfg(text),
        "residuals": {
            "reprojection_error_cam0_px": parse_statistics_triplet(
                text,
                rf"Reprojection error \(cam0\) \[px\]:\s*mean\s*({FLOAT_RE}),\s*median\s*({FLOAT_RE}),\s*std:\s*({FLOAT_RE})",
            ),
            "reprojection_error_cam1_px": parse_statistics_triplet(
                text,
                rf"Reprojection error \(cam1\) \[px\]:\s*mean\s*({FLOAT_RE}),\s*median\s*({FLOAT_RE}),\s*std:\s*({FLOAT_RE})",
            ),
            "gyroscope_error_imu0_rad_s": parse_statistics_triplet(
                text,
                rf"Gyroscope error \(imu0\) \[rad/s\]:\s*mean\s*({FLOAT_RE}),\s*median\s*({FLOAT_RE}),\s*std:\s*({FLOAT_RE})",
            ),
            "accelerometer_error_imu0_m_s2": parse_statistics_triplet(
                text,
                rf"Accelerometer error \(imu0\) \[m/s\^2\]:\s*mean\s*({FLOAT_RE}),\s*median\s*({FLOAT_RE}),\s*std:\s*({FLOAT_RE})",
            ),
        },
    }


def make_intrinsics_entry(width, height, fx, fy, cx, cy):
    return {
        f"{width}x{height}": {
            "fx": fx,
            "fy": fy,
            "ppx": cx,
            "ppy": cy,
        }
    }


def ensure_stereo_image_entry(side, stereo_payload):
    cam0 = stereo_payload["cam0"]
    cam1 = stereo_payload["cam1"]
    fx0, fy0 = cam0["focal_length"]
    cx0, cy0 = cam0["principal_point"]
    fx1, fy1 = cam1["focal_length"]
    cx1, cy1 = cam1["principal_point"]
    return {
        "shape": [400, 1280, 3],
        "names": ["height", "width", "channels"],
        "info": None,
        "intrinsics": {
            "cam0_640x400": {
                "fx": fx0,
                "fy": fy0,
                "ppx": cx0,
                "ppy": cy0,
            },
            "cam1_640x400": {
                "fx": fx1,
                "fy": fy1,
                "ppx": cx1,
                "ppy": cy1,
            },
        },
        "camera_model": cam0["camera_model"],
        "distortion_model": cam0["distortion_model"],
        "distortion_coeffs": cam0["distortion_coefficients"],
        "dtype": "video",
        "fps": 60,
        "side": side,
        "stereo": stereo_payload,
    }


def ensure_imu_entry(stereo_payload):
    imu = stereo_payload["imu0"]
    return {
        "dtype": "imu",
        "model": imu["model"],
        "update_rate_hz": imu["update_rate_hz"],
        "acc_noise_density_discrete": imu["accelerometer"]["noise_density_discrete"],
        "acc_random_walk": imu["accelerometer"]["random_walk"],
        "gyro_noise_density_discrete": imu["gyroscope"]["noise_density_discrete"],
        "gyro_random_walk": imu["gyroscope"]["random_walk"],
    }


src_json_path = pathlib.Path(base_json)
if src_json_path.exists():
    data = json.loads(src_json_path.read_text(encoding="utf-8"))
else:
    data = json.loads(pathlib.Path(fallback_json).read_text(encoding="utf-8"))

side_to_file = parse_prefixed_side_args(side_args, "camchain:")
side_to_imucam = parse_prefixed_side_args(side_args, "imucam:")
if not side_to_file:
    raise ValueError("no side-specific camchain files provided")

if set(side_to_file.keys()) != set(side_to_imucam.keys()):
    raise ValueError(
        "camchain/imucam side mismatch: "
        f"camchain={sorted(side_to_file.keys())} imucam={sorted(side_to_imucam.keys())}"
    )

main_cameras = {}
stereo_imu_bundles = {}
existing_calibration_info = data.get("calibration_info")
if not isinstance(existing_calibration_info, dict):
    existing_calibration_info = {}

existing_main_cameras = existing_calibration_info.get("main_cameras")
if isinstance(existing_main_cameras, dict):
    main_cameras.update(existing_main_cameras)

existing_stereo_imu_bundles = existing_calibration_info.get("stereo_imu_bundles")
if isinstance(existing_stereo_imu_bundles, dict):
    stereo_imu_bundles.update(existing_stereo_imu_bundles)

for side, camchain_file in sorted(side_to_file.items()):
    camchain = parse_camchain(camchain_file)
    stereo_payload = parse_imucam(side_to_imucam[side])
    width, height = camchain["resolution"]
    fx, fy, cx, cy = camchain["intrinsics"]
    target_key = TARGET_KEYS[side]
    existing_entry = pop_existing_image_entry(data, side)
    data[target_key] = ensure_image_entry(
        existing_entry,
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
    data[target_key].pop("rostopic", None)
    if camchain.get("rostopic"):
        data[target_key]["rostopic"] = camchain["rostopic"]

    main_cameras[side] = {
        "source": pathlib.Path(camchain_file).name,
        "camera_model": camchain["camera_model"],
        "distortion_model": camchain["distortion_model"],
        "resolution": camchain["resolution"],
    }

    data[STEREO_IMAGE_KEYS[side]] = ensure_stereo_image_entry(side, stereo_payload)
    data[IMU_KEYS[side]] = ensure_imu_entry(stereo_payload)
    stereo_imu_bundles[side] = {
        "source": pathlib.Path(side_to_imucam[side]).name,
        "calibration_data_format_version": stereo_payload["calibration_data_format_version"],
        "cam0": stereo_payload["cam0"],
        "cam1": stereo_payload["cam1"],
        "extrinsics": stereo_payload["extrinsics"],
        "imu0": stereo_payload["imu0"],
        "residuals": stereo_payload["residuals"],
    }

data.pop("observation.images.{{CAM_MAIN}}", None)
data.pop("observation.images.fays_cam0", None)
data.pop("observation.images.fays_cam1", None)
data.pop("observation.imu.fays_imu0", None)

metadata = data.get("metadata", {}) if isinstance(data.get("metadata"), dict) else {}
metadata["format_version"] = metadata.get("format_version", "1.0")
metadata["generation_date"] = datetime.now(timezone.utc).strftime("%Y-%m-%d")
metadata["description"] = "Imported calibration parameters"
metadata["calibration_status"] = "calibrated"
data["metadata"] = metadata

calib_info = data.get("calibration_info", {}) if isinstance(data.get("calibration_info"), dict) else {}
calib_info["calibration_date"] = datetime.now(timezone.utc).strftime("%Y-%m-%d")
calib_info["calibration_status"] = "calibrated"
calib_info["device_sn"] = device_sn
calib_info.pop("main_camera", None)
calib_info["main_cameras"] = main_cameras
calib_info.pop("fays_imu_bundle", None)
calib_info["stereo_imu_bundles"] = stereo_imu_bundles
calib_info["imported_sides"] = sorted(side_to_file.keys())
calib_info["notes"] = (
    "Auto-imported from USB by device SN after gripper SN matching. "
    "Host calibration.json now keeps per-side main camera, stereo, and board IMU payload fields aligned with the gripper calibration payload."
)
data["calibration_info"] = calib_info

out_path = pathlib.Path(output_json)
out_path.parent.mkdir(parents=True, exist_ok=True)
out_path.write_text(json.dumps(data, indent=4, ensure_ascii=False) + "\n", encoding="utf-8")
PY

for side in "${IMPORT_SIDES[@]}"; do
    if [ -z "${SIDE_TARGETED[$side]:-}" ]; then
        continue
    fi

    port="${SIDE_GRIPPER_PORTS[$side]}"
    gripper_sn="${SIDE_GRIPPER_SN[$side]}"
    source_path="${SIDE_GRIPPER_SOURCE[$side]}"
    payload_bin="$TMP_STAGE_DIR/gripper_${side}_${gripper_sn}.bin"
    if ! prepare_gripper_payload_bin "$source_path" "$payload_bin"; then
        log "生成夹爪 payload 失败：side=$side source=$source_path"
        exit 1
    fi
    chmod 644 "$payload_bin"

    SIDE_GRIPPER_SN[$side]="$gripper_sn"
    SIDE_GRIPPER_SOURCE[$side]="$source_path"
    SIDE_GRIPPER_BIN[$side]="$payload_bin"
    log "夹爪标定匹配成功：side=$side port=$port gripper_sn=$gripper_sn source=$(basename "$source_path")"
done

for side in "${IMPORT_SIDES[@]}"; do
    if [ -z "${SIDE_GRIPPER_BIN[$side]:-}" ]; then
        continue
    fi

    port="${SIDE_GRIPPER_PORTS[$side]}"
    if ! write_gripper_calibration "$port" "${SIDE_GRIPPER_BIN[$side]}"; then
        exit 1
    fi
done

mv -f "$TEMP_CALIB_JSON" "$PERSIST_CALIB_FILE"
if id "$HMI_RUN_USER" >/dev/null 2>&1; then
    chown "$HMI_RUN_USER":"$HMI_RUN_USER" "$PERSIST_CALIB_DIR" "$PERSIST_CALIB_FILE" >/dev/null 2>&1 || true
    chmod 775 "$PERSIST_CALIB_DIR" >/dev/null 2>&1 || true
    chmod 664 "$PERSIST_CALIB_FILE" >/dev/null 2>&1 || true
fi

IMPORT_STAMP="$(date +%Y%m%d_%H%M%S)"
IMPORT_SAVE_DIR="$PERSIST_CALIB_DIR/imported/${DEVICE_SN}/${IMPORT_STAMP}"
mkdir -p "$IMPORT_SAVE_DIR"
cat > "$IMPORT_SAVE_DIR/import_meta.txt" <<META
import_time_utc=$(date -u '+%Y-%m-%dT%H:%M:%SZ')
device_sn=$DEVICE_SN
usb_source_dir=$SEARCH_ROOT
imported_sides=$(printf '%s\n' "${!SIDE_TARGETED[@]}" | sort | paste -sd, -)
output_calibration_json=$PERSIST_CALIB_FILE
META

for side in "${IMPORT_SIDES[@]}"; do
    if [ -n "${SIDE_CAMCHAIN_FILES[$side]:-}" ]; then
        cp -f "${SIDE_CAMCHAIN_FILES[$side]}" "$IMPORT_SAVE_DIR/"
        printf 'camchain_%s_file=%s\n' "$side" "$(basename "${SIDE_CAMCHAIN_FILES[$side]}")" >> "$IMPORT_SAVE_DIR/import_meta.txt"
    fi
    if [ -n "${SIDE_TARGETED[$side]:-}" ] && [ -n "${SIDE_GRIPPER_BIN[$side]:-}" ]; then
        cp -f "${SIDE_GRIPPER_BIN[$side]}" "$IMPORT_SAVE_DIR/"
        printf 'gripper_%s_port=%s\n' "$side" "${SIDE_GRIPPER_PORTS[$side]}" >> "$IMPORT_SAVE_DIR/import_meta.txt"
        printf 'gripper_%s_sn=%s\n' "$side" "${SIDE_GRIPPER_SN[$side]}" >> "$IMPORT_SAVE_DIR/import_meta.txt"
        printf 'gripper_%s_calibration_source=%s\n' "$side" "${SIDE_GRIPPER_SOURCE[$side]}" >> "$IMPORT_SAVE_DIR/import_meta.txt"
        printf 'gripper_%s_payload_bin=%s\n' "$side" "$(basename "${SIDE_GRIPPER_BIN[$side]}")" >> "$IMPORT_SAVE_DIR/import_meta.txt"
    fi
done

trap - EXIT
cleanup_stage_dir

log "Calibration import completed for DEVICE_SN=$DEVICE_SN, sides=$(printf '%s\n' "${!SIDE_TARGETED[@]}" | sort | paste -sd, -)"
log "Updated: $PERSIST_CALIB_FILE"
exit 0
