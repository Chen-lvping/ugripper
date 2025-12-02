#!/bin/bash

#获取脚本所在目录
script_dir = "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$script_dir" || exit 1

# ================= 配置部分 =================
CONFIG_FILE="./config/config.txt"
DISK_DIR="/mnt/data_disk"
DATA_ROOT="/mnt/data_disk/raw_data"
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

# ===========================================
# 检查硬盘是否挂载
# ===========================================

if ! mountpoint -q "$DISK_DIR"; then
    echo "Error: $DISK_DIR is NOT mounted! Please check disk or fstab."
    echo "当前挂载情况如下："
    df -h | grep -E '^Filesystem|/mnt'
    exit 1
else
    echo "Disk OK: $DISK_DIR is mounted."
fi

# 2. 获取当前日期
DATE_STR=$(date +%Y%m%d)

# 3. 搜索最大 ID 并递增
mkdir -p "$DATA_ROOT"

# 查找匹配 episode_YYYYMMDD_DEVICEID_XXXX 的文件夹
# 逻辑：列出目录 -> 过滤包含当前日期和设备ID的项 -> 提取最后一段(ID) -> 排序 -> 取最大值
LAST_ID=$(ls "$DATA_ROOT" 2>/dev/null | grep "episode_${DATE_STR}_${device_id}_" | awk -F_ '{print $NF}' | sort -n | tail -1)

# 如果没找到，从 0 开始；否则 +1
if [ -z "$LAST_ID" ]; then
    NEW_ID=0
else
    NEW_ID=$((10#$LAST_ID + 1)) # 10# 强制按十进制处理，防止0001被当成八进制
fi

# 格式化为 4 位数字 (例如 0001)
ID_STR=$(printf "%04d" "$NEW_ID")

# 4. 创建最终文件夹路径
TARGET_DIR="${DATA_ROOT}/episode_${DATE_STR}_${device_id}_${ID_STR}"
mkdir -p "$TARGET_DIR"

echo "=========================================="
echo "Recording to: $TARGET_DIR"
echo "=========================================="

# 5. 定义清理函数 (Trap)
# 当脚本收到 SIGINT (Ctrl+C) 时，向所有子进程发送 SIGINT，等待它们保存并退出
cleanup() {
    echo ""
    echo "Stopping all recordings..."
    
    # 向子进程发送 SIGINT (等同于在终端按 Ctrl+C)
    # 检查进程是否存在再杀，避免报错
    if kill -0 $PID_CAM 2>/dev/null; then kill -2 $PID_CAM; fi
    if kill -0 $PID_ENC 2>/dev/null; then kill -2 $PID_ENC; fi
    if kill -0 $PID_IMU 2>/dev/null; then kill -2 $PID_IMU; fi
    
    # 等待子进程完全退出
    wait $PID_CAM $PID_ENC $PID_IMU
    
    echo "All recordings stopped. Data saved in $TARGET_DIR"
    exit 0
}

# 捕获 SIGINT (Ctrl+C)
trap cleanup SIGINT

# 6. 启动程序并传递路径
# 启动相机 (后台运行 &)
echo "Starting Camera..."
uv run ./camera_record/triple_camera_record.py
PID_CAM=$!

# 启动 Encoder (后台运行 &)
echo "Starting Encoder..."
./encoder_refactor/build/main "$TARGET_DIR" &
PID_ENC=$!

# 启动 IMU (后台运行 &)
echo "Starting IMU..."
./dm_imu_alone/build/dm_imu "$TARGET_DIR" &
PID_IMU=$!

#echo "Recording started. Press Ctrl+C to stop and save."
#echo "PIDs: Camera=$PID_CAM, Encoder=$PID_ENC, IMU=$PID_IMU"

# 7. 等待 (挂起脚本，直到收到信号)
# wait 命令会等待所有后台进程，或者直到脚本被信号中断
wait

