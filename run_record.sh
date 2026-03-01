#!/bin/bash

# ================= 脚本初始化 =================
script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$script_dir" || exit 1

# ================= 配置部分 =================
DISK_DIR="/mnt/data_disk"
ENV_FILE="/etc/environment"

get_env_value() {
    local key="$1"
    local line value

    [ -f "$ENV_FILE" ] || return 1
    line=$(grep -E "^${key}=" "$ENV_FILE" | head -n1 || true)
    [ -n "$line" ] || return 1

    value="${line#*=}"
    value="$(printf '%s' "$value" | tr -d '"' | xargs)"
    printf '%s' "$value"
}

DEVICE_SN="$(get_env_value "DEVICE_SN" || true)"
DEVICE_SN_LOWER="${DEVICE_SN,,}"

DATA_ROOT="/mnt/data_disk/${DEVICE_SN_LOWER:-noname_device}" 

# --- 网络配置 (双臂协同) ---
# 通过环境变量/etc/environment获取当前设备角色，默认为 Right (Master)
DEVICE_SIDE="$(get_env_value "DEVICE_SIDE" || true)"

CURRENT_SIDE=${DEVICE_SIDE:-Right}
CURRENT_SIDE_LOWER="${CURRENT_SIDE,,}"

IP_RIGHT="192.168.1.100"
IP_LEFT="192.168.1.101"
SYNC_PORT=12345

# --- LED 控制配置 ---
LED_SCRIPT="./led_manager.py"
LED_PIPE="/tmp/umi_led_pipe"

# --- 音频配置---
AUDIO_PLAY_SCRIPT="./audio/audio_play.py"
AUDIO_TEMP_DIR="/tmp/umi_audio"
AUDIO_PIPE="/tmp/umi_audio_pipe"

# --- 传感器录制配置 ---
SENSOR_RECORDER_BIN="./build/src/sensor_recorder/sensor_recorder"
FAYS_RECORD_SCRIPT="./build/faysSense_vi_kit/scripts/run_fays_record.sh"
CAMERA_CODEC="$(get_env_value "CAMERA_CODEC" || true)"
CAMERA_CODEC="${CAMERA_CODEC,,}"
CAMERA_CODEC="${CAMERA_CODEC:-h264}"   # h264 | h265
DURATION_GAP_THRESHOLD_SEC=5.0          # 时长误差阈值（秒）
VIDEO_SPAN_PROBE_TIMEOUT_SEC=1.2        # 视频跨度探测超时（秒）

# --- GPIO 配置 ---
PIN_BTN_UP="PIN_36"      # 上按键（原有）
PIN_BTN_DOWN="PIN_38"    # 下按键（新增）
BTN_ACTIVE_LEVEL=0       # 0表示按下
DEBOUNCE_MS=0.03         # 30ms

# 长按检测阈值（秒）
# 0.5s 在实操中容易把短按误判成长按，导致“偶发不触发录制”。
LONG_PRESS_THRESHOLD=0.8
DUAL_LONG_PRESS_THRESHOLD=4.0
SHUTDOWN_PROMPT_THRESHOLD=2.0
DUAL_CHORD_WINDOW=0.2
SHUTDOWN_REQUEST_FILE="/tmp/umi_shutdown_request"

DOWN_BUTTON_AVAILABLE=true
GPIO_UP_LINE=""
GPIO_DOWN_LINE=""


# ================= 全局变量 =================
IS_RECORDING=false
PID_CAM=""
PID_FAYS_DAEMON=""
PID_SENSOR=""
TARGET_DIR=""
LAST_EPISODE_DIR=""  # 记录上次录制的目录（用于post音频）
PRE_AUDIO_FILE=""    # 存储预录制音频文件路径
NEXT_RESET_SOURCE_EPISODE_DIR=""   # 下一次录制的复位来源目录（由下键短按设置）
CURRENT_RECORDING_IS_RESET=false    # 当前录制是否为复位录制
CURRENT_RECORDING_RESET_SOURCE_DIR="" # 当前复位录制的来源目录
CURRENT_RECORDING_EXPECT_FAYS=false   # 当前录制是否期望存在 Fays 数据
FAYS_ENABLED=false                    # 是否启用 Fays（启动可缺省，插入后可自动启用）
FAYS_USB_PRESENT=false                # 最近一次检测到的 Fays FTDI USB 存在状态
FAYS_RECORDING_ACTIVE=false           # 当前是否已向 Fays 下发 START
FAYS_LAST_MONITOR_TS=0                # Fays 热插拔监控节流时间戳（秒）
CLEANUP_RUNNING=false                 # 防止 cleanup trap 重入
LAST_MONITOR_ERROR_MSG=""             # 最近一次监控错误详情（用于去重日志）
FAYS_PRESENT_CACHE_FILE="/dev/shm/umi_fays_present"  # Fays 在位检测缓存（1=在位，0=不在位）
MONITOR_PID=""
RECORDING_LOCK_FILE="/tmp/umi_recording.lock" # 录制锁文件
# ERROR 灯效分级定义（数字越小越严重）
# ERROR_1: 数据完整性异常（存储/录制校验）
# ERROR_2: 关键硬件异常（相机/传感器/GPIO/Fays 断连）
# ERROR_3: 网络同步异常（对端不可达/PTP 同步超时）
# ERROR_4: 一般外设异常（非关键链路）
# ERROR_5: 运行时异常（进程超时/内部执行失败）
ERROR_DATA_INTEGRITY="ERROR_1"
ERROR_HW_ABNORMAL="ERROR_2"
ERROR_NET_SYNC="ERROR_3"
ERROR_OPTIONAL_PERIPHERAL="ERROR_4"
ERROR_RUNTIME="ERROR_5"

# 启动直接释放锁文件，防止断电导致残留
rm -f "$RECORDING_LOCK_FILE"
rm -f "$SHUTDOWN_REQUEST_FILE"
rm -f "$FAYS_PRESENT_CACHE_FILE"

# PTP状态参数
PTP_WAIT_START_TS=0
PTP_WAIT_TIMEOUT=60    # 最多等待 60 秒
PTP_OFFSET_THRESHOLD_NS=100000000  # 100ms
PTP_OFFSET_MAX_NS=1000000000  # 1000ms

# ================= 状态机与 LED 通信模块 =================

# 1. 创建命名管道 (如果不存在)
if [ ! -p "$LED_PIPE" ]; then
    mkfifo "$LED_PIPE"
fi

# 2. 启动 Python LED 管理器 (后台运行)
if [ -f "$LED_SCRIPT" ]; then
    echo "[INFO]:Starting LED Manager..."
    uv run "$LED_SCRIPT" &
    PID_LED_SCRIPT=$!
    # 给 Python 一点时间初始化
    sleep 0.2
else
    echo "[Warning]:LED script not found at $LED_SCRIPT"
fi

# 3. 定义发送状态的函数
# 可选状态:
# INIT (蓝), READY (绿呼吸), RECORDING (绿闪烁), EXIT (关)
# ERROR_1~ERROR_5（红色长短编码，含义见上方 ERROR 灯效分级定义）
# 兼容态 ERROR 仍可用，但建议统一使用 ERROR_1~ERROR_5
write_pipe_message() {
    local pipe_path=$1
    local message=$2
    local timeout_sec=${3:-0.15}

    if [ ! -p "$pipe_path" ]; then
        return 1
    fi

    timeout "$timeout_sec" bash -c 'printf "%s\n" "$1" > "$2"' _ "$message" "$pipe_path" 2>/dev/null
}

set_state() {
    local state=$1
    local attempts=3

    while [ "$attempts" -gt 0 ]; do
        if write_pipe_message "$LED_PIPE" "$state"; then
            return 0
        fi
        attempts=$((attempts - 1))
        sleep 0.03
    done

    return 1
}

set_error_state() {
    local state=$1

    case "$state" in
        ERROR_1|ERROR_2|ERROR_3|ERROR_4|ERROR_5)
            ;;
        *)
            state="$ERROR_RUNTIME"
            ;;
    esac

    set_state "$state"
}

# 音频状态通知函数
notify_audio() {
    local action=$1
    write_pipe_message "$AUDIO_PIPE" "$action" 0.10 || true
}

# 设置初始状态：初始化中
set_state "INIT"

# ================= 音频系统初始化 =================
amixer -c rockchipes8388 sset 'ALC Capture Function' Stereo
amixer -c rockchipes8388  sset 'ALC Capture Max PGA' 7

# 创建音频管道
if [ ! -p "$AUDIO_PIPE" ]; then
    mkfifo "$AUDIO_PIPE"
fi

# 启动音频播放管理器
if [ -f "$AUDIO_PLAY_SCRIPT" ]; then
    echo "[INFO]:Starting Audio Play Manager..."
    uv run "$AUDIO_PLAY_SCRIPT" \
        > /dev/null 2>&1 &
    PID_AUDIO_PLAY=$!
    sleep 0.2
else
    echo "[Warning]:Audio play script not found at $AUDIO_PLAY_SCRIPT"
fi

# 创建临时音频目录
mkdir -p "$AUDIO_TEMP_DIR"

# ================= GPIO 启动有效性检查 (仅 Right 需要) =================
if [ "$CURRENT_SIDE_LOWER" == "right" ]; then
    echo "[INFO]:Pre-check GPIO validity for Master (Right)..."

    gpio_error_reported=false
    last_gpio_error_msg=""

    while true; do
        GPIO_VALID=true
        gpio_error_msg=""

        GPIO_UP_LINE=$(gpiofind "$PIN_BTN_UP" 2>/dev/null || true)
        GPIO_DOWN_LINE=$(gpiofind "$PIN_BTN_DOWN" 2>/dev/null || true)

        if [ -z "$GPIO_UP_LINE" ]; then
            GPIO_VALID=false
            gpio_error_msg+=" missing $PIN_BTN_UP;"
        fi

        if [ -z "$GPIO_DOWN_LINE" ]; then
            GPIO_VALID=false
            gpio_error_msg+=" missing $PIN_BTN_DOWN;"
        fi

        if [ "$GPIO_VALID" = true ]; then
            if [ "$(gpioget $GPIO_UP_LINE)" -eq "$BTN_ACTIVE_LEVEL" ]; then
                GPIO_VALID=false
                gpio_error_msg+=" $PIN_BTN_UP active on startup;"
            fi

            if [ "$(gpioget $GPIO_DOWN_LINE)" -eq "$BTN_ACTIVE_LEVEL" ]; then
                GPIO_VALID=false
                gpio_error_msg+=" $PIN_BTN_DOWN active on startup;"
            fi
        fi

        if [ "$GPIO_VALID" = true ]; then
            if [ "$gpio_error_reported" = true ]; then
                echo "[INFO]:GPIO pre-check recovered. Continue startup."
                set_state "INIT"
            fi
            break
        fi

        if [ "$gpio_error_reported" = false ] || [ "$gpio_error_msg" != "$last_gpio_error_msg" ]; then
            echo "[ERROR]:GPIO pre-check failed:$gpio_error_msg"
            echo "[ERROR]:Hold before disk checks, retrying in 1s..."
            set_error_state "$ERROR_HW_ABNORMAL"
            notify_audio "error"
            gpio_error_reported=true
            last_gpio_error_msg="$gpio_error_msg"
        fi

        sleep 1
    done
