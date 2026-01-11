#!/bin/bash

# ================= 脚本初始化 =================
script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$script_dir" || exit 1

# ================= 配置部分 =================
DISK_DIR="/mnt/data_disk"
DEVICE_SN=""
if [ -f /etc/environment ]; then
    DEVICE_SN=$(grep -E '^DEVICE_SN=' /etc/environment \
        | head -n1 \
        | cut -d= -f2- \
        | tr -d '"' \
        | xargs)
fi
DATA_ROOT="/mnt/data_disk/${DEVICE_SN:-NonameDevice}" 

# --- 网络配置 (双臂协同) ---
# 通过环境变量/etc/environment获取当前设备角色，默认为 Right (Master)
DEVICE_SIDE=""

if [ -f /etc/environment ]; then
    DEVICE_SIDE=$(grep -E '^DEVICE_SIDE=' /etc/environment \
        | head -n1 \
        | cut -d= -f2- \
        | tr -d '"' \
        | xargs)
fi

CURRENT_SIDE=${DEVICE_SIDE:-Right}

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

# --- GPIO 配置 ---
PIN_BTN="PIN_36"    # 按钮输入
BTN_ACTIVE_LEVEL=1  # 1表示按下
DEBOUNCE_MS=0.03    # 30ms

# 长按检测阈值（秒）
LONG_PRESS_THRESHOLD=1.0
# 双击检测窗口（秒）
DOUBLE_CLICK_THRESHOLD=0.4

# ================= 状态机与 LED 通信模块 =================

# 1. 创建命名管道 (如果不存在)
if [ ! -p "$LED_PIPE" ]; then
    mkfifo "$LED_PIPE"
fi

# 2. 启动 Python LED 管理器 (后台运行)
if [ -f "$LED_SCRIPT" ]; then
    echo "Starting LED Manager..."
    uv run "$LED_SCRIPT" &
    PID_LED_SCRIPT=$!
    # 给 Python 一点时间初始化
    sleep 0.2
else
    echo "Warning: LED script not found at $LED_SCRIPT"
fi

# 3. 定义发送状态的函数
# 可选状态: INIT (蓝), READY (绿呼吸), RECORDING (红闪), ERROR (红快闪), EXIT (关)
set_state() {
    local state=$1
    # 仅当管道存在时写入，& 放入后台防止阻塞 Bash
    if [ -p "$LED_PIPE" ]; then
        echo "$state" > "$LED_PIPE" &
    fi
}

