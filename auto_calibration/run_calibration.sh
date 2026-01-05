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
IMU_CALIB_BIN="./dm_imu_alone/build/imu_calib"
ENCODER_CALIB_BIN="./encoder_refactor/build/zeroing"

# --- LED 控制配置 ---
LED_SCRIPT="./led_manager.py"
LED_PIPE="/tmp/umi_led_pipe"

# --- 音频配置 ---
AUDIO_PLAY_SCRIPT="./audio/audio_play.py"
AUDIO_PIPE="/tmp/umi_audio_pipe"

# 全局变量存储 PID
PID_LED_SHELL=""
PID_AUDIO_SHELL=""

# --- 按钮配置 ---
PIN_BTN="PIN_36"    # 按钮输入
BTN_ACTIVE_LEVEL=1  # 1表示按下
DEBOUNCE_MS=0.03    # 30ms


# 停止业务服务，防止占用
echo "Stopping ugripper.service..."
systemctl stop ugripper.service
sleep 5

# ================= 检查按钮状态 =================
if [ -z "$(gpiofind "$PIN_BTN")" ]; then
    echo "Error: Could not find GPIO pin $PIN_BTN."
    exit 1
fi

BTN_VAL=$(gpioget $(gpiofind "$PIN_BTN"))
if [ "$BTN_VAL" != "$BTN_ACTIVE_LEVEL" ]; then
    echo "Button is not pressed. Exiting calibration."
    systemctl start ugripper.service
    exit 0
fi

# ================= 辅助函数 =================

run_as_user() {
    runuser -u "$TARGET_USER" -- bash -lc "$*"
}

set_state() {
    local state=$1
    if [ -p "$LED_PIPE" ]; then
        echo "$state" > "$LED_PIPE" &
    fi
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
    
    rm -f "$LED_PIPE" "$AUDIO_PIPE"
    mkfifo "$LED_PIPE"
    mkfifo "$AUDIO_PIPE"
    chmod 666 "$LED_PIPE" "$AUDIO_PIPE"

    if [ -f "$LED_SCRIPT" ]; then
        run_as_user "uv run $LED_SCRIPT" &
        PID_LED_SHELL=$! 
        echo "LED Manager shell started (PID: $PID_LED_SHELL)"
    fi

    if [ -f "$AUDIO_PLAY_SCRIPT" ]; then
        amixer -c rockchipes8388 sset 'ALC Capture Function' Stereo >/dev/null 2>&1
        amixer -c rockchipes8388 sset 'ALC Capture Max PGA' 7 >/dev/null 2>&1

        run_as_user "uv run $AUDIO_PLAY_SCRIPT" &
        PID_AUDIO_SHELL=$!
        echo "Audio Manager shell started (PID: $PID_AUDIO_SHELL)"
    fi

    sleep 1.5
    set_state "INIT"
}

stop_helpers() {
    echo "Stopping helper processes..."
    
    # 1. 发送退出状态（通知逻辑层）
    set_state "EXIT"
    
    # 2. 使用 PID 递归清理进程
    if [ -n "$PID_LED_SHELL" ]; then
        echo "Stopping LED process tree..."
        kill_tree "$PID_LED_SHELL"
    fi
    
    if [ -n "$PID_AUDIO_SHELL" ]; then
        echo "Stopping Audio process tree..."
        kill_tree "$PID_AUDIO_SHELL"
    fi
    
    rm -f "$LED_PIPE" "$AUDIO_PIPE"
}

# ================= 异常捕获 =================

on_exit_cleanup() {
    # 防止重复执行清理
    trap '' EXIT SIGINT SIGTERM
    
    echo ""
    echo ">>> Trapped signal or exit. Cleaning up..."
    
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