fi

# ================= 业务配置检查 =================
# 检查硬盘挂载 (改为循环等待模式)
disk_error_reported=false

while ! mountpoint -q "$DISK_DIR"; do
    if [ "$disk_error_reported" = false ]; then
        echo "[ERROR]:$DISK_DIR is NOT mounted! Waiting for disk..."
        set_error_state "$ERROR_DATA_INTEGRITY"
        # notify_audio "error"  # 可选播放提示音
        disk_error_reported=true
    fi

    sleep 1
done
disk_error_reported=false

echo "[INFO]:Disk OK: $DISK_DIR is mounted."

# ================= 日志系统 =================
# -------------------------
# 日志路径
# -------------------------
LOG_DIR_LOCAL="/tmp"
LOG_DIR_DISK="$DISK_DIR/logs"
TODAY=$(date +%Y%m%d)

LOG_FILE_LOCAL="$LOG_DIR_LOCAL/umi_sys_${DEVICE_SN_LOWER}_${TODAY}.log"
LOG_FILE_DISK="$LOG_DIR_DISK/umi_sys_${DEVICE_SN_LOWER}_${TODAY}.log"
LOG_SYNC_POS_FILE="/tmp/umi_sys_${DEVICE_SN_LOWER}_${TODAY}.pos"
LOG_SYNC_LOCK_FILE="/tmp/umi_sys_${DEVICE_SN_LOWER}_${TODAY}.lock"

# -------------------------
# 清理旧日志（非今天的）
# -------------------------
cleanup_old_logs() {
    for dir in "$LOG_DIR_LOCAL" "$LOG_DIR_DISK"; do
        [ -d "$dir" ] || continue
        for file in "$dir"/umi_sys_"${DEVICE_SN_LOWER}"_*.log; do
            # 如果文件名中不包含今天日期，就删除
            if [[ "$file" != *"${TODAY}.log" ]]; then
                echo "[INFO]:Deleting old log: $file"
                rm -f "$file"
            fi
        done
    done
}

cleanup_old_logs

# -------------------------
# 重定向日志到本地临时文件，同时输出到屏幕
# -------------------------
echo "[INFO]:Logging locally to: $LOG_FILE_LOCAL"
exec > >(tee -a "$LOG_FILE_LOCAL") 2>&1

# 3. 定义增量同步函数
# 作用：仅同步 src 新增字节到 dest，不做整文件重写
sync_logs_once() {
    local src="$1"
    local dest="$2"
    local state_file="$3"
    local lock_file="$4"
    local last_pos=0
    local curr_size=0

    [ -f "$src" ] || return 0

    # 关键：检查挂载点是否存活，避免写入失效路径。
    if ! mountpoint -q "$DISK_DIR"; then
        return 1
    fi

    mkdir -p "$(dirname "$dest")" 2>/dev/null || return 1

    {
        flock -x 9

        if [ -f "$state_file" ]; then
            last_pos=$(cat "$state_file" 2>/dev/null || echo 0)
        fi
        curr_size=$(stat -c%s "$src" 2>/dev/null || echo 0)

        # 日志轮转或重建后，游标回退到 0
        if [ "$curr_size" -lt "$last_pos" ]; then
            last_pos=0
        fi

        [ "$curr_size" -gt "$last_pos" ] || exit 0

        # 仅追加新增内容（增量），避免整文件重复写入。
        tail -c +$((last_pos + 1)) "$src" >> "$dest" 2>/dev/null || exit 1
        printf "%s" "$curr_size" > "$state_file"
    } 9>"$lock_file"
}

# 4. 定义后台同步函数
# 作用：定期检查硬盘是否在，如果在，就把新增日志搬运过去
sync_logs_to_disk() {
    local src="$1"
    local dest="$2"
    local state_file="$3"
    local lock_file="$4"

    while true; do
        sync_logs_once "$src" "$dest" "$state_file" "$lock_file" || true
        # 每 5 秒同步一次，降低 IO 压力
        sleep 5
    done
}

# 5. 启动后台同步进程并记录 PID
sync_logs_to_disk "$LOG_FILE_LOCAL" "$LOG_FILE_DISK" "$LOG_SYNC_POS_FILE" "$LOG_SYNC_LOCK_FILE" &
PID_LOG_SYNC=$!


# 硬盘检查通过，恢复为初始化状态(蓝灯)，准备后续硬件检查
set_state "INIT"


# ================= 目录结构与元数据 =================
echo "[INFO]:Initializing Data Structure..."

DIR_RUNTIME="/tmp/umi_episode_runtime"
DIR_META="$DIR_RUNTIME/metadata"
DIR_CALIB="$DIR_RUNTIME/calibration"
DIR_DATA="$DATA_ROOT/data"

mkdir -p "$DIR_META"
mkdir -p "$DIR_CALIB"
mkdir -p "$DIR_DATA"

# 1. 生成 metadata
META_FILE="$DIR_META/metadata.json"
cat <<EOF > "$META_FILE"
{
    "device_type": "UMI",
    "device_model": "ugripper",
    "device_id": "${DEVICE_SN}",
    "collector": "default_user",
    "device_side": "${DEVICE_SIDE:-Right}",
    "data_path": "data/episode_{date:08d}_{episode_index:04d}"
}
EOF
echo "[INFO]:Metadata metadata.json prepared."

# 2. 拷贝 calibration 文件
# 从 ./config/fake*Calib.json 拷贝到 calibration/xxx.json
if [ -f "./config/fakeCamCalib.json" ]; then
    cp -f "./config/fakeCamCalib.json" "$DIR_CALIB/cam.json"
fi
if [ -f "./config/fakeEncoderCalib.json" ]; then
    cp -f "./config/fakeEncoderCalib.json" "$DIR_CALIB/encoder.json"
fi
if [ -f "./config/fakeIMUCalib.json" ]; then
    cp -f "./config/fakeIMUCalib.json" "$DIR_CALIB/imu.json"
fi
echo "[INFO]:Calibration files synced."

# ================= 全局占位符处理 (CAM_MAIN) =================
handle_global_placeholders() {
    local json_file="$DIR_CALIB/cam.json"
    
    if [ ! -f "$json_file" ]; then return; fi

    # 1. 初始化 {{CAM_MAIN}}
    # 根据环境变量 CURRENT_SIDE_LOWER (left/right) 决定 cam_left 或 cam_right
    if grep -q "{{CAM_MAIN}}" "$json_file"; then
        local cam_name=""
        if [[ "${CURRENT_SIDE_LOWER}" == "left" ]]; then
            cam_name="cam_left"
        elif [[ "${CURRENT_SIDE_LOWER}" == "right" ]]; then
            cam_name="cam_right"
        else
            echo "[WARNING]:CURRENT_SIDE='$CURRENT_SIDE_LOWER' is invalid. Skipping {{CAM_MAIN}} init."
        fi

        if [ -n "$cam_name" ]; then
            echo "[INFO]:Initializing {{CAM_MAIN}} key to $cam_name in $json_file..."
            # 直接使用 sed 替换 key 字符串
            sed -i "s/{{CAM_MAIN}}/$cam_name/g" "$json_file"
        fi
    fi
}

# 立即执行一次全局占位符处理
handle_global_placeholders

# ================= 硬件序列号校验与初始化 =================
check_tactile_hardware() {
    local dev_node=$1
    local name=$2
    local json_file="$DIR_CALIB/cam.json"
    
    echo "[INFO]:Checking $name ($dev_node)..."
    
    if [ ! -e "$dev_node" ]; then
        echo "[WARNING]:Device $dev_node not found!"
        return
    fi

    # 使用 udevadm 查找父级 USB 设备的 serial
    # 逻辑：查找SUBSYSTEMS=="usb" 且 DRIVERS=="usb" 下的 ATTRS{serial}
    # 注意：udevadm 输出是层级的，我们取第一个匹配到的 USB serial
    local usb_serial=$(udevadm info --attribute-walk --name="$dev_node" \
            | grep -Pzo '(?s)SUBSYSTEMS=="usb".*?DRIVERS=="usb".*?ATTRS{serial}=="(?!xhci-)[^"]+"' \
            | tr '\0' '\n' \
            | sed -n 's/.*ATTRS{serial}=="\([^"]*\)".*/\1/p' \
            | head -n 1)

    if [ -z "$usb_serial" ]; then
        echo "[WARNING]:Could not read USB serial for $dev_node"
        return
    fi

    # 判断左右 (根据 device node 名称)
    local side=""
    local side_upper=""
    if [[ "$dev_node" == *"left"* ]]; then
        side="left"
        side_upper="LEFT"
    elif [[ "$dev_node" == *"right"* ]]; then
        side="right"
        side_upper="RIGHT"
    fi
    
    if [ -z "$side" ]; then
        echo "[WARNING]:Could not determine side from device node $dev_node, skipping check."
        return
    fi
    
    # 构造占位符字符串，例如 {{TACTILE_LEFT_SERIAL}}
    local placeholder="{{TACTILE_${side_upper}_SERIAL}}"

    # =========================
    # 初始化模式：检测占位符
    # =========================
    if grep -q "$placeholder" "$json_file"; then
        echo "[INFO]:Found placeholder $placeholder. Initializing to $usb_serial..."

        # 直接字符串替换，占位符安全
        sed -i "s|$placeholder|$usb_serial|g" "$json_file"

        echo "[INFO]:Initialization complete."
        return
    fi

    # =========================
    # 校验模式：Verify
    # =========================
    local json_path=".\"observation.images.gripper_${side}_tactile\".serial"

    local serial_in_json
    serial_in_json=$(jq -r "$json_path // empty" "$json_file")

    if [ -z "$serial_in_json" ]; then
        echo "[WARNING]:Could not find serial at $json_path in $json_file."

    elif [ "$serial_in_json" != "$usb_serial" ]; then
        echo "[WARNING]:Serial mismatch for $side tactile camera!"
        echo "  - Configured (JSON): $serial_in_json"
        echo "  - Detected (HW)    : $usb_serial"
        echo "  - ACTION: Keeping existing configuration (Manual intervention required if hardware changed)."

    else
        echo "[INFO]:  - Serial match OK: $usb_serial"
    fi
}