# 音频状态通知函数
notify_audio() {
    local action=$1
    if [ -p "$AUDIO_PIPE" ]; then
        echo "$action" > "$AUDIO_PIPE" &
    fi
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
    echo "Starting Audio Play Manager..."
    uv run "$AUDIO_PLAY_SCRIPT" &
    PID_AUDIO_PLAY=$!
    sleep 0.2
else
    echo "Warning: Audio play script not found at $AUDIO_PLAY_SCRIPT"
fi

# 创建临时音频目录
mkdir -p "$AUDIO_TEMP_DIR"

# ================= 业务配置检查 =================
# 检查硬盘挂载 (改为循环等待模式)
echo "Checking Disk Mount..."
while ! mountpoint -q "$DISK_DIR"; do
    echo "Error: $DISK_DIR is NOT mounted! Waiting for disk..."
    
    # 设置为错误状态 (红灯快闪)
    set_state "ERROR"
    notify_audio "error"
    
    # 等待 3 秒再次检查
    sleep 3
done

echo "Disk OK: $DISK_DIR is mounted."

# ================= 日志系统 =================
# 1. 定义路径
LOG_FILE_LOCAL="/tmp/umi_sys_${CURRENT_SIDE}_$(date +%Y%m%d).log"
LOG_FILE_DISK="$DISK_DIR/logs/umi_sys_${CURRENT_SIDE}_$(date +%Y%m%d).log"

# 2. 重定向到【本地临时文件】
echo "Logging locally to: $LOG_FILE_LOCAL"
exec > >(tee -a "$LOG_FILE_LOCAL") 2>&1

# 3. 定义后台同步函数
# 作用：定期检查硬盘是否在，如果在，就把新增日志搬运过去
sync_logs_to_disk() {
    local src="$1"
    local dest="$2"
    local last_pos=0
    
    while true; do
        # 仅当源文件存在且有大小变化时才尝试写入
        if [ -f "$src" ]; then
            local curr_size=$(stat -c%s "$src" 2>/dev/null || echo 0)
            
            if [ "$curr_size" -gt "$last_pos" ]; then
                # 关键：检查挂载点是否存活，避免往 broken pipe 写
                if mountpoint -q "$DISK_DIR"; then
                    # 确保目标目录存在
                    mkdir -p "$(dirname "$dest")" 2>/dev/null
                    
                    # 计算增量并追加到硬盘 (tail -c +N 从第N个字节开始输出)
                    # 只追加新内容，写完立即释放文件句柄
                    tail -c +$((last_pos + 1)) "$src" >> "$dest" 2>/dev/null
                    
                    # 只有写入成功才更新游标，防止数据丢失
                    if [ $? -eq 0 ]; then
                        last_pos=$curr_size
                    fi
                fi
            fi
        fi
        # 每 5 秒同步一次，降低 IO 压力
        sleep 5
    done
}

# 4. 启动后台同步进程并记录 PID
sync_logs_to_disk "$LOG_FILE_LOCAL" "$LOG_FILE_DISK" &
PID_LOG_SYNC=$!


# 硬盘检查通过，恢复为初始化状态(蓝灯)，准备后续硬件检查
set_state "INIT"


# ================= 目录结构与元数据 =================
echo "Initializing Data Structure..."

DIR_META="$DATA_ROOT/metadata"
DIR_CALIB="$DATA_ROOT/calibration"
DIR_DATA="$DATA_ROOT/data"

mkdir -p "$DIR_META"
mkdir -p "$DIR_CALIB"
mkdir -p "$DIR_DATA"

# 1. 生成 metadata
META_FILE="$DIR_META/info.json"
if [ ! -f "$META_FILE" ]; then
    # 使用 cat 生成 JSON 内容
    # 注意：DEVICE_SN 来源于脚本开头的 /etc/environment 读取
    cat <<EOF > "$META_FILE"
{
    "device_type": "UMI",
    "device_model": "ugripper",
    "device_id": "${DEVICE_SN}",
    "collector": "default_user",
    "device_side": "${CURRENT_SIDE}",
    "data_path": "data/episode_{date:08d}_{episode_index:04d}"
}
EOF
    echo "Metadata info.json created."
fi

# 2. 拷贝 calibration 文件
# 从 ./config/fake*Calib.json 拷贝到 calibration/xxx.json
if [ -f "./config/fakeCamCalib.json" ]; then
    cp -n "./config/fakeCamCalib.json" "$DIR_CALIB/cam.json"
fi
if [ -f "./config/fakeEncoderCalib.json" ]; then
    cp -n "./config/fakeEncoderCalib.json" "$DIR_CALIB/encoder.json"
fi
if [ -f "./config/fakeIMUCalib.json" ]; then
    cp -n "./config/fakeIMUCalib.json" "$DIR_CALIB/imu.json"
fi
echo "Calibration files synced."

# ================= 全局占位符处理 (CAM_MAIN) =================
handle_global_placeholders() {
    local json_file="$DIR_CALIB/cam.json"
    
    if [ ! -f "$json_file" ]; then return; fi

    # 1. 初始化 {{CAM_MAIN}}
    # 根据环境变量 CURRENT_SIDE (Left/Right) 决定 cam_left 或 cam_right
    if grep -q "{{CAM_MAIN}}" "$json_file"; then
        local cam_name=""
        # ${CURRENT_SIDE,,} 将变量转为小写 (需要 Bash 4.0+)
        if [[ "${CURRENT_SIDE,,}" == "left" ]]; then
            cam_name="cam_left"
        elif [[ "${CURRENT_SIDE,,}" == "right" ]]; then
            cam_name="cam_right"
        else
            echo "WARNING: CURRENT_SIDE='$CURRENT_SIDE' is invalid. Skipping {{CAM_MAIN}} init."
        fi

        if [ -n "$cam_name" ]; then
            echo "Initializing {{CAM_MAIN}} key to $cam_name in $json_file..."
            # 直接使用 sed 替换 key 字符串
            sed -i "s/{{CAM_MAIN}}/$cam_name/g" "$json_file"
        fi
    fi
}

# 立即执行一次全局占位符处理
handle_global_placeholders

# ================= 硬件序列号校验与初始化 =================
check_camera_hardware() {
    local dev_node=$1
    local name=$2
    local json_file="$DIR_CALIB/cam.json"
    
    echo "Checking $name ($dev_node)..."
    
    if [ ! -e "$dev_node" ]; then
        echo "WARNING: Device $dev_node not found!"
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
        echo "WARNING: Could not read USB serial for $dev_node"
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
        echo "WARNING: Could not determine side from device node $dev_node, skipping check."
        return
    fi
    
    # 构造占位符字符串，例如 {{TACTILE_LEFT_SERIAL}}
    local placeholder="{{TACTILE_${side_upper}_SERIAL}}"

    # 检查 jq
    if ! command -v jq &> /dev/null; then
        echo "ERROR: 'jq' command not found. Please install jq."
        return
    fi

    if [ ! -f "$json_file" ]; then
        echo "WARNING: $json_file not found."
        return
    fi

    # --- 逻辑分支 ---
    
    # 1. 检查是否存在占位符（初始化模式）
    if grep -q "$placeholder" "$json_file"; then
        echo "  - Found placeholder $placeholder. Initializing to $usb_serial..."
        # 使用 sed 直接替换占位符（最安全的方式，无需关心 JSON 深度）
        sed -i "s/$placeholder/$usb_serial/g" "$json_file"
        echo "  - Initialization complete."
        return
    fi

    # 2. 占位符不存在，进行校验（Verify 模式）
    # 构造 JSON 查询路径：.observation.tactile.gripper_left_tactile.serial
    # 注意：这里的 gripper_${side}_tactile 必须匹配 JSON 中的实际 key 名
    local json_path=".observation.tactile.gripper_${side}_tactile.serial"
    
    local serial_in_json=$(jq -r "$json_path // empty" "$json_file")
    
    if [ -z "$serial_in_json" ]; then
        echo "WARNING: Could not find serial at '$json_path' in $json_file."
    elif [ "$serial_in_json" != "$usb_serial" ]; then
        # 序列号不匹配 -> 仅警告
        echo "WARNING: Serial mismatch for $side side!"
        echo "  - Configured (JSON): $serial_in_json"
        echo "  - Detected (HW)    : $usb_serial"
        echo "  - ACTION: Keeping existing configuration (Manual intervention required if hardware changed)."
    else
        echo "  - Serial match OK: $usb_serial"
    fi
}



# 执行校验
check_camera_hardware "/dev/left_tcam" "Left Tactile"
check_camera_hardware "/dev/right_tcam" "Right Tactile"

# ================= GPIO 初始化 (仅 Right 需要) =================
if [ "$CURRENT_SIDE" == "Right" ]; then
    echo "Initializing GPIO for Master (Right)..."
    if [ -z "$(gpiofind "$PIN_BTN")" ]; then
        echo "Error: Could not find GPIO pins."
        set_state "ERROR"; notify_audio "error"; exit 1
    fi
else
    echo "GPIO initialization skipped for Slave (Left)."
fi

# ================= 全局变量 =================
IS_RECORDING=false
PID_CAM=""
PID_ENC=""
PID_IMU=""
TARGET_DIR=""
LAST_EPISODE_DIR=""  # 记录上次录制的目录（用于post音频）
PRE_AUDIO_FILE=""    # 存储预录制音频文件路径

# ================= 函数定义 =================

# 硬件健康监测函数
monitor_system_health() {
    # 如果正在录制，不要执行检查，以免争抢 IO 导致丢帧
    if [ "$IS_RECORDING" = true ]; then return; fi

    local has_error=false
    local error_msg=""

    # 1. 检查硬盘
    if ! mountpoint -q "$DISK_DIR"; then
        has_error=true
        error_msg="${error_msg} Disk not mounted;"
    fi

    # 2. 检查相机节点
    if [ ! -e "/dev/right_tcam" ]; then
        has_error=true
        error_msg="${error_msg} Right Cam lost"
    fi
    if [ ! -e "/dev/left_tcam" ]; then
        has_error=true
        error_msg="${error_msg} Left Cam lost"
    fi

    # 3. 状态切换处理
    if [ "$has_error" = true ]; then
        # 仅当状态改变时才触发报警，防止日志刷屏
        if [ "$SYSTEM_HEALTH_STATUS" != "ERROR" ]; then
            echo "[$(date)] MONITOR ERROR: $error_msg"
            set_state "ERROR"
            notify_audio "error"
            SYSTEM_HEALTH_STATUS="ERROR"
        fi
    else
        # 如果之前是 ERROR，现在恢复了
        if [ "$SYSTEM_HEALTH_STATUS" == "ERROR" ]; then
            echo "[$(date)] MONITOR RECOVERED: System is back online."
            set_state "READY"
            notify_audio "ready"
            SYSTEM_HEALTH_STATUS="OK"
        fi
    fi
}

# 函数：发送网络命令 (仅 Right 调用)
send_network_command() {
    local cmd=$1
    local arg=$2
    # 使用 netcat 发送 UDP 包或者 TCP 连接，这里使用 TCP 并设置超时
    # 格式: COMMAND|ARGUMENT
    echo "${cmd}|${arg}" | nc -w 1 "$IP_LEFT" "$SYNC_PORT" 2>/dev/null
    echo "Sent to Left: ${cmd}|${arg}"
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

    echo "New recording session: $TARGET_DIR"
}

# 函数：启动音频录制
record_audio() {
    # 如果是 Left (Slave)，直接禁用录音功能
    if [ "$CURRENT_SIDE" == "Left" ]; then
        echo "Audio recording disabled on Slave (Left) side."
        return
    fi

    local audio_type=$1
    local mode=${2:-"hold"}
    local timestamp=$(date +%Y%m%d_%H%M%S)
    local temp_file="$AUDIO_TEMP_DIR/audio_${audio_type}_${timestamp}.wav"
    
    echo "Starting $audio_type audio recording (Mode: $mode)..."
    notify_audio "audio_recording_start"
    
    arecord -D hw:rockchipes8388,0 -f cd -r 44100 -c 2 -t wav "$temp_file.raw" &
    local arecord_pid=$!
    
    # 按键检测逻辑 (仅在 Right 有效)
    if [ "$mode" = "hold" ]; then
        echo "Recording... (Release button to stop)"
        # Hold模式：循环直到按钮松开
        while kill -0 $arecord_pid 2>/dev/null; do
            if [ "$(gpioget $(gpiofind "$PIN_BTN"))" -ne "$BTN_ACTIVE_LEVEL" ]; then
                kill -SIGINT $arecord_pid 2>/dev/null
                break
            fi
            sleep 0.05
        done
    elif [ "$mode" = "latch" ]; then
        echo "Recording... (Press button again to stop)"
        # Latch模式：循环直到按钮再次按下
        # 首先等待按钮松开（防止误触）
        while [ "$(gpioget $(gpiofind "$PIN_BTN"))" -eq "$BTN_ACTIVE_LEVEL" ]; do sleep 0.05; done
        
        # 然后等待按钮按下
        while kill -0 $arecord_pid 2>/dev/null; do
            if [ "$(gpioget $(gpiofind "$PIN_BTN"))" -eq "$BTN_ACTIVE_LEVEL" ]; then
                # 按下后去抖，并停止
                sleep $DEBOUNCE_MS
                kill -SIGINT $arecord_pid 2>/dev/null
                break
            fi
            sleep 0.05
        done
        # 等待停止时的按键释放，避免退出后立即触发其他逻辑
        while [ "$(gpioget $(gpiofind "$PIN_BTN"))" -eq "$BTN_ACTIVE_LEVEL" ]; do sleep 0.05; done
    fi
    
    # 等待录音进程完全结束
    wait $arecord_pid 2>/dev/null
    
    # 降噪处理
    if [ -f "$temp_file.raw" ]; then
        sox "$temp_file.raw" "$temp_file" noisered "$script_dir/audio/noise.prof" 0.15 remix 2 2 norm 
        #rm -f "$temp_file.raw"
    else
        echo "Warning: No audio data recorded"
        return 1
    fi

    # 播放录制完成提示音
    notify_audio "audio_recording_stop"

    # 根据音频类型处理
    if [ "$audio_type" = "pre" ]; then
        PRE_AUDIO_FILE="$temp_file"
        echo "Pre-audio stored for next episode"
    elif [ "$audio_type" = "post" ]; then
        if [ -n "$LAST_EPISODE_DIR" ] && [ -d "$LAST_EPISODE_DIR" ]; then
            mv "$temp_file" "$LAST_EPISODE_DIR/audio_post.wav"
            echo "Post-audio moved to last episode: $LAST_EPISODE_DIR"
        else
            echo "Warning: No previous episode found for post-audio"
            rm -f "$temp_file"
        fi
    fi
}

# 函数：启动所有录制进程
# 参数1 (可选): 强制指定的目录名 (用于 Slave)
start_recording() {
    local sync_dir_name=$1

    # 1. 准备目录
    prepare_directory "$sync_dir_name"
    
    # 2. 如果是 Master，需要通知 Slave
    if [ "$CURRENT_SIDE" == "Right" ]; then
        # 获取纯文件夹名
        local dirname=$(basename "$TARGET_DIR")
        # 发送 START 指令和文件夹名
        send_network_command "START" "$dirname"
    fi

    # 3. 处理预录制音频 (仅 Master)
    if [ "$CURRENT_SIDE" == "Right" ] && [ -n "$PRE_AUDIO_FILE" ] && [ -f "$PRE_AUDIO_FILE" ]; then
        mv "$PRE_AUDIO_FILE" "$TARGET_DIR/audio_pre.wav"
        PRE_AUDIO_FILE=""
    fi
    
    echo "Starting processes..."

    # 启动相机    
    uv run ./camera_record/triple_camera_record_h265.py --output-dir "$TARGET_DIR" &
    PID_CAM=$!

    # 启动 Encoder
    ./encoder_refactor/build/main "$TARGET_DIR" &
    PID_ENC=$!

    # 启动 IMU    
    ./dm_imu_alone/build/dm_imu "$TARGET_DIR" &
    PID_IMU=$!
    
    IS_RECORDING=true
    LAST_EPISODE_DIR="$TARGET_DIR"  # 更新上次录制目录
    
    set_state "RECORDING"
    notify_audio "recording_start"
    
    echo ">>> RECORDING STARTED [ PIDs: Cam=$PID_CAM Enc=$PID_ENC Imu=$PID_IMU ]"
}

# ================= 数据校验函数 =================
validate_recording() {
    local dir=$1
    local validation_pass=true
    local error_details=""

    echo "Validating data in: $dir"

    # --- 1. 检查 MKV 文件大小 ---
    # 检查 cam.mkv, tact_left.mkv, tact_right.mkv 是否存在且大小不为 0
    # 注意：根据实际生成的文件名可能需要调整，这里假设文件名如下
    local mkv_files=("cam.mkv" "tact_left.mkv" "tact_right.mkv")
    
    for fname in "${mkv_files[@]}"; do
        local fpath="$dir/$fname"
        # 检查文件是否存在
        if [ ! -f "$fpath" ]; then
             echo "Warning: $fname missing (might be optional based on config)."
        else
            local fsize=$(stat -c%s "$fpath" 2>/dev/null || echo 0)
            if [ "$fsize" -eq 0 ]; then
                validation_pass=false
                error_details="${error_details} Zero-byte file: $fname;"
                echo "FAIL: $fname is 0 bytes."
            fi
        fi
    done

    # --- 2. 检查 Encoder CSV 数据 (检查 65535) ---
    # 假设 encoder 生成的 csv 包含 "encoder" 字样或者就是唯一的 csv
    local csv_file=$(find "$dir" -name "*encoder*.csv" -o -name "*.csv" | head -n 1)
    
    if [ -f "$csv_file" ]; then
        # 统计第二列 (currentPosition) 等于 65535 的行数
        # awk 逻辑：以逗号分隔，如果$2是65535，计数器加1
        local bad_rows=$(awk -F, '$2 == 65535 {count++} END {print count+0}' "$csv_file")
        
        # 设定阈值，例如超过 50 行数据异常就报错（防止启动瞬间的一两帧干扰）
        local threshold=50
        
        if [ "$bad_rows" -gt "$threshold" ]; then
            validation_pass=false
            error_details="${error_details} Encoder Init Fail ($bad_rows rows of 65535);"
            echo "FAIL: Encoder CSV has $bad_rows rows of 65535 (Threshold: $threshold)."
        else
            echo "PASS: Encoder CSV check ok (Bad rows: $bad_rows)."
        fi
    else
        echo "Warning: No CSV file found to validate."
    fi

    # --- 3. 结果处理 ---
    if [ "$validation_pass" = false ]; then
        echo ">>> VALIDATION FAILED: $error_details"
        # 触发报错灯光和声音
        set_state "ERROR"
        notify_audio "validation_failed"
        
        # 将文件夹重命名，标记为坏数据
        local new_dir="${dir}_BAD"
        mv "$dir" "$new_dir"
        echo "Renamed $dir -> $new_dir"
        
        # 强制让灯光保持 Error 状态一小段时间，避免马上被 monitor 覆盖
        sleep 3
    else
        echo ">>> VALIDATION PASSED."
    fi
}

# 函数：停止所有录制进程
stop_recording() {
    # 1. 如果是 Master，先通知 Slave 停止
    if [ "$CURRENT_SIDE" == "Right" ]; then
        send_network_command "STOP" "0"
    fi

    echo "Stopping processes on $CURRENT_SIDE..."

    # 发送 SIGINT (Ctrl+C) 信号
    if [ -n "$PID_CAM" ] && kill -0 $PID_CAM 2>/dev/null; then kill -2 $PID_CAM; fi
    if [ -n "$PID_ENC" ] && kill -0 $PID_ENC 2>/dev/null; then kill -2 $PID_ENC; fi
    if [ -n "$PID_IMU" ] && kill -0 $PID_IMU 2>/dev/null; then kill -2 $PID_IMU; fi
    
    # 等待退出
    wait $PID_CAM $PID_ENC $PID_IMU 2>/dev/null

    # 强制同步数据到磁盘
    echo "Syncing data to disk..."
    set_state "INIT"  # 临时切换状态指示sync
    sync "$DISK_DIR"

    IS_RECORDING=false
    echo ">>> RECORDING STOPPED. Processes terminated."

    # 先默认回 READY，如果校验失败，校验函数会覆盖为 ERROR
    set_state "READY"
    notify_audio "recording_stop"

    # 再运行校验 (如果失败，它会把灯变红)
    if [ -d "$TARGET_DIR" ]; then
        validate_recording "$TARGET_DIR"
    fi

    # 清空 PID
    PID_CAM=""
    PID_ENC=""
    PID_IMU=""
}

cleanup() {
    echo ""
    echo "System exit requested."

    # 1. 停止录制业务
    if [ "$IS_RECORDING" = true ]; then
        stop_recording
    fi

    # 2. 停止音频播放
    # 优先停止音频，防止报错音一直响
    notify_audio "exit"
    if [ -n "$PID_AUDIO_PLAY" ]; then
        sleep 0.1
        kill $PID_AUDIO_PLAY 2>/dev/null
    fi

    # 3. 关闭/复原 LED 状态
    # 发送 EXIT 状态给 Python 脚本，令其熄灭灯光
    set_state "EXIT"
    
    # 4. 确保 Python 脚本退出
    if [ -n "$PID_LED_SCRIPT" ]; then
        # 给 Python 脚本 0.5秒 时间处理 "EXIT" 信号并关闭灯光硬件
        sleep 0.5
        kill $PID_LED_SCRIPT 2>/dev/null
    fi

    # 停止日志同步
    if [ -n "$PID_LOG_SYNC" ]; then
        kill $PID_LOG_SYNC 2>/dev/null
    fi
    
    # 5. 清理临时文件
    rm -rf "$AUDIO_TEMP_DIR"
    rm -f "$LED_PIPE" "$AUDIO_PIPE"

    echo "Cleanup done."
    exit 0
}

# 捕获 信号 和 退出(EXIT)
# 注意：增加 EXIT 捕获可以确保脚本因任何原因退出时都尝试关灯
trap cleanup SIGINT SIGTERM EXIT

# ================= 逻辑分流：Master (Right) vs Slave (Left) =================

set_state "READY"
# 播放准备就绪提示音
notify_audio "ready"


LAST_CHECK_TIME=$(date +%s)
CHECK_INTERVAL=1  # 检查间隔（秒）

if [ "$CURRENT_SIDE" == "Left" ]; then
    # ================= Slave (Left) 逻辑 =================
    echo "=========================================="
    echo "RUNNING AS SLAVE (LEFT)"
    echo " - Physical buttons DISABLED"
    echo " - Audio recording DISABLED"
    echo " - Waiting for commands from RIGHT ($IP_RIGHT)..."
    echo "=========================================="

    # 1. 网络检查
    echo "Checking connection to Master..."
    ping -c 1 -W 2 "$IP_RIGHT" > /dev/null
    if [ $? -eq 0 ]; then
        echo "Master ($IP_RIGHT) is reachable."
        notify_audio "ready"
    else
        echo "WARNING: Master ($IP_RIGHT) is NOT reachable."
        set_state "ERROR" # 亮红灯警告
    fi

    # 2. 网络监听循环
    while true; do
        # === 优先执行：硬件健康检查 ===
        # 将检查放在循环开头，确保每次循环都覆盖
        # 移除时间间隔判断，或者保持很短的间隔，确保拔盘即红灯
        monitor_system_health
        
        # === 网络监听 ===
        # 监听 TCP 端口，收到数据后退出 nc
        # 格式: START|episode_xxxx 或 STOP|0
        raw_msg=$(nc -l -p "$SYNC_PORT" -w 1)
        
        if [ -n "$raw_msg" ]; then
            cmd=$(echo "$raw_msg" | awk -F'|' '{print $1}')
            arg=$(echo "$raw_msg" | awk -F'|' '{print $2}')
            
            echo "Received Network Command: $cmd Args: $arg"

            case "$cmd" in
                "START")
                    if [ "$IS_RECORDING" = false ]; then
                        echo "Trigger: Start Recording (Sync Dir: $arg)"
                        start_recording "$arg"
                    else
                        echo "Ignored: Already recording."
                    fi
                    ;;
                "STOP")
                    if [ "$IS_RECORDING" = true ]; then
                        echo "Trigger: Stop Recording"
                        stop_recording
                    else
                        echo "Ignored: Not recording."
                    fi
                    ;;
                *)
                    echo "Unknown command: $cmd"
                    ;;
            esac
        fi
        
    done

