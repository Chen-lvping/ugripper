#!/bin/bash

# ================= 脚本初始化 =================
script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$script_dir" || exit 1

# ================= 配置部分 =================
CONFIG_FILE="./config/config.txt"
DISK_DIR="/mnt/data_disk"
DATA_ROOT="/mnt/data_disk/raw_data" 

# --- LED 控制配置 ---
LED_SCRIPT="./led_manager.py"
LED_PIPE="/tmp/umi_led_pipe"

# --- 音频配置---
AUDIO_RECORD_SCRIPT="./audio/audio_record.py"
AUDIO_PLAY_SCRIPT="./audio/audio_play.py"
AUDIO_TEMP_DIR="/tmp/umi_audio"
AUDIO_PIPE="/tmp/umi_audio_pipe"

# --- GPIO 配置 ---
PIN_HIGH="PIN_32"   # 3.3V 输出
PIN_BTN="PIN_36"    # 按钮输入
BTN_ACTIVE_LEVEL=1  # 1表示按下
DEBOUNCE_MS=0.03    # 30ms

# 长按检测阈值（秒）
LONG_PRESS_THRESHOLD=1.0

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

# 播放准备就绪提示音
notify_audio "ready"

# ================= 业务配置检查 =================

# 1. 读取 Device ID
if [ -f "$CONFIG_FILE" ]; then
    source "$CONFIG_FILE"
else
    echo "Error: Configuration file $CONFIG_FILE not found!"
    set_state "ERROR"
    notify_audio "error"
    exit 1
fi

if [ -z "$device_id" ]; then
    echo "Error: device_id not defined in config file."
    set_state "ERROR"
    notify_audio "error"
    exit 1
fi

DEVICE_MODEL=$(echo "$device_id" | awk -F_ '{print $1}')
DEVICE_NUM=$(echo "$device_id" | awk -F_ '{print $2}')
COLLECTOR=${collector:-"default_user"}

# 2. 检查硬盘挂载
if ! mountpoint -q "$DISK_DIR"; then
    echo "Error: $DISK_DIR is NOT mounted!"
    set_state "ERROR"
    notify_audio "error"
    exit 1
else
    echo "Disk OK: $DISK_DIR is mounted."
fi

# ================= 目录结构与元数据 =================
echo "Initializing Data Structure..."

DIR_META="$DATA_ROOT/metadata"
DIR_CALIB="$DATA_ROOT/calibration"
DIR_DATA="$DATA_ROOT/data"

mkdir -p "$DIR_META"
mkdir -p "$DIR_CALIB"
mkdir -p "$DIR_DATA"

# 1. 生成 metadata
META_FILE="$DIR_META/info.txt"
if [ ! -f "$META_FILE" ]; then
    echo "device_type: UMI" > "$META_FILE"
    echo "device_model: $DEVICE_MODEL" >> "$META_FILE"
    echo "device_id: $DEVICE_NUM" >> "$META_FILE"
    echo "collector: $COLLECTOR" >> "$META_FILE"
    echo "Metadata generated."
fi

# 2. 拷贝 calibration 文件
# 从 ./config/fake*Calib.yaml 拷贝到 calibration/xxx.yaml
if [ -f "./config/fakeCamCalib.yaml" ]; then
    cp "./config/fakeCamCalib.yaml" "$DIR_CALIB/cam.yaml"
fi
if [ -f "./config/fakeEncoderCalib.yaml" ]; then
    cp "./config/fakeEncoderCalib.yaml" "$DIR_CALIB/encoder.yaml"
fi
if [ -f "./config/fakeIMUCalib.yaml" ]; then
    cp "./config/fakeIMUCalib.yaml" "$DIR_CALIB/imu.yaml"
fi
echo "Calibration files synced."

# ================= 硬件序列号校验 =================
check_camera_hardware() {
    local dev_node=$1
    local name=$2
    local yaml_file="$DIR_CALIB/cam.yaml"
    
    echo "Checking $name ($dev_node)..."
    
    if [ ! -e "$dev_node" ]; then
        echo "WARNING: Device $dev_node not found!"
        return
    fi

    # 使用 udevadm 查找父级 USB 设备的 serial
    # 逻辑：查找SUBSYSTEMS=="usb" 且 DRIVERS=="usb" 下的 ATTRS{serial}
    # 注意：udevadm 输出是层级的，我们取第一个匹配到的 USB serial
    local usb_serial=$(udevadm info --attribute-walk --name="$dev_node" | \
                       grep -Pzo "(?s)SUBSYSTEMS==\"usb\".*?DRIVERS==\"usb\".*?ATTRS{serial}==\".*?\"" | \
                       grep "ATTRS{serial}" | head -n 1 | awk -F'"' '{print $2}')

    if [ -z "$usb_serial" ]; then
        echo "WARNING: Could not read USB serial for $dev_node"
        return
    fi

    # 读取 cam.yaml 中的序列号
    # 暂时直接读取 yaml 里是否有该序列号字符串
    
    local yaml_serial_match=$(grep "$usb_serial" "$yaml_file")
    
    # 获取 yaml 里对应的预期 serial (这里简化处理，需根据实际yaml结构完善)
    # 假设我们只检查 yaml 里是否存在这个序列号
    if [ -z "$yaml_serial_match" ]; then
        echo "WARNING: Serial $usb_serial for $dev_node NOT FOUND in $yaml_file!"
    else
        echo "  - Serial match OK: $usb_serial"
    fi
}