# --- 逻辑分支 ---
# 执行校验
check_tactile_hardware "/dev/left_tcam" "Left Tactile"
check_tactile_hardware "/dev/right_tcam" "Right Tactile"

# ================= GPIO 初始化说明 =================
# Right 侧 GPIO 已在启动阶段完成有效性检查与初始化；Left 侧无需按键 GPIO。
if [ "$CURRENT_SIDE_LOWER" != "right" ]; then
    echo "[INFO]:GPIO initialization skipped for Slave (Left)."
fi

# ================= 函数定义 =================
# 函数：系统健康监测与状态管理
monitor_system_health() {
    local is_recording_now=false
    local has_error=false
    local error_msg=""
    local error_state=""
    local error_rank=99
    local error_key=""

    if [ -f "$RECORDING_LOCK_FILE" ]; then
        is_recording_now=true
    fi

    # ===============================
    # 1. 硬件 / 连接错误检测（最高优先级）
    # ===============================

    # 硬盘
    if ! mountpoint -q "$DISK_DIR"; then
        has_error=true
        error_msg+=" Disk not mounted;"
        error_state="$ERROR_DATA_INTEGRITY"
        error_rank=1
    fi

    # 相机
    if [ ! -e "/dev/right_tcam" ]; then
        has_error=true
        error_msg+=" Right Cam lost;"
        if [ "$error_rank" -gt 2 ]; then
            error_state="$ERROR_HW_ABNORMAL"
            error_rank=2
        fi
    fi
    if [ ! -e "/dev/left_tcam" ]; then
        has_error=true
        error_msg+=" Left Cam lost;"
        if [ "$error_rank" -gt 2 ]; then
            error_state="$ERROR_HW_ABNORMAL"
            error_rank=2
        fi
    fi

    # 对端连接
    if [ "$CURRENT_SIDE_LOWER" == "left" ]; then
        if ! ping -c 1 -W 1 "$IP_RIGHT" >/dev/null 2>&1; then
            has_error=true
            error_msg+=" Right device unreachable;"
            if [ "$error_rank" -gt 3 ]; then
                error_state="$ERROR_NET_SYNC"
                error_rank=3
            fi
        fi
    fi

    # Fays：作为必需设备，缺失即持续报错（服务保持运行）。
    # 注意这里读取共享缓存（/dev/shm/umi_fays_present），避免后台监控进程
    # 使用到主循环中的进程内变量，导致断连状态不可见。
    if ! is_fays_ftdi_present; then
        has_error=true
        error_msg+=" Fays camera lost;"
        if [ "$error_rank" -gt 2 ]; then
            error_state="$ERROR_HW_ABNORMAL"
            error_rank=2
        fi
    fi

    # ===============================
    # 2. 若存在 ERROR，立刻进入 ERROR
    # ===============================
    if [ "$has_error" = true ]; then
        [ -z "$error_state" ] && error_state="$ERROR_RUNTIME"
        error_key="${error_state}|${error_msg}"

        if [ "$SYSTEM_HEALTH_STATUS" != "ERROR" ] || [ "$LAST_MONITOR_ERROR_MSG" != "$error_key" ]; then
            echo "[ERROR]:[$(date)] MONITOR ERROR (${error_state}):$error_msg"
            set_error_state "$error_state"
            notify_audio "error"
        fi
        SYSTEM_HEALTH_STATUS="ERROR"
        LAST_MONITOR_ERROR_MSG="$error_key"
        return
    fi

    # 录制中仅做硬件错误检测，不切换到 READY/CALIB 状态。
    if [ "$is_recording_now" = true ]; then
        if [ "$SYSTEM_HEALTH_STATUS" = "ERROR" ]; then
            echo "[INFO]:[$(date)] MONITOR RECOVERED during recording."
            set_state "RECORDING"
        fi
        SYSTEM_HEALTH_STATUS="OK"
        LAST_MONITOR_ERROR_MSG=""
        return
    fi

    # ===============================
    # 3. 无 ERROR → 检查 PTP 同步
    # ===============================
    if [ "$CURRENT_SIDE_LOWER" == "left" ] && [ -f "/dev/shm/umi_ptp_status" ]; then
        ptp_state=$(jq -r '.state // "UNKNOWN"' /dev/shm/umi_ptp_status)
        ptp_offset=$(jq -r '.offset // 0' /dev/shm/umi_ptp_status | awk '{print ($1<0)?-$1:$1}')

        if [ "$ptp_state" != "SLAVE" ] || [ "$ptp_offset" -ge "$PTP_OFFSET_THRESHOLD_NS" ]; then
            now_ts=$(date +%s)
            [ "$PTP_WAIT_START_TS" -eq 0 ] && {
                PTP_WAIT_START_TS=$now_ts
                echo "[WARNING]:PTP first enter abnormal state: state=$ptp_state offset=$ptp_offset"
            }

            wait_elapsed=$((now_ts - PTP_WAIT_START_TS))

            if [ "$wait_elapsed" -ge "$PTP_WAIT_TIMEOUT" ]; then
                local ptp_error_msg=" PTP sync timeout (state=${ptp_state}, offset=${ptp_offset}, waited=${wait_elapsed}s);"
                local ptp_error_key="${ERROR_NET_SYNC}|PTP sync timeout"

                if [ "$SYSTEM_HEALTH_STATUS" != "ERROR" ] || [ "$LAST_MONITOR_ERROR_MSG" != "$ptp_error_key" ]; then
                    echo "[ERROR]:[$(date)] MONITOR ERROR (${ERROR_NET_SYNC}):${ptp_error_msg}"
                    set_error_state "$ERROR_NET_SYNC"
                    notify_audio "error"
                fi

                SYSTEM_HEALTH_STATUS="ERROR"
                LAST_MONITOR_ERROR_MSG="$ptp_error_key"
                return
            fi

            # --- 同步进度 ---
            progress=$(awk -v off="$ptp_offset" -v max="$PTP_OFFSET_MAX_NS" '
                BEGIN {
                    p = 1.0 - (off / max)
                    if (p < 0) p = 0
                    if (p > 1) p = 1
                    printf "%.2f", p
                }')

            set_state "CALIB_RUN:$progress"

            SYSTEM_HEALTH_STATUS="WAITING"
            LAST_MONITOR_ERROR_MSG=""
            return
        elif [ "$PTP_WAIT_START_TS" -ne 0 ]; then
            # --- PTP 已同步 ---
            PTP_WAIT_START_TS=0
        fi
    fi

    # ===============================
    # 4. 全部正常 → READY
    # ===============================
    if [ "$SYSTEM_HEALTH_STATUS" != "OK" ]; then
        set_state "READY"
        notify_audio "ready"
        SYSTEM_HEALTH_STATUS="OK"
    fi
    LAST_MONITOR_ERROR_MSG=""
}

monitor_loop() {
    local now_ts=0
    local last_probe_ts=0
    while true; do
        now_ts=$(date +%s)
        if [ "$now_ts" -ne "$last_probe_ts" ]; then
            refresh_fays_presence_cache
            last_probe_ts="$now_ts"
        fi
        monitor_system_health
        sleep 0.05  # 控制检查频率
    done
}

# 函数：发送网络命令 (仅 Right 调用)
send_network_command() {
    local cmd=$1
    local arg=$2
    local payload="${cmd}|${arg}"

    # 只发送，不在主流程等待 Left 响应/连接结果。
    (
        printf "%s\n" "$payload" | nc "$IP_LEFT" "$SYNC_PORT" >/dev/null 2>&1 || true
    ) &
    echo "Sent to Left (async): ${payload}"
}

# 函数：计算新路径并创建文件夹
# 修改：支持传入指定的文件夹名 (用于 Left 同步 Right 的命名)
prepare_directory() {
    local specified_name=$1
    
    if [ -n "$specified_name" ]; then
        # [Slave 模式] 使用 Master 指定的名字
        TARGET_DIR="${DIR_DATA}/${specified_name}"
    else
        # [Master 模式] 自动生成名字
        local date_str=$(date +%Y%m%d)

        # 搜索当天的最大序号（只根据日期前缀）
        local last_id=$(find "$DIR_DATA" -maxdepth 1 -type d \
            -name "episode_${date_str}_*" \
            -printf "%f\n" | \
            awk -F_ '{print $NF}' | \
            grep -E '^[0-9]+$' | \
            sort -n | tail -1)

        local new_id=1
        if [ -n "$last_id" ]; then
            new_id=$((10#$last_id + 1))
        fi

        local id_str=$(printf "%04d" "$new_id")
        
        # 仅返回文件夹的基础名称，不含全路径
        local dirname="episode_${date_str}_${id_str}"
        TARGET_DIR="${DIR_DATA}/${dirname}"
    fi

    mkdir -p "$TARGET_DIR"

    echo "[INFO]:New recording session: $TARGET_DIR"
}

# 函数：在 episode 目录写入 metadata/calibration 信息
prepare_episode_manifest_files() {
    local episode_dir="$1"
    local episode_meta_file="$episode_dir/metadata.json"
    local episode_calib_file="$episode_dir/calibration.json"

    if [ -f "$META_FILE" ]; then
        cp -f "$META_FILE" "$episode_meta_file"
    else
        echo "[WARNING]:Metadata source missing: $META_FILE"
    fi

    local calib_items=()
    [ -f "$DIR_CALIB/cam.json" ] && calib_items+=("\"cam\"")
    [ -f "$DIR_CALIB/imu.json" ] && calib_items+=("\"imu\"")
    [ -f "$DIR_CALIB/encoder.json" ] && calib_items+=("\"encoder\"")

    local joined_calib
    joined_calib=$(IFS=,; echo "${calib_items[*]}")
    printf '[%s]\n' "$joined_calib" > "$episode_calib_file"
}

# 函数：在当前 episode 的 info.json 上写入复位 tag
mark_reset_tag_to_info() {
    if [ "$CURRENT_RECORDING_IS_RESET" != true ]; then
        return 0
    fi

    if [ -z "$TARGET_DIR" ] || [ ! -d "$TARGET_DIR" ]; then
        echo "[WARNING]:Skip reset tag: invalid TARGET_DIR ($TARGET_DIR)."
        return 1
    fi

    local info_file="$TARGET_DIR/info.json"
    if [ ! -f "$info_file" ]; then
        echo "[WARNING]:Skip reset tag: info.json not found at $info_file"
        return 1
    fi

    local tmp_file="$info_file.tmp"
    local source_name
    source_name=$(basename "$CURRENT_RECORDING_RESET_SOURCE_DIR")

    if jq \
        --arg source_dir "$CURRENT_RECORDING_RESET_SOURCE_DIR" \
        --arg source_name "$source_name" \
        '
        .tags = ((.tags // []) + ["reset"]) |
        .tags |= unique |
        .reset_info = {
            "is_reset_operation": true,
            "source_episode_dir": $source_dir,
            "source_episode_name": $source_name
        }
        ' "$info_file" > "$tmp_file"; then
        mv "$tmp_file" "$info_file"
        echo "[INFO]:Reset tag written to info.json: source=$CURRENT_RECORDING_RESET_SOURCE_DIR"
        return 0
    fi

    echo "[ERROR]:Failed to write reset tag to info.json"
    rm -f "$tmp_file"
    return 1
}

# 函数：启动音频录制
record_audio() {
    # 如果是 Left (Slave)，直接禁用录音功能
    if [ "$CURRENT_SIDE_LOWER" == "left" ]; then
        echo "[INFO]:Audio recording disabled on Slave (Left) side."
        return
    fi

    local audio_type=$1
    local mode=${2:-"hold"}
    local monitor_gpio=${3:-""}
    local timestamp=$(date +%Y%m%d_%H%M%S)
    local temp_file="$AUDIO_TEMP_DIR/audio_${audio_type}_${timestamp}.wav"
    local capture_file="$AUDIO_TEMP_DIR/audio_${audio_type}_${timestamp}_capture.wav"
    local ch1_file="$AUDIO_TEMP_DIR/audio_${audio_type}_${timestamp}_ch1.wav"
    local ch1_denoised="$AUDIO_TEMP_DIR/audio_${audio_type}_${timestamp}_ch1_denoised.wav"
    local noise_profile="$script_dir/audio/noise.prof"
    
    echo "[INFO]:Starting $audio_type audio recording (Mode: $mode)..."
    if [ "$audio_type" = "pre" ]; then
        notify_audio "pre_audio_recording"
    elif [ "$audio_type" = "post" ]; then
        notify_audio "post_audio_recording"
    fi
    
    arecord -D hw:rockchipes8388,0 -f cd -r 44100 -c 2 -t wav "$capture_file" &
    local arecord_pid=$!

    if [ "$mode" != "hold" ]; then
        echo "[WARNING]:Unsupported audio mode '$mode', fallback to hold."
        mode="hold"
    fi

    if [ -z "$monitor_gpio" ]; then
        echo "[ERROR]:Mode $mode requires GPIO monitor line."
        kill $arecord_pid 2>/dev/null
        wait $arecord_pid 2>/dev/null
        rm -f "$capture_file"
        notify_audio "audio_recording_stop"
        return 1
    fi
    
    # 按键检测逻辑 (仅在 Right 有效)
    echo "[INFO]:Recording... (Release button to stop)"
    while kill -0 $arecord_pid 2>/dev/null; do
        if [ "$(gpioget $monitor_gpio)" -ne "$BTN_ACTIVE_LEVEL" ]; then
            kill -SIGINT $arecord_pid 2>/dev/null
            break
        fi
        sleep 0.05
    done
    
    # 等待录音进程完全结束
    wait $arecord_pid 2>/dev/null
    
    # 降噪处理：固定提取 ch1，单声道降噪后输出。
    if [ -s "$capture_file" ]; then
        if [ -f "$noise_profile" ] \
            && sox "$capture_file" "$ch1_file" remix 1 \
            && sox "$ch1_file" "$ch1_denoised" noisered "$noise_profile" 0.15 \
            && sox "$ch1_denoised" "$temp_file" norm; then
            echo "[INFO]:Audio denoise completed on ch1."
        else
            echo "[WARNING]:Denoise pipeline failed or noise profile missing, fallback to direct normalization."
            if ! sox "$capture_file" "$ch1_file" remix 1 \
                || ! sox "$ch1_file" "$temp_file" norm; then
                echo "[ERROR]:Audio post-process failed."
                rm -f "$capture_file" "$ch1_file" "$ch1_denoised"
                notify_audio "audio_recording_stop"
                return 1
            fi
        fi
        rm -f "$capture_file" "$ch1_file" "$ch1_denoised"
    else
        echo "[WARNING]:No audio data recorded"
        return 1
    fi

    # 根据音频类型处理
    if [ "$audio_type" = "pre" ]; then
        PRE_AUDIO_FILE="$temp_file"
        echo "[INFO]:Pre-audio stored for next episode"
    elif [ "$audio_type" = "post" ]; then
        if [ -n "$LAST_EPISODE_DIR" ] && [ -d "$LAST_EPISODE_DIR" ]; then
            mv "$temp_file" "$LAST_EPISODE_DIR/audio_post.wav"
            echo "[INFO]:Post-audio moved to last episode: $LAST_EPISODE_DIR"
        else
            echo "[WARNING]:No previous episode found for post-audio"
            rm -f "$temp_file"
        fi
    fi

    sync -f "$DISK_DIR"
    
    # 播放录制完成提示音
    notify_audio "audio_recording_stop"
}

is_pid_alive() {
    local pid="$1"
    [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null
}

wait_for_pid_exit() {
    local pid="$1"
    local timeout_s="${2:-5}"
    local start_ts now_ts

    [ -z "$pid" ] && return 0

    start_ts=$(date +%s)
    while is_pid_alive "$pid"; do
        now_ts=$(date +%s)
        if [ $((now_ts - start_ts)) -ge "$timeout_s" ]; then
            return 1
        fi
        sleep 0.1
    done

    return 0
}

wait_for_pids_exit() {
    local timeout_s="${1:-5}"
    shift

    local start_ts now_ts pid alive
    start_ts=$(date +%s)

    while true; do
        alive=0
        for pid in "$@"; do
            if is_pid_alive "$pid"; then
                alive=1
                break
            fi
        done

        [ "$alive" -eq 0 ] && return 0

        now_ts=$(date +%s)
        if [ $((now_ts - start_ts)) -ge "$timeout_s" ]; then
            return 1
        fi
        sleep 0.1
    done
}

force_stop_pid() {
    local pid="$1"
    local name="$2"

    [ -z "$pid" ] && return 0
    if ! is_pid_alive "$pid"; then
        wait "$pid" 2>/dev/null || true
        return 0
    fi

    kill -15 "$pid" 2>/dev/null || true
    if ! wait_for_pid_exit "$pid" 2; then
        echo "[WARNING]:$name (PID=$pid) did not exit after SIGTERM, sending SIGKILL."
        kill -9 "$pid" 2>/dev/null || true
        wait_for_pid_exit "$pid" 1 || true
    fi

    wait "$pid" 2>/dev/null || true
}

detect_fays_ftdi_present_raw() {
    if ! command -v v4l2-ctl >/dev/null 2>&1; then
        return 1
    fi

    local devices
    devices=$(v4l2-ctl --list-devices 2>/dev/null || true)
    echo "$devices" | grep -q "FTDI Superspeed Video Bridge"
}

refresh_fays_presence_cache() {
    if detect_fays_ftdi_present_raw; then
        echo "1" > "$FAYS_PRESENT_CACHE_FILE"
    else
        echo "0" > "$FAYS_PRESENT_CACHE_FILE"
    fi
}

is_fays_ftdi_present() {
    if [ -f "$FAYS_PRESENT_CACHE_FILE" ]; then
        [ "$(cat "$FAYS_PRESENT_CACHE_FILE" 2>/dev/null)" = "1" ]
        return
    fi
    detect_fays_ftdi_present_raw
}

start_fays_daemon() {
    if [ "$FAYS_ENABLED" != true ]; then
        return 0
    fi

    if is_pid_alive "$PID_FAYS_DAEMON"; then
        return 0
    fi

    if [ ! -x "$FAYS_RECORD_SCRIPT" ]; then
        echo "[ERROR]:Fays record script missing or not executable: $FAYS_RECORD_SCRIPT"
        return 1
    fi

    echo "[INFO]:Starting FaysSense daemon (warmup mode)..."
    # Filter noisy SDK FPS logs while keeping other daemon output visible.
    "$FAYS_RECORD_SCRIPT" daemon > >(sed -u -e '/IMU FPS:/d' -e '/Stereo FPS:/d') 2>&1 &
    PID_FAYS_DAEMON=$!

    sleep 0.5
    if ! is_pid_alive "$PID_FAYS_DAEMON"; then
        echo "[ERROR]:FaysSense daemon failed to start."
        PID_FAYS_DAEMON=""
        return 1
    fi

    # 等待 daemon 打开控制 FIFO 读端，避免紧接着 START 写入超时。
    local _ready=0
    for _ in $(seq 1 20); do
        if is_pid_alive "$PID_FAYS_DAEMON" && [ -p "/tmp/umi_fays_cmd" ] && \
           ls -l "/proc/$PID_FAYS_DAEMON/fd" 2>/dev/null | grep -q "/tmp/umi_fays_cmd"; then
            _ready=1
            break
        fi
        sleep 0.1
    done
    if [ "$_ready" -ne 1 ]; then
        echo "[WARNING]:Fays daemon started but control FIFO reader not ready yet."
    fi

    echo "[INFO]:FaysSense daemon started. PID=$PID_FAYS_DAEMON"
    return 0
}

start_fays_recording_session() {
    local output_dir="$1"
    local attempt=1
    local max_attempts=1

    if [ "$FAYS_ENABLED" != true ]; then
        return 0
    fi

    if ! start_fays_daemon; then
        return 1
    fi

    while [ "$attempt" -le "$max_attempts" ]; do
        if "$FAYS_RECORD_SCRIPT" start "$output_dir"; then
            return 0
        fi

        if ! is_pid_alive "$PID_FAYS_DAEMON"; then
            echo "[ERROR]:FaysSense daemon exited before START could be delivered."
            break
        fi

        echo "[WARNING]:Failed to send START command to FaysSense daemon (attempt ${attempt}/${max_attempts}). Retrying..."
        attempt=$((attempt + 1))
        sleep 0.2
    done

    echo "[ERROR]:Failed to send START command to FaysSense daemon."
    return 1
}

stop_fays_recording_session() {
    if [ "$FAYS_ENABLED" != true ]; then
        return 0
    fi

    if ! is_pid_alive "$PID_FAYS_DAEMON"; then
        return 0
    fi

    if ! "$FAYS_RECORD_SCRIPT" stop; then
        echo "[WARNING]:Failed to send STOP command to FaysSense daemon."
        return 1
    fi

    return 0
}

check_and_maintain_fays() {
    local now_ts
    now_ts=$(date +%s)
    if [ "$now_ts" -eq "$FAYS_LAST_MONITOR_TS" ]; then
        return
    fi
    FAYS_LAST_MONITOR_TS="$now_ts"

    local present_now=false
    if is_fays_ftdi_present; then
        present_now=true
    fi

    # Case 1: startup had no Fays, but device is plugged later.
    if [ "$FAYS_ENABLED" != true ] && [ "$present_now" = true ]; then
        echo "[INFO]:Fays FTDI detected. Enabling Fays recording."
        FAYS_ENABLED=true
        FAYS_USB_PRESENT=true
        if start_fays_daemon; then
            if [ "$IS_RECORDING" = true ] && [ -n "$TARGET_DIR" ] && [ -d "$TARGET_DIR" ]; then
                echo "[INFO]:Fays daemon recovered during recording. Skip START replay for current episode."
            fi
        else
            echo "[WARNING]:Fays detected but daemon start failed. Will retry."
        fi
        return
    fi

    # Case 2: Fays enabled but FTDI removed.
    if [ "$FAYS_ENABLED" = true ] && [ "$present_now" != true ]; then
        if [ "$FAYS_USB_PRESENT" = true ]; then
            echo "[WARNING]:Fays FTDI disconnected."
        fi
        FAYS_USB_PRESENT=false
        FAYS_RECORDING_ACTIVE=false
        stop_fays_daemon || true
        return
    fi

    # Case 3: Fays enabled and FTDI present -> keep daemon healthy.
    if [ "$FAYS_ENABLED" = true ] && [ "$present_now" = true ]; then
        if [ "$FAYS_USB_PRESENT" != true ]; then
            echo "[INFO]:Fays FTDI reconnected."
        fi
        FAYS_USB_PRESENT=true

        if ! is_pid_alive "$PID_FAYS_DAEMON" || [ ! -p "/tmp/umi_fays_cmd" ]; then
            echo "[WARNING]:Fays daemon/FIFO unavailable. Attempting restart..."
            stop_fays_daemon || true
            if start_fays_daemon; then
                if [ "$IS_RECORDING" = true ] && [ -n "$TARGET_DIR" ] && [ -d "$TARGET_DIR" ]; then
                    echo "[INFO]:Fays daemon recovered during recording. Skip START replay for current episode."
                fi
            else
                echo "[WARNING]:Fays daemon restart failed. Will retry."
            fi
        fi
    fi
}

get_video_timestamp_span_sec() {
    local file_path="$1"
    local probe_output=""
    local start_ts=""
    local duration_val=""
    local span_ts=""

    [ -f "$file_path" ] || return 1

    probe_output=$(timeout "$VIDEO_SPAN_PROBE_TIMEOUT_SEC" ffprobe -v error \
        -show_entries format=start_time,duration \
        -of default=noprint_wrappers=1:nokey=1 \
        "$file_path" 2>/dev/null | sed '/^[[:space:]]*$/d')

    start_ts=$(printf "%s\n" "$probe_output" | sed -n '1p' | tr -d '\r')
    duration_val=$(printf "%s\n" "$probe_output" | sed -n '2p' | tr -d '\r')

    if [ -z "$start_ts" ] || [ -z "$duration_val" ] || [ "$start_ts" = "N/A" ] || [ "$duration_val" = "N/A" ]; then
        return 1
    fi

    if ! awk -v s="$start_ts" -v d="$duration_val" 'BEGIN { exit ((s+0>=0) && (d+0>0)) ? 0 : 1 }'; then
        return 1
    fi

    span_ts="$duration_val"

    # 某些 copyts 场景下 format.duration 可能表现为“绝对结束时间”，
    # 只要 duration_val > start_ts，就按 end-start 归一化。
    if awk -v s="$start_ts" -v d="$duration_val" 'BEGIN { exit (d > s) ? 0 : 1 }'; then
        span_ts=$(awk -v s="$start_ts" -v e="$duration_val" 'BEGIN { printf "%.6f", e - s }')
    fi

    if ! awk -v x="$span_ts" 'BEGIN { exit (x+0>0) ? 0 : 1 }'; then
        return 1
    fi

    printf "%s" "$span_ts"
    return 0
}

get_mcap_summary_span_sec() {
    local file_path="$1"
    local span_ts=""
    local -a py_cmd

    [ -f "$file_path" ] || return 1

    if [ -x "./.venv/bin/python" ]; then
        py_cmd=("./.venv/bin/python")
    else
        py_cmd=(uv run python)
    fi

    # 只读取 MCAP summary 统计中的最早/最晚消息时间戳，不扫描全量消息。
    span_ts=$(timeout 2 "${py_cmd[@]}" - "$file_path" <<'PY' 2>/dev/null || true
import sys
from mcap.reader import make_reader

mcap_path = sys.argv[1]

try:
    with open(mcap_path, "rb") as fh:
        summary = make_reader(fh).get_summary()

    if summary is None or summary.statistics is None:
        raise RuntimeError("missing summary statistics")

    start_ns = int(summary.statistics.message_start_time)
    end_ns = int(summary.statistics.message_end_time)
    if end_ns <= start_ns:
        raise RuntimeError("invalid message time range")

    print(f"{(end_ns - start_ns) / 1_000_000_000:.6f}")
except Exception:
    sys.exit(1)
PY
)

    if [ -z "$span_ts" ]; then
        return 1
    fi

    if ! awk -v x="$span_ts" 'BEGIN { exit (x+0>0) ? 0 : 1 }'; then
        return 1
    fi

    printf "%s" "$span_ts"
    return 0
}

stop_fays_daemon() {
    if ! is_pid_alive "$PID_FAYS_DAEMON"; then
        PID_FAYS_DAEMON=""
        FAYS_RECORDING_ACTIVE=false
        return 0
    fi

    echo "[INFO]:Stopping FaysSense daemon..."
    "$FAYS_RECORD_SCRIPT" stop >/dev/null 2>&1 || true
    "$FAYS_RECORD_SCRIPT" exit >/dev/null 2>&1 || true

    for _ in $(seq 1 30); do
        if ! is_pid_alive "$PID_FAYS_DAEMON"; then
            break
        fi
        sleep 0.1
    done

    if is_pid_alive "$PID_FAYS_DAEMON"; then
        kill -2 "$PID_FAYS_DAEMON" 2>/dev/null || true
    fi

    if ! wait_for_pid_exit "$PID_FAYS_DAEMON" 2; then
        echo "[WARNING]:Fays daemon did not exit in time, sending SIGKILL."
        kill -9 "$PID_FAYS_DAEMON" 2>/dev/null || true
        wait_for_pid_exit "$PID_FAYS_DAEMON" 1 || true
    fi

    wait "$PID_FAYS_DAEMON" 2>/dev/null || true
    PID_FAYS_DAEMON=""
    FAYS_RECORDING_ACTIVE=false
}

# 函数：启动所有录制进程
# 参数1 (可选): 强制指定的目录名 (用于 Slave)
start_recording() {
    local sync_dir_name=$1
    local reset_source_candidate="${NEXT_RESET_SOURCE_EPISODE_DIR:-}"

    CURRENT_RECORDING_IS_RESET=false
    CURRENT_RECORDING_RESET_SOURCE_DIR=""
    CURRENT_RECORDING_EXPECT_FAYS=false
    FAYS_RECORDING_ACTIVE=false
    NEXT_RESET_SOURCE_EPISODE_DIR=""

    if [ -n "$reset_source_candidate" ] && [ -d "$reset_source_candidate" ]; then
        CURRENT_RECORDING_IS_RESET=true
        CURRENT_RECORDING_RESET_SOURCE_DIR="$reset_source_candidate"
        echo "[INFO]:This recording is marked as RESET. Source: $CURRENT_RECORDING_RESET_SOURCE_DIR"
    fi

    # 1. 创建锁 (防止 NTP 在录制期间运行)
    touch "$RECORDING_LOCK_FILE"

    # 2. 准备目录
    prepare_directory "$sync_dir_name"

    # 2.1 录制开始时，将 metadata/calibration 写入当前 episode 目录
    prepare_episode_manifest_files "$TARGET_DIR"
    
    # 3. 如果是 Master，需要通知 Slave
    if [ "$CURRENT_SIDE_LOWER" == "right" ]; then
        # 获取纯文件夹名
        local dirname=$(basename "$TARGET_DIR")
        # 发送 START 指令和文件夹名
        send_network_command "START" "$dirname"
    fi

    # 4. 处理预录制音频 (仅 Master)
    if [ "$CURRENT_SIDE_LOWER" == "right" ] && [ -n "$PRE_AUDIO_FILE" ] && [ -f "$PRE_AUDIO_FILE" ]; then
        mv "$PRE_AUDIO_FILE" "$TARGET_DIR/audio_pre.wav"
        PRE_AUDIO_FILE=""
    fi
    
    # --- 记录时间同步状态 (仅打印到控制台) ---
    if [ -f "/dev/shm/umi_ptp_status" ]; then
        # 读取状态文件
        ptp_data=$(cat /dev/shm/umi_ptp_status)
        
        # 解析各个字段
        ptp_state=$(echo "$ptp_data" | jq -r '.state // "UNKNOWN"')
        ptp_offset=$(echo "$ptp_data" | jq -r '.offset // 0')
        sys_offset=$(echo "$ptp_data" | jq -r '.sys_offset // 0')

        echo "  - PTP State: $ptp_state"
        echo "  - PTP Offset: ${ptp_offset} ns"
        echo "  - Sys Offset: ${sys_offset} ns"
    else
        echo "  - WARNING: PTP status file (/dev/shm/umi_ptp_status) not found."
    fi
    # -----------------------------------------------

    time_stamp=$(date +"%Y-%m-%d %H:%M:%S")
    echo "[INFO]:Recording started at: $time_stamp"

    if [ ! -x "$SENSOR_RECORDER_BIN" ]; then
        echo "[ERROR]:Sensor recorder binary not found or not executable: $SENSOR_RECORDER_BIN"
        set_error_state "$ERROR_RUNTIME"
        notify_audio "error"
        rm -f "$RECORDING_LOCK_FILE"
        return 1
    fi

    # 若启动时未启用 Fays，但录制前设备已插入，则即时启用。
    if [ "$FAYS_ENABLED" != true ] && is_fays_ftdi_present; then
        echo "[INFO]:Fays FTDI detected before recording. Enabling Fays."
        FAYS_ENABLED=true
        FAYS_USB_PRESENT=true
        start_fays_daemon || true
    fi

    # FaysSense 为可选设备：仅在启用时尝试进入录制态。
    if [ "$FAYS_ENABLED" = true ]; then
        if start_fays_recording_session "$TARGET_DIR"; then
            CURRENT_RECORDING_EXPECT_FAYS=true
            FAYS_RECORDING_ACTIVE=true
        else
            echo "[WARNING]:Fays recording start failed, continuing without Fays for this episode."
            FAYS_RECORDING_ACTIVE=false
        fi
    else
        echo "[INFO]:Fays not enabled, skip Fays recording for this episode."
    fi
    
    # 启动相机（每次录制前刷新 /etc/environment 中的 CAMERA_CODEC）
    local camera_codec_from_env=""
    camera_codec_from_env="$(get_env_value "CAMERA_CODEC" || true)"
    if [ -n "$camera_codec_from_env" ]; then
        CAMERA_CODEC="${camera_codec_from_env,,}"
    fi

    # 启动相机
    if [ "$CAMERA_CODEC" != "h264" ] && [ "$CAMERA_CODEC" != "h265" ]; then
        echo "[WARNING]:Invalid CAMERA_CODEC=$CAMERA_CODEC, fallback to h264."
        CAMERA_CODEC="h264"
    fi
    uv run ./camera_record/triple_camera_record.py --codec "$CAMERA_CODEC" --output-dir "$TARGET_DIR" &
    PID_CAM=$!

    "$SENSOR_RECORDER_BIN" "$TARGET_DIR" &
    PID_SENSOR=$!
    
    IS_RECORDING=true
    LAST_EPISODE_DIR="$TARGET_DIR"  # 更新上次录制目录
    
    set_state "RECORDING"
    if [ "$CURRENT_RECORDING_IS_RESET" = true ]; then
        notify_audio "reset_recording_start"
    else
        notify_audio "recording_start"
    fi
}

# ================= 数据校验函数 =================
validate_recording() {
    local dir=$1
    local validation_pass=true
    local error_details=""
    local span_tmp_dir=""
    local cam_span_file=""
    local fays_span_file=""
    local tact_left_span_file=""
    local tact_right_span_file=""
    local sensor_mcap_span_file=""
    local pid_cam_span=""
    local pid_fays_span=""
    local pid_tact_left_span=""
    local pid_tact_right_span=""
    local pid_sensor_mcap_span=""
    local cam_file="$dir/cam.mkv"
    local fays_file="$dir/fays_stereo_output.mkv"
    local tact_left_file="$dir/tact_left.mkv"
    local tact_right_file="$dir/tact_right.mkv"
    local sensor_mcap_file="$dir/sensor_data.mcap"

    echo "[INFO]:Validating data in: $dir"

    # --- 1. 检查 MKV 文件大小 ---
    # 检查 cam.mkv, tact_left.mkv, tact_right.mkv 是否存在且大小不为 0
    # 注意：根据实际生成的文件名可能需要调整，这里假设文件名如下
    local mkv_files=("cam.mkv" "tact_left.mkv" "tact_right.mkv")
    if [ "$CURRENT_RECORDING_EXPECT_FAYS" = true ]; then
        mkv_files+=("fays_stereo_output.mkv")
    else
        echo "[INFO]:Skip Fays MKV validation for this episode (Fays not expected)."
    fi
    
    for fname in "${mkv_files[@]}"; do
        local fpath="$dir/$fname"
        # 检查文件是否存在
        if [ ! -f "$fpath" ]; then
            echo "[ERROR]:$fname missing (might be optional based on config)."
            validation_pass=false
        else
            local fsize=$(stat -c%s "$fpath" 2>/dev/null || echo 0)
            if [ "$fsize" -eq 0 ]; then
                validation_pass=false
                error_details="${error_details} Zero-byte file: $fname;"
                echo "[ERROR]:$fname is 0 bytes."
            fi
        fi
    done

    span_tmp_dir=$(mktemp -d /tmp/umi_validate_span.XXXXXX 2>/dev/null || mktemp -d)
    cam_span_file="$span_tmp_dir/cam.span"
    fays_span_file="$span_tmp_dir/fays.span"
    tact_left_span_file="$span_tmp_dir/tact_left.span"
    tact_right_span_file="$span_tmp_dir/tact_right.span"
    sensor_mcap_span_file="$span_tmp_dir/sensor.span"

    if [ "$CURRENT_RECORDING_EXPECT_FAYS" = true ] && [ -f "$cam_file" ]; then
        (get_video_timestamp_span_sec "$cam_file" >"$cam_span_file" 2>/dev/null || true) &
        pid_cam_span=$!
    fi
    if [ "$CURRENT_RECORDING_EXPECT_FAYS" = true ] && [ -f "$fays_file" ]; then
        (get_video_timestamp_span_sec "$fays_file" >"$fays_span_file" 2>/dev/null || true) &
        pid_fays_span=$!
    fi
    if [ -f "$sensor_mcap_file" ]; then
        (get_mcap_summary_span_sec "$sensor_mcap_file" >"$sensor_mcap_span_file" 2>/dev/null || true) &
        pid_sensor_mcap_span=$!
        if [ -f "$tact_left_file" ]; then
            (get_video_timestamp_span_sec "$tact_left_file" >"$tact_left_span_file" 2>/dev/null || true) &
            pid_tact_left_span=$!
        fi
        if [ -f "$tact_right_file" ]; then
            (get_video_timestamp_span_sec "$tact_right_file" >"$tact_right_span_file" 2>/dev/null || true) &
            pid_tact_right_span=$!
        fi
    fi

    # --- 1.5 Fays 与主相机视频起止时间差校验 ---
    if [ "$CURRENT_RECORDING_EXPECT_FAYS" = true ]; then
        if [ -f "$cam_file" ] && [ -f "$fays_file" ]; then
            local cam_span=""
            local fays_span=""
            [ -n "$pid_cam_span" ] && wait "$pid_cam_span" 2>/dev/null || true
            [ -n "$pid_fays_span" ] && wait "$pid_fays_span" 2>/dev/null || true
            [ -f "$cam_span_file" ] && cam_span=$(cat "$cam_span_file")
            [ -f "$fays_span_file" ] && fays_span=$(cat "$fays_span_file")

            if [ -z "$cam_span" ] || [ -z "$fays_span" ]; then
                validation_pass=false
                error_details="${error_details} Failed to read video timestamp span (cam=${cam_span:-NA}, fays=${fays_span:-NA});"
                echo "[ERROR]:Failed to read video timestamp span (cam=${cam_span:-NA}, fays=${fays_span:-NA})."
            else
                local duration_gap
                duration_gap=$(awk -v c="$cam_span" -v f="$fays_span" 'BEGIN { printf "%.3f", c - f }')
                if awk -v g="$duration_gap" -v t="$DURATION_GAP_THRESHOLD_SEC" 'BEGIN { exit (g > t) ? 0 : 1 }'; then
                    validation_pass=false
                    error_details="${error_details} Fays video too short by timestamp span (cam=${cam_span}s, fays=${fays_span}s, gap=${duration_gap}s);"
                    echo "[ERROR]:Fays video too short by timestamp span (cam=${cam_span}s, fays=${fays_span}s, gap=${duration_gap}s)."
                else
                    echo "[INFO]:PASS: video timestamp-span check (cam=${cam_span}s, fays=${fays_span}s, gap=${duration_gap}s)."
                fi
            fi
        fi
    fi

    # --- 2. 检查 MCAP 文件 ---
    local mcap_files=("sensor_data.mcap")
    if [ "$CURRENT_RECORDING_EXPECT_FAYS" = true ]; then
        mcap_files+=("fays_data.mcap")
    else
        echo "[INFO]:Skip Fays MCAP validation for this episode (Fays not expected)."
    fi
    for mcap_name in "${mcap_files[@]}"; do
        local mcap_file="$dir/$mcap_name"
        if [ -f "$mcap_file" ]; then
            local mcap_size
            mcap_size=$(stat -c%s "$mcap_file" 2>/dev/null || echo 0)
            if [ "$mcap_size" -le 1024 ]; then
                validation_pass=false
                error_details="${error_details} ${mcap_name} too small (${mcap_size}B);"
                echo "[ERROR]:${mcap_name} size (${mcap_size} bytes) looks invalid."
            else
                echo "[INFO]:PASS: ${mcap_name} present (${mcap_size} bytes)."
            fi
        else
            echo "[ERROR]:${mcap_name} missing."
            validation_pass=false
            error_details="${error_details} ${mcap_name} missing;"
        fi
    done

    # --- 2.5 tact 视频与传感器 MCAP 时长一致性校验 ---
    if [ -f "$sensor_mcap_file" ]; then
        local sensor_mcap_span=""
        [ -n "$pid_sensor_mcap_span" ] && wait "$pid_sensor_mcap_span" 2>/dev/null || true
        [ -f "$sensor_mcap_span_file" ] && sensor_mcap_span=$(cat "$sensor_mcap_span_file")
        if [ -z "$sensor_mcap_span" ]; then
            validation_pass=false
            error_details="${error_details} Failed to read sensor_data.mcap summary span;"
            echo "[ERROR]:Failed to read sensor_data.mcap summary span."
        else
            local tact_name=""
            local tact_span_file=""
            local tact_pid=""
            for tact_name in tact_left.mkv tact_right.mkv; do
                local tact_file="$dir/$tact_name"
                [ -f "$tact_file" ] || continue

                case "$tact_name" in
                    tact_left.mkv)
                        tact_span_file="$tact_left_span_file"
                        tact_pid="$pid_tact_left_span"
                        ;;
                    tact_right.mkv)
                        tact_span_file="$tact_right_span_file"
                        tact_pid="$pid_tact_right_span"
                        ;;
                    *)
                        tact_span_file=""
                        tact_pid=""
                        ;;
                esac

                local tact_span=""
                [ -n "$tact_pid" ] && wait "$tact_pid" 2>/dev/null || true
                [ -n "$tact_span_file" ] && [ -f "$tact_span_file" ] && tact_span=$(cat "$tact_span_file")
                if [ -z "$tact_span" ]; then
                    validation_pass=false
                    error_details="${error_details} Failed to read ${tact_name} timestamp span;"
                    echo "[ERROR]:Failed to read ${tact_name} timestamp span."
                    continue
                fi

                local duration_gap_abs=""
                duration_gap_abs=$(awk -v v="$tact_span" -v m="$sensor_mcap_span" 'BEGIN { d=v-m; if (d<0) d=-d; printf "%.3f", d }')
                if awk -v g="$duration_gap_abs" -v t="$DURATION_GAP_THRESHOLD_SEC" 'BEGIN { exit (g > t) ? 0 : 1 }'; then
                    validation_pass=false
                    error_details="${error_details} ${tact_name} vs sensor_data.mcap span gap too large (video=${tact_span}s, mcap=${sensor_mcap_span}s, gap=${duration_gap_abs}s);"
                    echo "[ERROR]:${tact_name} vs sensor_data.mcap span gap too large (video=${tact_span}s, mcap=${sensor_mcap_span}s, gap=${duration_gap_abs}s)."
                else
                    echo "[INFO]:PASS: ${tact_name} vs sensor_data.mcap span check (video=${tact_span}s, mcap=${sensor_mcap_span}s, gap=${duration_gap_abs}s)."
                fi
            done
        fi
    fi

    rm -rf "$span_tmp_dir"

    # --- 3. 结果处理 ---
    if [ "$validation_pass" = false ]; then
        echo ">>> VALIDATION FAILED: $error_details"
        # 触发报错灯光和声音
        set_error_state "$ERROR_DATA_INTEGRITY"
        notify_audio "validation_failed"
        
        # 在数据文件夹中写入一个错误日志
        echo "Validation failed at $(date): $error_details" > "$dir/validation_error.log"
        
        # 强制让灯光保持 Error 状态一小段时间，避免马上被 monitor 覆盖
        sleep 3
        return 1
    else
        echo ">>> VALIDATION PASSED."
        return 0
    fi
}

# 函数：停止所有录制进程
stop_recording() {
    # 1. 如果是 Master，先通知 Slave 停止
    if [ "$CURRENT_SIDE_LOWER" == "right" ]; then
        send_network_command "STOP" "0"
    fi

    time_stamp=$(date +"%Y-%m-%d %H:%M:%S")
    echo "[INFO]:Recording stopping at: $time_stamp on $CURRENT_SIDE_LOWER"

    # 先默认回 READY，如果校验失败，校验函数会覆盖为 ERROR
    set_state "READY"
    notify_audio "recording_stop"

    # 先通知 FaysSense 停止落盘（进程保持常驻预热）
    if [ "$FAYS_RECORDING_ACTIVE" = true ]; then
        stop_fays_recording_session || true
        FAYS_RECORDING_ACTIVE=false
    fi

    # 发送 SIGINT
    for pid in "$PID_CAM" "$PID_SENSOR"; do
        if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
            kill -2 "$pid"
        fi
    done

    if ! wait_for_pids_exit 5 "$PID_CAM" "$PID_SENSOR"; then
        set_error_state "$ERROR_RUNTIME"
        echo "[ERROR]:Timeout waiting for recording processes to stop. Sending SIGTERM."
        for pid in "$PID_CAM" "$PID_SENSOR"; do
            if is_pid_alive "$pid"; then
                kill -15 "$pid" 2>/dev/null || true
            fi
        done
    fi

    if ! wait_for_pids_exit 2 "$PID_CAM" "$PID_SENSOR"; then
        echo "[WARNING]:Recording processes still alive. Sending SIGKILL."
        for pid in "$PID_CAM" "$PID_SENSOR"; do
            if is_pid_alive "$pid"; then
                kill -9 "$pid" 2>/dev/null || true
            fi
        done
        wait_for_pids_exit 1 "$PID_CAM" "$PID_SENSOR" || true
    fi

    # 回收子进程，避免僵尸
    [ -n "$PID_CAM" ] && wait "$PID_CAM" 2>/dev/null || true
    [ -n "$PID_SENSOR" ] && wait "$PID_SENSOR" 2>/dev/null || true

    # 复位录制：在录制结束后对 info.json 打 tag
    mark_reset_tag_to_info || true

    IS_RECORDING=false
    echo "[INFO]:>>> RECORDING STOPPED. Processes terminated."

    local validation_failed=false

    # 再运行校验 (如果失败，它会把灯变红)
    if [ -d "$TARGET_DIR" ]; then
        if ! validate_recording "$TARGET_DIR"; then
            validation_failed=true
        fi
    fi

    # 校验结束后再刷盘，确保 validation_error.log 等校验产物落盘。
    echo "[INFO]:Syncing data to disk after validation..."
    notify_audio "writing"
    set_state "INIT"
    sync -f "$DISK_DIR"
    set_state "READY"
    notify_audio "ready"

    # 若校验失败，恢复错误态，避免被写盘状态覆盖。
    if [ "$validation_failed" = true ]; then
        set_error_state "$ERROR_DATA_INTEGRITY"
    fi

    # 校验结束后立即做一次增量日志刷盘，降低拔盘前日志未落盘概率。
    sync_logs_once "$LOG_FILE_LOCAL" "$LOG_FILE_DISK" "$LOG_SYNC_POS_FILE" "$LOG_SYNC_LOCK_FILE" || true

    # 清空 PID
    PID_CAM=""
    PID_SENSOR=""
    CURRENT_RECORDING_IS_RESET=false
    CURRENT_RECORDING_RESET_SOURCE_DIR=""
    CURRENT_RECORDING_EXPECT_FAYS=false

    # 删除录制锁文件
    rm -f "$RECORDING_LOCK_FILE"
}

request_system_shutdown() {
    echo "[INFO]:Requesting system shutdown via service trigger..."

    if [ "$IS_RECORDING" = true ]; then
        echo "[INFO]:Recording is active, stopping before shutdown request."
        stop_recording
    fi

    set_state "EXIT"
    notify_audio "writing"
    touch "$SHUTDOWN_REQUEST_FILE"
    echo "[INFO]:Shutdown request file created: $SHUTDOWN_REQUEST_FILE"
}

handle_dual_button_shutdown() {
    local press_start
    local shutdown_triggered=false
    local shutdown_prompted=false

    press_start=$(date +%s.%N)

    while [ "$(gpioget $GPIO_UP_LINE)" -eq "$BTN_ACTIVE_LEVEL" ] && \
          [ "$(gpioget $GPIO_DOWN_LINE)" -eq "$BTN_ACTIVE_LEVEL" ]; do
        local current_time elapsed
        current_time=$(date +%s.%N)
        elapsed=$(echo "$current_time - $press_start" | bc)

        if [ "$shutdown_prompted" = false ] && (( $(echo "$elapsed >= $SHUTDOWN_PROMPT_THRESHOLD" | bc -l) )); then
            shutdown_prompted=true
            echo "[INFO]:Dual-button hold 2s reached. Playing shutdown prompt..."
            notify_audio "shutdown"
        fi

        if (( $(echo "$elapsed >= $DUAL_LONG_PRESS_THRESHOLD" | bc -l) )); then
            shutdown_triggered=true
            echo "[INFO]:Dual-button long press detected. Triggering shutdown service..."
            request_system_shutdown
            break
        fi

        sleep 0.05
    done

    while [ "$(gpioget $GPIO_UP_LINE)" -eq "$BTN_ACTIVE_LEVEL" ] || \
          [ "$(gpioget $GPIO_DOWN_LINE)" -eq "$BTN_ACTIVE_LEVEL" ]; do
        sleep 0.1
    done

    if [ "$shutdown_triggered" = false ]; then
        echo "[INFO]:Dual-button short press detected (ignored)."
    fi
}

cleanup() {
    if [ "$CLEANUP_RUNNING" = true ]; then
        return 0
    fi
    CLEANUP_RUNNING=true
    trap - SIGINT SIGTERM EXIT

    echo "[INFO]:System exit requested."

    # 1. 停止录制业务
    if [ "$IS_RECORDING" = true ]; then
        stop_recording
    fi

    # 停止 FaysSense 常驻进程
    stop_fays_daemon || true

    # 2. 停止音频播放
    # 优先停止音频，防止报错音一直响
    notify_audio "exit"
    if [ -n "$PID_AUDIO_PLAY" ]; then
        sleep 0.1
        force_stop_pid "$PID_AUDIO_PLAY" "Audio player"
    fi

    # 3. 关闭/复原 LED 状态
    # 发送 EXIT 状态给 Python 脚本，令其熄灭灯光
    set_state "EXIT"
    
    # 4. 确保 Python 脚本退出
    if [ -n "$PID_LED_SCRIPT" ]; then
        # 给 Python 脚本 0.5秒 时间处理 "EXIT" 信号并关闭灯光硬件
        sleep 0.5
        force_stop_pid "$PID_LED_SCRIPT" "LED manager"
    fi

    # 5.停止硬件监控
    if [ -n "$MONITOR_PID" ]; then
        force_stop_pid "$MONITOR_PID" "Hardware monitor"
    fi

    # 停止日志同步
    if [ -n "$PID_LOG_SYNC" ]; then
        force_stop_pid "$PID_LOG_SYNC" "Log sync"
    fi
    
    # 5. 清理临时文件
    rm -rf "$AUDIO_TEMP_DIR"
    rm -f "$LED_PIPE" "$AUDIO_PIPE"
    rm -f "$RECORDING_LOCK_FILE"

    echo "[INFO]:Cleanup done."
    exit 0
}

# 捕获 信号 和 退出(EXIT)
# 注意：增加 EXIT 捕获可以确保脚本因任何原因退出时都尝试关灯
trap cleanup SIGINT SIGTERM EXIT

# ================= 逻辑分流：Master (Right) vs Slave (Left) =================
if is_fays_ftdi_present; then
    FAYS_ENABLED=true
    FAYS_USB_PRESENT=true
    if ! start_fays_daemon; then
        echo "[WARNING]:Fays detected but daemon failed during startup. Will retry automatically."
    fi
else
    FAYS_ENABLED=false
    FAYS_USB_PRESENT=false
    echo "[WARNING]:Fays FTDI not detected at startup. Keep service running and stay in error state until recovered."
fi

refresh_fays_presence_cache
monitor_loop &        # 后台运行硬件监控（含 Fays 在位缓存刷新）
MONITOR_PID=$!
echo "[INFO]:Hardware monitor PID: $MONITOR_PID"

if [ "$CURRENT_SIDE_LOWER" == "left" ]; then
    # ================= Slave (Left) 逻辑 =================
    echo "=========================================="
    echo "RUNNING AS SLAVE (LEFT)"
    #echo " - Physical buttons DISABLED"
    #echo " - Audio recording DISABLED"
    echo " - Waiting for commands from RIGHT ($IP_RIGHT)..."
    echo "=========================================="

    set_state "READY"
    # 播放准备就绪提示音
    notify_audio "ready"

    # 网络监听循环
    while true; do
        check_and_maintain_fays

        # 会阻塞执行
        
        # === 网络监听 ===
        # 监听 TCP 端口，收到数据后退出 nc
        # 格式: START|episode_xxxx 或 STOP|0
        raw_msg=$(nc -l -p "$SYNC_PORT" -w 1)
        
        if [ -n "$raw_msg" ]; then
            cmd=$(echo "$raw_msg" | awk -F'|' '{print $1}')
            arg=$(echo "$raw_msg" | awk -F'|' '{print $2}')
            
            echo "[INFO]:Received Network Command: $cmd Args: $arg"

            case "$cmd" in
                "START")
                    if [ "$IS_RECORDING" = false ]; then
                        echo "[INFO]:Trigger: Start Recording (Sync Dir: $arg)"
                        start_recording "$arg"
                    else
                        echo "[WARNING]:Ignored: Already recording."
                    fi
                    ;;
                "STOP")
                    if [ "$IS_RECORDING" = true ]; then
                        echo "[INFO]:Trigger: Stop Recording"
                        stop_recording
                    else
                        echo "[WARNING]:Ignored: Not recording."
                    fi
                    ;;
                *)
                    echo "[WARNING]:Unknown command: $cmd"
                    ;;
            esac
        fi
        
    done

