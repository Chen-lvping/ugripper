#!/bin/bash

# ================= 脚本初始化 =================
TARGET_USER="radxa"

# 检查是否以 root 运行
if [ "$EUID" -ne 0 ]; then
  echo "Please run as root"
  exit 1
fi

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$script_dir/../" || exit 1

# ================= 配置部分 =================
ENCODER_CALIB_BIN="./build/src/sensor_recorder/zeroing"

# --- HMI 灯效配置 ---
HMI_HELPER_BIN="./build/src/gripper_hmi/gripper_hmi_test"
HMI_PORT_ARGS=(--port /dev/right_gripper --port /dev/left_gripper)

# --- 音频配置 ---
AUDIO_PLAY_SCRIPT="./audio/audio_play.py"
AUDIO_PIPE="/tmp/umi_audio_pipe"
AUDIO_PYTHON=""

# 全局变量存储 PID
PID_LED_SHELL=""
PID_AUDIO_SHELL=""

# --- 硬盘检测配置 ---
USB_LINK="/dev/usb_update_stick"
MOUNT_POINT="/mnt/usb_calib"

# 停止业务服务，防止占用
echo "Stopping ugripper.service..."
systemctl stop ugripper.service
sleep 3


# ================= 升级硬盘及文件检测 =================
echo "Checking USB trigger conditions..."

if [ ! -b "$USB_LINK" ]; then
    echo "Error: USB update stick ($USB_LINK) not found. Exiting."
    systemctl start ugripper.service
    exit 1
fi

mkdir -p "$MOUNT_POINT"
if ! mount -o ro "$USB_LINK" "$MOUNT_POINT" 2>/dev/null; then
    echo "Error: Failed to mount USB stick."
    systemctl start ugripper.service
    exit 1
fi

if [ ! -f "$MOUNT_POINT/calibration.txt" ]; then
    echo "Error: calibration.txt not found in USB root. Exiting."
    umount "$MOUNT_POINT" 2>/dev/null
    systemctl start ugripper.service    
    exit 1
fi

echo "USB Check Passed: calibration.txt detected."

# ================= 辅助函数 =================

run_as_user() {
    runuser -u "$TARGET_USER" -- bash -lc "$*"
}

resolve_audio_python() {
    if [ -x "./.venv/bin/python3" ]; then
        AUDIO_PYTHON="./.venv/bin/python3"
    elif command -v uv >/dev/null 2>&1; then
        AUDIO_PYTHON="uv run python3"
    else
        AUDIO_PYTHON="python3"
    fi
}

stop_led_helper() {
    if [ -n "$PID_LED_SHELL" ]; then
        kill_tree "$PID_LED_SHELL"
        PID_LED_SHELL=""
    fi
}

set_state() {
    local state=$1

    stop_led_helper

    if [ ! -x "$HMI_HELPER_BIN" ]; then
        echo "HMI helper not found: $HMI_HELPER_BIN"
        return 1
    fi

    if id "$TARGET_USER" >/dev/null 2>&1; then
        runuser -u "$TARGET_USER" -- "$HMI_HELPER_BIN" "${HMI_PORT_ARGS[@]}" --state "$state" --led-only --duration 0 >/dev/null 2>&1 &
    else
        "$HMI_HELPER_BIN" "${HMI_PORT_ARGS[@]}" --state "$state" --led-only --duration 0 >/dev/null 2>&1 &
    fi
    PID_LED_SHELL=$!
    sleep 0.2
}

notify_audio() {
    local action=$1
    if [ -p "$AUDIO_PIPE" ]; then
        echo "$action" > "$AUDIO_PIPE" &
    fi
}

# --- 递归杀死进程树函数 ---
kill_tree() {
    local _pid=$1
    
    if [ -z "$_pid" ]; then return; fi
    
    # 查找当前 PID 的所有直接子进程
    local _children=$(pgrep -P "$_pid")
    
    # 递归调用：先杀子进程
    for _child in $_children; do
        kill_tree "$_child" "$_sig"
    done
    
    # 最后杀当前进程
    # 检查进程是否存在以避免报错
    if kill -0 "$_pid" 2>/dev/null; then
        echo "Killing PID: $_pid"
        kill -9 "$_pid" 2>/dev/null
    fi
}

start_helpers() {
    echo "Starting helper processes..."
    
    rm -f "$AUDIO_PIPE"
    mkfifo "$AUDIO_PIPE"
    chmod 666 "$AUDIO_PIPE"

    if [ -x "$HMI_HELPER_BIN" ]; then
        set_state "INIT"
        echo "HMI LED helper started (PID: $PID_LED_SHELL)"
    fi

    if [ -f "$AUDIO_PLAY_SCRIPT" ]; then
        resolve_audio_python
        run_as_user "$AUDIO_PYTHON $AUDIO_PLAY_SCRIPT" &
        PID_AUDIO_SHELL=$!
        echo "Audio Manager shell started (PID: $PID_AUDIO_SHELL)"
    fi

    sleep 1.5
    set_state "INIT"
}

stop_helpers() {
    echo "Stopping helper processes..."
    # 使用 PID 递归清理进程
    if [ -n "$PID_LED_SHELL" ]; then
        echo "Stopping LED process tree..."
        stop_led_helper
    fi
    
    if [ -n "$PID_AUDIO_SHELL" ]; then
        echo "Stopping Audio process tree..."
        kill_tree "$PID_AUDIO_SHELL"
    fi
}

# ================= 异常捕获 =================

on_exit_cleanup() {
    trap '' EXIT SIGINT SIGTERM
    echo ""
    echo ">>> Trapped signal or exit. Cleaning up..."
    
    if mountpoint -q "$MOUNT_POINT"; then
        umount "$MOUNT_POINT" 2>/dev/null
        echo "USB unmounted."
    fi

    stop_helpers
    
    if ! systemctl is-active --quiet ugripper.service; then
        echo "Restoring ugripper.service..."
        systemctl start ugripper.service
    fi
    
    echo ">>> Cleanup Finished."
}

trap on_exit_cleanup EXIT SIGINT SIGTERM

# ================= 主逻辑 =================

echo ">>> Calibration Triggered."

echo "Stopping ugripper.service..."
systemctl stop ugripper.service
sleep 1

start_helpers

# --- 阶段 1: 准备 ---
echo "Phase 1: Preparation (Yellow Slow Flash)"
set_state "CALIB_PRE"       
notify_audio "calib_start"  
sleep 3                     

# --- 阶段 2: 执行校准 ---
echo "Phase 2: Calibrating (Yellow Fast Flash)"
set_state "CALIB_RUN"
notify_audio "calibrating"  

# IMU校准会影响vio标定参数，并且由于半双工，开启主动发送会干扰校准指令，先屏蔽掉
# 2.1 IMU
# if [ -f "$IMU_CALIB_BIN" ]; then
#     echo "Running IMU Calibration..."
#     run_as_user "$IMU_CALIB_BIN"
# else
#     echo "Error: IMU binary not found at $IMU_CALIB_BIN"
# fi

# 2.2 Encoder
if [ -f "$ENCODER_CALIB_BIN" ]; then
    echo "Running Encoder Zeroing..."
    run_as_user "$ENCODER_CALIB_BIN"
else
    echo "Error: Encoder binary not found at $ENCODER_CALIB_BIN"
fi

sleep 2 
# --- 阶段 3: 完成 ---
echo "Phase 3: Done (Green Flash)"
set_state "CALIB_DONE"
notify_audio "calib_done"   
sleep 3                     

echo ">>> Calibration Sequence Finished."
echo "Restoring ugripper.service..."
systemctl start ugripper.service
exit 0
