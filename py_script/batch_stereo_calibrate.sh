#!/usr/bin/env bash
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# ========================= Defaults =========================
DATA_DIR="/home/junquan/dm_test/calib/two_camera_calib_data"
TARGET_FILE="aprilgrid_6x6_0.055m_0.3.yaml"
IMU_YAML_FILE="imu_fays.yaml"
MODELS="pinhole-equi pinhole-equi"
BAG_FREQ="20"
TOPICS="/fays/atrak/cam0 /fays/atrak/cam1"
KALIBR_SETUP="/home/rosuser/catkin_ws/devel/setup.bash"
CONVERTER="$SCRIPT_DIR/fays_imu_para_bag.py"
BAG_NAME="output.bag"
KALIBR_BIN="/home/rosuser/catkin_ws/devel/lib/kalibr"
# ============================================================

usage() {
    cat <<EOF
Usage: $(basename "$0") [OPTIONS]

Batch stereo + IMU calibration: convert mkv+mcap to rosbag, run kalibr
stereo camera calibration, then IMU-camera calibration, and clean up.

Options:
  --data-dir DIR      Root data directory         (default: $DATA_DIR)
  --target FILE       Target yaml filename        (default: $TARGET_FILE, relative to data-dir)
  --imu-yaml FILE     IMU params yaml filename    (default: $IMU_YAML_FILE, relative to data-dir)
  --models MODEL      Camera models for kalibr    (default: $MODELS)
  --bag-freq FREQ     Bag sampling frequency      (default: $BAG_FREQ)
  --topics TOPIC      Stereo image topics         (default: $TOPICS)
  --kalibr-setup PATH Path to kalibr setup.bash   (default: $KALIBR_SETUP)
  -h, --help          Show this help message
EOF
    exit 0
}

# ========================= Argument parsing =========================
while [[ $# -gt 0 ]]; do
    case "$1" in
        --data-dir)     DATA_DIR="$2";       shift 2 ;;
        --target)       TARGET_FILE="$2";    shift 2 ;;
        --imu-yaml)     IMU_YAML_FILE="$2";  shift 2 ;;
        --models)       MODELS="$2";         shift 2 ;;
        --bag-freq)     BAG_FREQ="$2";       shift 2 ;;
        --topics)       TOPICS="$2";         shift 2 ;;
        --kalibr-setup) KALIBR_SETUP="$2";   shift 2 ;;
        -h|--help)      usage ;;
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

# Resolve IMU yaml path (absolute or relative to DATA_DIR)
if [[ "$IMU_YAML_FILE" == /* ]]; then
    IMU_YAML="$IMU_YAML_FILE"
else
    IMU_YAML="$DATA_DIR/$IMU_YAML_FILE"
fi

# Camchain filename is derived from bag name: output.bag -> output-camchain.yaml
BAG_STEM="${BAG_NAME%.bag}"
CAMCHAIN_NAME="${BAG_STEM}-camchain.yaml"

# ========================= Validation =========================
if [[ ! -d "$DATA_DIR" ]]; then
    echo "[ERROR] Data directory does not exist: $DATA_DIR"
    exit 1
fi

if [[ ! -f "$TARGET" ]]; then
    echo "[ERROR] Target yaml does not exist: $TARGET"
    exit 1
fi

if [[ ! -f "$IMU_YAML" ]]; then
    echo "[ERROR] IMU yaml does not exist: $IMU_YAML"
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
echo " Batch Stereo + IMU Calibration"
echo "=========================================="
echo " Data dir   : $DATA_DIR"
echo " Target     : $TARGET"
echo " IMU yaml   : $IMU_YAML"
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
    [[ -f "$d/fays_stereo_output.mkv" && -f "$d/fays_data.mcap" ]] || continue
    device_list+=("$d")
done

if [[ ${#device_list[@]} -eq 0 ]]; then
    echo "[WARN] No device directories found (need fays_stereo_output.mkv + fays_data.mcap). Exiting."
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
    device_dir="${device_dir%/}"
    device_name="$(basename "$device_dir")"
    seq=$((idx + 1))
    bag_file="$device_dir/$BAG_NAME"

    echo "=========================================="
    echo " [$seq/$total] Processing: $device_name"
    echo "=========================================="

    # ---------- Step 1: mkv + mcap -> rosbag ----------
    echo "[$device_name] Step 1/4: Converting mkv+mcap to rosbag ..."
    if ! python3 "$CONVERTER" "$device_dir" "$bag_file"; then
        echo "[ERROR] [$device_name] Data conversion failed. Skipping."
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

    # ---------- Step 2: kalibr stereo camera calibration ----------
    echo "[$device_name] Step 2/4: Running kalibr stereo camera calibration ..."
    pushd "$device_dir" > /dev/null

    # $TOPICS and $MODELS are intentionally unquoted to allow word splitting
    # shellcheck disable=SC2086
    if ! "$KALIBR_BIN/kalibr_calibrate_cameras" \
            --bag "$bag_file" \
            --topics $TOPICS \
            --models $MODELS \
            --target "$TARGET" \
            --dont-show-report \
            --bag-freq "$BAG_FREQ"; then
        echo "[ERROR] [$device_name] Stereo camera calibration failed."
        popd > /dev/null
        rm -f "$bag_file"
        echo "[$device_name] Cleaned up rosbag after failure."
        fail_count=$((fail_count + 1))
        failed_devices+=("$device_name")
        continue
    fi

    echo "[$device_name] Stereo camera calibration completed."

    # Verify camchain output exists (needed for next step)
    if [[ ! -f "$device_dir/$CAMCHAIN_NAME" ]]; then
        echo "[ERROR] [$device_name] Camchain file not found: $CAMCHAIN_NAME"
        popd > /dev/null
        rm -f "$bag_file"
        fail_count=$((fail_count + 1))
        failed_devices+=("$device_name")
        continue
    fi

    # ---------- Step 3: kalibr IMU-camera calibration ----------
    echo "[$device_name] Step 3/4: Running kalibr IMU-camera calibration ..."

    if ! "$KALIBR_BIN/kalibr_calibrate_imu_camera" \
            --bag "$bag_file" \
            --cams "./$CAMCHAIN_NAME" \
            --imu "$IMU_YAML" \
            --target "$TARGET" \
            --dont-show-report; then
        echo "[ERROR] [$device_name] IMU-camera calibration failed."
        popd > /dev/null
        rm -f "$bag_file"
        echo "[$device_name] Cleaned up rosbag after failure."
        fail_count=$((fail_count + 1))
        failed_devices+=("$device_name")
        continue
    fi

    popd > /dev/null
    echo "[$device_name] IMU-camera calibration completed."

    # ---------- Step 4: Clean up rosbag ----------
    echo "[$device_name] Step 4/4: Removing rosbag to free disk space ..."
    rm -f "$bag_file"
    echo "[$device_name] Rosbag removed."

    success_count=$((success_count + 1))
    echo ""
done

# ========================= Summary =========================
echo "=========================================="
echo " Batch Stereo + IMU Calibration Summary"
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