else
    # ================= Master (Right) 逻辑 =================
    echo "=========================================="
    echo "RUNNING AS MASTER (RIGHT)"
    #echo " - Up click: Start/Stop data recording (syncs Left)"
    #echo " - Up long press: Record pre-annotation audio"
    #echo " - Down long press: Record post-annotation audio"
    #echo " - Both long press: System shutdown"
    echo "=========================================="

    set_state "READY"
    # 播放准备就绪提示音
    notify_audio "ready"

    while true; do
        check_and_maintain_fays

        if [ "$DOWN_BUTTON_AVAILABLE" = true ] && [ -n "$GPIO_DOWN_LINE" ] && \
           [ "$(gpioget $GPIO_UP_LINE)" -eq "$BTN_ACTIVE_LEVEL" ] && \
           [ "$(gpioget $GPIO_DOWN_LINE)" -eq "$BTN_ACTIVE_LEVEL" ]; then
            sleep $DEBOUNCE_MS

            if [ "$(gpioget $GPIO_UP_LINE)" -eq "$BTN_ACTIVE_LEVEL" ] && \
               [ "$(gpioget $GPIO_DOWN_LINE)" -eq "$BTN_ACTIVE_LEVEL" ]; then
                handle_dual_button_shutdown

                sleep 0.2
                echo "[INFO]:Waiting for next command..."
                continue
            fi
        fi

        if [ "$(gpioget $GPIO_UP_LINE)" -eq "$BTN_ACTIVE_LEVEL" ]; then
            sleep $DEBOUNCE_MS

            if [ "$(gpioget $GPIO_UP_LINE)" -eq "$BTN_ACTIVE_LEVEL" ]; then
                # 给双键组合留一个短窗口，避免双按时被误判为上键长按音频
                if [ "$DOWN_BUTTON_AVAILABLE" = true ] && [ -n "$GPIO_DOWN_LINE" ]; then
                    sleep "$DUAL_CHORD_WINDOW"
                    if [ "$(gpioget $GPIO_UP_LINE)" -eq "$BTN_ACTIVE_LEVEL" ] && \
                       [ "$(gpioget $GPIO_DOWN_LINE)" -eq "$BTN_ACTIVE_LEVEL" ]; then
                        echo "[INFO]:Dual-button chord detected while UP pending, entering dual-button handler..."
                        handle_dual_button_shutdown
                        continue
                    fi
                fi

                press_start=$(date +%s.%N)
                handled=false

                while [ "$(gpioget $GPIO_UP_LINE)" -eq "$BTN_ACTIVE_LEVEL" ]; do
                    if [ "$DOWN_BUTTON_AVAILABLE" = true ] && [ -n "$GPIO_DOWN_LINE" ] && \
                       [ "$(gpioget $GPIO_DOWN_LINE)" -eq "$BTN_ACTIVE_LEVEL" ]; then
                        handled=true
                        echo "[INFO]:Dual-button chord detected during UP hold, entering dual-button handler..."
                        handle_dual_button_shutdown
                        break
                    fi

                    current_time=$(date +%s.%N)
                    elapsed=$(echo "$current_time - $press_start" | bc)

                    if (( $(echo "$elapsed >= $LONG_PRESS_THRESHOLD" | bc -l) )); then
                        handled=true
                        echo "[INFO]:Up button long press detected. Recording PRE audio..."
                        if [ "$IS_RECORDING" = false ]; then
                            record_audio "pre" "hold" "$GPIO_UP_LINE"
                        else
                            echo "[WARNING]:Ignored: Cannot record pre-audio while recording data."
                            while [ "$(gpioget $GPIO_UP_LINE)" -eq "$BTN_ACTIVE_LEVEL" ]; do sleep 0.1; done
                        fi
                        break
                    fi
                    sleep 0.05
                done

                if [ "$handled" = false ]; then
                    echo "[INFO]:Up button single click detected."
                    if [ "$IS_RECORDING" = false ]; then
                        NEXT_RESET_SOURCE_EPISODE_DIR=""
                        start_recording
                    else
                        stop_recording
                    fi
                fi

                sleep 0.2
                echo "[INFO]:Waiting for next command..."
                continue
            fi
        fi

        if [ "$DOWN_BUTTON_AVAILABLE" = true ] && [ -n "$GPIO_DOWN_LINE" ]; then
            if [ "$(gpioget $GPIO_DOWN_LINE)" -eq "$BTN_ACTIVE_LEVEL" ]; then
                sleep $DEBOUNCE_MS

                if [ "$(gpioget $GPIO_DOWN_LINE)" -eq "$BTN_ACTIVE_LEVEL" ]; then
                    # 给双键组合留一个短窗口，避免双按时被误判为下键长按音频
                    sleep "$DUAL_CHORD_WINDOW"
                    if [ "$(gpioget $GPIO_UP_LINE)" -eq "$BTN_ACTIVE_LEVEL" ] && \
                       [ "$(gpioget $GPIO_DOWN_LINE)" -eq "$BTN_ACTIVE_LEVEL" ]; then
                        echo "[INFO]:Dual-button chord detected while DOWN pending, entering dual-button handler..."
                        handle_dual_button_shutdown
                        continue
                    fi

                    press_start=$(date +%s.%N)
                    down_long=false

                    while [ "$(gpioget $GPIO_DOWN_LINE)" -eq "$BTN_ACTIVE_LEVEL" ]; do
                        if [ "$(gpioget $GPIO_UP_LINE)" -eq "$BTN_ACTIVE_LEVEL" ]; then
                            down_long=true
                            echo "[INFO]:Dual-button chord detected during DOWN hold, entering dual-button handler..."
                            handle_dual_button_shutdown
                            break
                        fi

                        current_time=$(date +%s.%N)
                        elapsed=$(echo "$current_time - $press_start" | bc)

                        if (( $(echo "$elapsed >= $LONG_PRESS_THRESHOLD" | bc -l) )); then
                            down_long=true
                            echo "[INFO]:Down button long press detected. Recording POST audio..."
                            if [ "$IS_RECORDING" = false ]; then
                                record_audio "post" "hold" "$GPIO_DOWN_LINE"
                            else
                                echo "[WARNING]:Ignored: Cannot record post-audio while recording data."
                                while [ "$(gpioget $GPIO_DOWN_LINE)" -eq "$BTN_ACTIVE_LEVEL" ]; do sleep 0.1; done
                            fi
                            break
                        fi
                        sleep 0.05
                    done

                    if [ "$down_long" = false ]; then
                        echo "[INFO]:Down button short press detected."
                        while [ "$(gpioget $GPIO_DOWN_LINE)" -eq "$BTN_ACTIVE_LEVEL" ]; do sleep 0.05; done

                        if [ "$IS_RECORDING" = false ]; then
                            if [ -n "$LAST_EPISODE_DIR" ] && [ -d "$LAST_EPISODE_DIR" ]; then
                                NEXT_RESET_SOURCE_EPISODE_DIR="$LAST_EPISODE_DIR"
                                echo "[INFO]:Trigger: Start RESET recording from LAST_EPISODE_DIR=$LAST_EPISODE_DIR"
                                start_recording
                            else
                                echo "[INFO]:No LAST_EPISODE_DIR found, reset recording not needed."
                                notify_audio "no_reset_needed"
                            fi
                        else
                            stop_recording
                        fi
                    fi

                    sleep 0.2
                    echo "[INFO]:Waiting for next command..."
                    continue
                fi
            fi
        fi

        sleep $DEBOUNCE_MS
    done
fi
