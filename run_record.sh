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
AUDIO_PLAY_SCRIPT="./audio/audio_play.py"
AUDIO_TEMP_DIR="/tmp/umi_audio"
AUDIO_PIPE="/tmp/umi_audio_pipe"

# --- GPIO 配置 ---
PIN_BTN="PIN_36"    # 按钮输入
BTN_ACTIVE_LEVEL=1  # 1表示按下
DEBOUNCE_MS=0.03    # 30ms

# 长按检测阈值（秒）
LONG_PRESS_THRESHOLD=1.0
# 双击检测窗口（秒）- 新增
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
if [ -z "$(gpiofind "$PIN_BTN")" ]; then
    echo "Error: Could not find GPIO pins."
    set_state "ERROR"
    notify_audio "error"
    exit 1
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
# 参数1: audio_type ("pre" 或 "post")
# 参数2: mode ("hold" 表示按住录音, "latch" 表示点击停止)
record_audio() {
    local audio_type=$1
    local mode=${2:-"hold"} # 默认为按住模式
    local timestamp=$(date +%Y%m%d_%H%M%S)
    local temp_file="$AUDIO_TEMP_DIR/audio_${audio_type}_${timestamp}.wav"
    
    echo "Starting $audio_type audio recording (Mode: $mode)..."
    
    notify_audio "audio_recording_start"
    
    arecord -D hw:rockchipes8388,0 -f cd -r 44100 -c 2 -t wav "$temp_file.raw" &
    local arecord_pid=$!
    
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

# 初始化全部完成后，设为 READY (黄绿灯呼吸)
set_state "READY"

echo "=========================================="
echo "System Ready."
echo " - Click: Start/Stop Camera"
echo " - Long Press: Record Pre-Audio (Hold)"
echo " - Double Click: Record Post-Audio (Latch)"
echo "=========================================="

while true; do
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
