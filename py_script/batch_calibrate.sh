#!/usr/bin/env bash
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# ========================= Defaults =========================
DATA_DIR="/home/junquan/dm_test/calib/calib_data"
TARGET_FILE="aprilgrid_6x6_0.055m_0.3.yaml"
MODELS="pinhole-equi"
BAG_FREQ="20"
TOPICS="/camera/image_raw"
KALIBR_SETUP="/home/rosuser/catkin_ws/devel/setup.bash"
CONVERTER="$SCRIPT_DIR/rgb_mkv_to_rosbag_ffmpeg.py"
BAG_NAME="rgb_video_ros.bag"
# ============================================================

usage() {
    cat <<EOF
Usage: $(basename "$0") [OPTIONS]

Batch camera calibration: convert .mkv to rosbag, run kalibr, then clean up.

Options:
  --data-dir DIR      Root data directory         (default: $DATA_DIR)
  --target FILE       Target yaml filename        (default: $TARGET_FILE, relative to data-dir)
  --models MODEL      Camera model for kalibr     (default: $MODELS)
  --bag-freq FREQ     Bag sampling frequency      (default: $BAG_FREQ)
  --topics TOPIC      ROS image topic             (default: $TOPICS)
  --kalibr-setup PATH Path to kalibr setup.bash   (default: $KALIBR_SETUP)
  -h, --help          Show this help message
EOF
    exit 0
}

# ========================= Argument parsing =========================
while [[ $# -gt 0 ]]; do
    case "$1" in
        --data-dir)    DATA_DIR="$2";      shift 2 ;;
        --target)      TARGET_FILE="$2";   shift 2 ;;
        --models)      MODELS="$2";        shift 2 ;;
        --bag-freq)    BAG_FREQ="$2";      shift 2 ;;
        --topics)      TOPICS="$2";        shift 2 ;;
        --kalibr-setup) KALIBR_SETUP="$2"; shift 2 ;;
        -h|--help)     usage ;;
        *)
            echo "[ERROR] Unknown option: $1"
            usage
            ;;
    esac
done

# Resolve target path (absolute or relative to DATA_DIR)
if [[ "$TARGET_FILE" == /* ]]; then
    TARGET="$TARGET_FILE"
else
    TARGET="$DATA_DIR/$TARGET_FILE"
fi

# ========================= Validation =========================
if [[ ! -d "$DATA_DIR" ]]; then
    echo "[ERROR] Data directory does not exist: $DATA_DIR"
    exit 1
fi

if [[ ! -f "$TARGET" ]]; then
    echo "[ERROR] Target yaml does not exist: $TARGET"
    exit 1
fi

if [[ ! -f "$CONVERTER" ]]; then
    echo "[ERROR] Converter script not found: $CONVERTER"
    exit 1
fi

if [[ ! -f "$KALIBR_SETUP" ]]; then
    echo "[ERROR] Kalibr setup.bash not found: $KALIBR_SETUP"
    exit 1
fi

# ========================= Environment =========================
echo "=========================================="
echo " Batch Camera Calibration"
echo "=========================================="
echo " Data dir   : $DATA_DIR"
echo " Target     : $TARGET"
echo " Models     : $MODELS"
echo " Bag freq   : $BAG_FREQ"
echo " Topics     : $TOPICS"
echo " Converter  : $CONVERTER"
echo "=========================================="

# shellcheck disable=SC1090
source "$KALIBR_SETUP"

# ========================= Discover devices =========================
device_list=()
for d in "$DATA_DIR"/*/; do
    [[ -f "$d/cam.mkv" && -f "$d/info.json" ]] || continue
    device_list+=("$d")
done

if [[ ${#device_list[@]} -eq 0 ]]; then
    echo "[WARN] No device directories found (need cam.mkv + info.json). Exiting."
    exit 0
fi

echo ""
echo "Found ${#device_list[@]} device(s) to process:"
for d in "${device_list[@]}"; do
    echo "  - $(basename "$d")"
done
echo ""

# ========================= Processing =========================
total=${#device_list[@]}
success_count=0
fail_count=0
failed_devices=()

for idx in "${!device_list[@]}"; do
    device_dir="${device_list[$idx]}"
    device_dir="${device_dir%/}"  # Remove trailing slash to avoid double slashes
    device_name="$(basename "$device_dir")"
    seq=$((idx + 1))
    bag_file="$device_dir/$BAG_NAME"

    echo "=========================================="
    echo " [$seq/$total] Processing: $device_name"
    echo "=========================================="

    # ---------- Step 1: mkv -> rosbag ----------
    echo "[$device_name] Step 1/3: Converting mkv to rosbag ..."
    if ! python3 "$CONVERTER" \
            --cam-mkv "$device_dir/cam.mkv" \
            --info-json "$device_dir/info.json" \
            --out-bag "$bag_file"; then
        echo "[ERROR] [$device_name] mkv-to-rosbag conversion failed. Skipping."
        fail_count=$((fail_count + 1))
        failed_devices+=("$device_name")
        continue
    fi

    if [[ ! -f "$bag_file" ]]; then
        echo "[ERROR] [$device_name] Rosbag not created. Skipping."
        fail_count=$((fail_count + 1))
        failed_devices+=("$device_name")
        continue
    fi
    echo "[$device_name] Rosbag created: $bag_file"

    # ---------- Step 2: kalibr calibration ----------
    echo "[$device_name] Step 2/3: Running kalibr calibration ..."
    pushd "$device_dir" > /dev/null

    if ! /home/rosuser/catkin_ws/devel/lib/kalibr/kalibr_calibrate_cameras \
            --bag "$bag_file" \
            --topics "$TOPICS" \
            --models "$MODELS" \
            --target "$TARGET" \
            --dont-show-report \
            --bag-freq "$BAG_FREQ"; then
        echo "[ERROR] [$device_name] Kalibr calibration failed."
        popd > /dev/null
        rm -f "$bag_file"
        echo "[$device_name] Cleaned up rosbag after failure."
        fail_count=$((fail_count + 1))
        failed_devices+=("$device_name")
        continue
    fi

    popd > /dev/null
    echo "[$device_name] Calibration completed successfully."

    # ---------- Step 3: Clean up rosbag ----------
    echo "[$device_name] Step 3/3: Removing rosbag to free disk space ..."
    rm -f "$bag_file"
    echo "[$device_name] Rosbag removed."

    success_count=$((success_count + 1))
    echo ""
done

# ========================= Summary =========================
echo "=========================================="
echo " Batch Calibration Summary"
echo "=========================================="
echo " Total devices : $total"
echo " Succeeded     : $success_count"
echo " Failed        : $fail_count"
if [[ ${#failed_devices[@]} -gt 0 ]]; then
    echo " Failed list   :"
    for name in "${failed_devices[@]}"; do
        echo "   - $name"
    done
fi
echo "=========================================="