else
    # ================= Master (Right) 逻辑 =================
    echo "=========================================="
    echo "RUNNING AS MASTER (RIGHT)"
    echo " - Click: Start/Stop Camera (Triggers Left)"
    echo " - Long Press: Record Pre-Audio"
    echo " - Double Click: Record Post-Audio"
    echo "=========================================="

    while true; do
        # 定时检查逻辑 (非阻塞)
        CURRENT_TIME=$(date +%s)
        if [ $((CURRENT_TIME - LAST_CHECK_TIME)) -ge $CHECK_INTERVAL ]; then
            monitor_system_health
            LAST_CHECK_TIME=$CURRENT_TIME
        fi
        
        BTN_VAL=$(gpioget $(gpiofind "$PIN_BTN"))
        
        if [ "$BTN_VAL" -eq "$BTN_ACTIVE_LEVEL" ]; then
            # 1. 物理去抖
            sleep $DEBOUNCE_MS
            
            # 2. 再次读取确认
            if [ "$(gpioget $(gpiofind "$PIN_BTN"))" -eq "$BTN_ACTIVE_LEVEL" ]; then
                press_start=$(date +%s.%N)
                is_long_press=false
                
                # === 阶段1：判断长按 ===
                while [ "$(gpioget $(gpiofind "$PIN_BTN"))" -eq "$BTN_ACTIVE_LEVEL" ]; do
                    current_time=$(date +%s.%N)
                    elapsed=$(echo "$current_time - $press_start" | bc)

                    # 如果超过长按阈值
                    if (( $(echo "$elapsed >= $LONG_PRESS_THRESHOLD" | bc -l) )); then
                        is_long_press=true
                        echo "Long press detected. Recording PRE audio..."
                    
                        # 只有不在录制状态才建议录制Pre音频，或者根据需求调整
                        if [ "$IS_RECORDING" = false ]; then
                            record_audio "pre" "hold"
                        else
                            echo "Ignored: Cannot record pre-audio while recording data."
                            # 等待释放
                            while [ "$(gpioget $(gpiofind "$PIN_BTN"))" -eq "$BTN_ACTIVE_LEVEL" ]; do sleep 0.1; done
                        fi
                        
                        break # 长按处理结束
                    fi
                    sleep 0.05
                done
                
                # === 阶段2：短按释放后的判断（单击 vs 双击）===
                if [ "$is_long_press" = false ]; then
                    # 按钮已经松开，现在等待是否有第二次按下
                    is_double_click=false
                    
                    # 在窗口期内轮询检查第二次按下
                    # Bash 循环大概模拟窗口时间，0.05s * 8 ≈ 0.4s
                    steps=$(echo "$DOUBLE_CLICK_THRESHOLD / 0.05" | bc)
                    for ((i=0; i<steps; i++)); do
                        sleep 0.05
                        if [ "$(gpioget $(gpiofind "$PIN_BTN"))" -eq "$BTN_ACTIVE_LEVEL" ]; then
                            is_double_click=true
                            break
                        fi
                    done
                    
                    if [ "$is_double_click" = true ]; then
                        # === 双击逻辑：录制 Post 音频 ===
                        echo "Double click detected. Recording POST audio..."
                        # 使用 latch 模式：再次点击停止
                        # 此时第二次点击尚未松开，record_audio 中的 latch 逻辑会先等待松开
                        if [ "$IS_RECORDING" = false ]; then
                             record_audio "post" "latch"
                        else
                            echo "Warning: Ignored double click while camera is recording."
                        fi
                    else
                        # === 单击逻辑：开始/停止 录像 ===
                        echo "Single click detected."
                        if [ "$IS_RECORDING" = false ]; then
                            start_recording
                        else
                            stop_recording
                        fi
                    fi
                fi
                
                # 短延时，防止连续误触发
                sleep 0.2

                echo "Waiting for next command..."
            fi
        fi
        
        # 循环延时，降低 CPU 占用
        sleep $DEBOUNCE_MS
    done
fi