# 执行校验
check_camera_hardware "/dev/left_tcam" "Left Tactile"
check_camera_hardware "/dev/right_tcam" "Right Tactile"

# ================= GPIO 初始化 =================
echo "Initializing GPIO..."

# 获取引脚的控制器和偏移量
if [ -z "$(gpiofind "$PIN_HIGH")" ] || [ -z "$(gpiofind "$PIN_BTN")" ]; then
    echo "Error: Could not find GPIO pins."
    set_state "ERROR"
    notify_audio "error"
    exit 1
fi

# 设置输出高电平
gpioset -m signal $(gpiofind "$PIN_HIGH")=1 &
PID_GPIO_HIGH=$!
echo "GPIO $PIN_HIGH set to HIGH (PID: $PID_GPIO_HIGH)"

# ================= 全局变量 =================
IS_RECORDING=false
PID_CAM=""
PID_ENC=""
PID_IMU=""
TARGET_DIR=""
LAST_EPISODE_DIR=""  # 记录上次录制的目录（用于post音频）
PRE_AUDIO_FILE=""    # 存储预录制音频文件路径

# ================= 函数定义 =================

# 函数：计算新路径并创建文件夹
prepare_directory() {
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
    TARGET_DIR="${DIR_DATA}/episode_${date_str}_${id_str}"
    mkdir -p "$TARGET_DIR"

    echo "New recording session: $TARGET_DIR"
}

