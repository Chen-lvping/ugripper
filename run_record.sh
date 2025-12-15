#!/bin/bash

# ================= 脚本初始化 =================
script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$script_dir" || exit 1

# ================= 配置部分 =================
CONFIG_FILE="./config/config.txt"
DISK_DIR="/mnt/data_disk"
DATA_ROOT="/mnt/data_disk/raw_data"

# GPIO 配置 (请根据实际情况修改)
PIN_HIGH="PIN_32"   # 输出高电平,电源引脚不够了，用IO输出凑合一下
PIN_BTN="PIN_36"   # 按钮输入
BTN_ACTIVE_LEVEL=1 # 0表示按下(低电平有效/上拉)，1表示按下(高电平有效/下拉)
# 去抖参数
DEBOUNCE_MS=0.03 # 30ms 去抖
# ===========================================

# 1. 读取 Device ID
if [ -f "$CONFIG_FILE" ]; then
    source "$CONFIG_FILE"
else
    echo "Error: Configuration file $CONFIG_FILE not found!"
    exit 1
fi

if [ -z "$device_id" ]; then
    echo "Error: device_id not defined in config file."
    exit 1
fi

# 2. 检查硬盘挂载
if ! mountpoint -q "$DISK_DIR"; then
    echo "Error: $DISK_DIR is NOT mounted!"
    exit 1
else
    echo "Disk OK: $DISK_DIR is mounted."
fi

# ================= GPIO 初始化 =================
echo "Initializing GPIO..."

# 获取引脚的控制器和偏移量
if [ -z "$(gpiofind "$PIN_HIGH")" ] || [ -z "$(gpiofind "$PIN_BTN")" ]; then
    echo "Error: Could not find GPIO pins. Check gpiofind."
    exit 1
fi

# 设置 输出引脚 高电平 (后台运行以保持电平)
# 使用 -m signal (或 --mode=signal) 让 gpioset 等待信号而不立即退出，从而保持电平
gpioset -m signal $(gpiofind "$PIN_HIGH")=1 &
PID_GPIO_HIGH=$!
echo "GPIO $PIN_HIGH set to HIGH (PID: $PID_GPIO_HIGH)"

# ================= 全局变量 =================
IS_RECORDING=false
PID_CAM=""
PID_ENC=""
PID_IMU=""
TARGET_DIR=""

# ================= 函数定义 =================

# 函数：计算新路径并创建文件夹
prepare_directory() {
    local date_str=$(date +%Y%m%d)
    mkdir -p "$DATA_ROOT"
    
    # 搜索最大ID
    local last_id=$(ls "$DATA_ROOT" 2>/dev/null | grep "episode_${date_str}_${device_id}_" | awk -F_ '{print $NF}' | sort -n | tail -1)
    
    local new_id=0
    if [ -n "$last_id" ]; then
        new_id=$((10#$last_id + 1))
    fi
    
    local id_str=$(printf "%04d" "$new_id")
    TARGET_DIR="${DATA_ROOT}/episode_${date_str}_${device_id}_${id_str}"
    mkdir -p "$TARGET_DIR"
    
    echo "New recording session: $TARGET_DIR"
}

# 函数：启动所有录制进程
start_recording() {
    prepare_directory
    
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
    echo ">>> RECORDING STOPPED. Saved to $TARGET_DIR"
    
    # 重置 PID
    PID_CAM=""
    PID_ENC=""
    PID_IMU=""
}

# 函数：清理并退出 (Ctrl+C 触发)
cleanup() {
    echo ""
    echo "System exit requested."
    
    if [ "$IS_RECORDING" = true ]; then
        stop_recording
    fi
    
    # 关闭 输出置高 (杀掉 gpioset 进程，电平通常会恢复默认或输入态，视硬件而定)
    if [ -n "$PID_GPIO_HIGH" ]; then
        kill $PID_GPIO_HIGH 2>/dev/null
        echo "GPIO HIGH released."
    fi
    
    exit 0
}

# 捕获脚本自身的退出信号
trap cleanup SIGINT SIGTERM

# ================= 主循环 (按钮扫描) =================

echo "=========================================="
echo "System Ready. Press button on $PIN_BTN to Start/Stop."
echo "=========================================="

while true; do
    # 读取按钮电平 (输出 0 或 1)
    BTN_VAL=$(gpioget $(gpiofind "$PIN_BTN"))
    
    # 检查是否按下 (根据 BTN_ACTIVE_LEVEL 判断)
    if [ "$BTN_VAL" -eq "$BTN_ACTIVE_LEVEL" ]; then
        
        # 1. 检测到触发，先去抖 (睡眠)
        # 注意：bash sleep 支持小数
        sleep $DEBOUNCE_MS
        
        # 2. 再次读取确认
        BTN_VAL_CHECK=$(gpioget $(gpiofind "$PIN_BTN"))
        
        if [ "$BTN_VAL_CHECK" -eq "$BTN_ACTIVE_LEVEL" ]; then
            # === 确认按下，执行状态切换 ===
            echo "Button pressed. Toggling recording state..."
            
            if [ "$IS_RECORDING" = false ]; then
                start_recording
            else
                stop_recording
            fi
            
            # 3. 等待按钮释放 (死循环直到松开)
            # 防止一直按着导致反复开关
            while [ "$(gpioget $(gpiofind "$PIN_BTN"))" -eq "$BTN_ACTIVE_LEVEL" ]; do
                sleep 0.1
            done
            
            echo "Button released. Waiting for next command..."
        fi
    fi
    
    # 循环延时，降低 CPU 占用
    sleep $DEBOUNCE_MS
done