# 函数：启动音频录制
record_audio() {
    local audio_type=$1  # "pre" 或 "post"
    local timestamp=$(date +%Y%m%d_%H%M%S)
    local temp_file="$AUDIO_TEMP_DIR/audio_${audio_type}_${timestamp}.wav"
    
    echo "Starting $audio_type audio recording..."
    
    # 播放开始录制提示音
    notify_audio "audio_recording_start"
    
    # 开始录制
    arecord -D hw:1,0 -f cd -r 48000 -c 2 -t wav "$temp_file.raw" 2>/dev/null &
    local arecord_pid=$!
    
    echo "Recording... (Hold button, release to stop)"
    
    # 循环检测按钮状态，直到松开
    while kill -0 $arecord_pid 2>/dev/null; do
        # 检查按钮是否松开
        if [ "$(gpioget $(gpiofind "$PIN_BTN"))" -ne "$BTN_ACTIVE_LEVEL" ]; then
            # 按钮松开，停止录音
            kill -SIGINT $arecord_pid 2>/dev/null
            break
        fi
        sleep 0.05  # 降低CPU占用
    done
    
    # 等待录音进程完全结束
    wait $arecord_pid 2>/dev/null
    
    # 降噪处理
    if [ -f "$temp_file.raw" ]; then
        sox "$temp_file.raw" "$temp_file" noisered "./audio/noise.prof" 0.15 remix 2 2 norm 2>/dev/null
        rm -f "$temp_file.raw"
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
start_recording() {
    prepare_directory
    
    # 如果存在预录制音频，移动到当前episode
    if [ -n "$PRE_AUDIO_FILE" ] && [ -f "$PRE_AUDIO_FILE" ]; then
        mv "$PRE_AUDIO_FILE" "$TARGET_DIR/audio_pre.wav"
        echo "Pre-audio moved to episode: $TARGET_DIR/audio_pre.wav"
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
    
    # === 切换状态灯：录制中 (红闪) ===
    set_state "RECORDING"
    notify_audio "recording_start"
    
    echo ">>> RECORDING STARTED [ PIDs: Cam=$PID_CAM Enc=$PID_ENC Imu=$PID_IMU ]"
}

# 函数：停止所有录制进程
stop_recording() {
    echo "Stopping processes..."

    # 发送 SIGINT (Ctrl+C) 信号
    if [ -n "$PID_CAM" ] && kill -0 $PID_CAM 2>/dev/null; then kill -2 $PID_CAM; fi
    if [ -n "$PID_ENC" ] && kill -0 $PID_ENC 2>/dev/null; then kill -2 $PID_ENC; fi
    if [ -n "$PID_IMU" ] && kill -0 $PID_IMU 2>/dev/null; then kill -2 $PID_IMU; fi
    
    # 等待退出
    wait $PID_CAM $PID_ENC $PID_IMU 2>/dev/null
    
    IS_RECORDING=false
    
    # === 切换状态灯：待机 (绿呼吸) ===
    set_state "READY"
    notify_audio "recording_stop"
    
    echo ">>> RECORDING STOPPED. Saved to $TARGET_DIR"
    
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

    # 2. 释放 GPIO
    if [ -n "$PID_GPIO_HIGH" ]; then
        kill $PID_GPIO_HIGH 2>/dev/null
    fi

    # 3. 关闭 LED (发送退出指令)
    set_state "EXIT"
    
    # 4. 停止音频播放
    notify_audio "exit"
    
    # 5. 确保 Python 脚本退出
    if [ -n "$PID_LED_SCRIPT" ]; then
        # 给它一点时间处理 EXIT 命令
        sleep 0.2
        kill $PID_LED_SCRIPT 2>/dev/null
    fi
    
    if [ -n "$PID_AUDIO_PLAY" ]; then
        sleep 0.2
        kill $PID_AUDIO_PLAY 2>/dev/null
    fi
    
    # 6. 清理临时文件
    rm -rf "$AUDIO_TEMP_DIR"
    rm -f "$LED_PIPE" "$AUDIO_PIPE"

    echo "Cleanup done."
    exit 0
}

# 捕获脚本自身的退出信号
trap cleanup SIGINT SIGTERM

# ================= 主循环 =================

# 初始化全部完成后，设为 READY (绿灯呼吸)
set_state "READY"

echo "=========================================="
echo "System Ready. Press button to Start/Stop."
echo "Long press for audio recording (pre/post)."
echo "=========================================="

while true; do
    # 读取按钮电平 (输出 0 或 1)
    BTN_VAL=$(gpioget $(gpiofind "$PIN_BTN"))
    
    # 检查是否按下 (根据 BTN_ACTIVE_LEVEL 判断)
    if [ "$BTN_VAL" -eq "$BTN_ACTIVE_LEVEL" ]; then
    echo "test"
        
        # 1. 检测到触发，先去抖 (睡眠)
        sleep $DEBOUNCE_MS
        
        # 2. 再次读取确认
        BTN_VAL_CHECK=$(gpioget $(gpiofind "$PIN_BTN"))
        
        if [ "$BTN_VAL_CHECK" -eq "$BTN_ACTIVE_LEVEL" ]; then
            # === 检测长按 ===
            press_start=$(date +%s.%N)
            is_long_press=false
            
            # 等待达到长按阈值或按钮释放
            while [ "$(gpioget $(gpiofind "$PIN_BTN"))" -eq "$BTN_ACTIVE_LEVEL" ]; do
                current_time=$(date +%s.%N)
                elapsed=$(echo "$current_time - $press_start" | bc)
                
                # 如果长按超过阈值
                if (( $(echo "$elapsed >= $LONG_PRESS_THRESHOLD" | bc -l) )); then
                    is_long_press=true
                    
                    # 长按确认：立即开始录音（record_audio会阻塞直到松开）
                    echo "Long press detected. Starting audio recording..."
                    
                    # 判断是pre还是post录制
                    if [ "$IS_RECORDING" = false ]; then
                        # 录制pre音频（为下次录制准备）
                        record_audio "pre"
                    else
                        # 录制post音频（为当前录制追加）
                        record_audio "post"
                    fi
                    
                    echo "Audio recording complete."
                    break
                fi
                sleep 0.05
            done
            
            # 如果不是长按（按钮在阈值内释放），则执行短按操作
            if [ "$is_long_press" = false ]; then
                # === 短按：录制控制 ===
                echo "Short press detected. Toggling recording state..."
                
                if [ "$IS_RECORDING" = false ]; then
                    start_recording
                else
                    stop_recording
                fi
            fi
            
            # 3. 确保按钮完全释放
            while [ "$(gpioget $(gpiofind "$PIN_BTN"))" -eq "$BTN_ACTIVE_LEVEL" ]; do
                sleep 0.1
            done
            
            # 短延时，防止误触发
            sleep 0.3
            
            echo "Waiting for next command..."
        fi
    fi
    
    # 循环延时，降低 CPU 占用
    sleep $DEBOUNCE_MS
done
