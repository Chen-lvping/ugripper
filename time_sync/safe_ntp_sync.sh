#!/bin/bash

# ================= 配置区域 =================
LOCK_FILE="/tmp/umi_recording.lock"

# 网络检测目标 (IP地址，避免DNS问题)
# Google DNS (8.8.8.8), Cloudflare (1.1.1.1), AliDNS (223.5.5.5)
CHECK_TARGETS=("8.8.8.8" "1.1.1.1" "223.5.5.5")

# 最大同步等待时间 (秒)
MAX_SYNC_WAIT=60

log() {
    echo "[NTP-Service] $1"
}

# --- 阶段 1: 等待网络连通 ---
log "Service started (Backend: systemd-timesyncd). Waiting for network..."

check_network() {
    for target in "${CHECK_TARGETS[@]}"; do
        if ping -c 1 -W 2 "$target" >/dev/null 2>&1; then
            return 0
        fi
    done
    return 1
}

while ! check_network; do
    sleep 3
done
log "Network is UP."

# --- 阶段 2: 安全检测 (录制锁) ---
while [ -f "$LOCK_FILE" ]; do
    log "System is RECORDING (Lock found). Waiting for idle..."
    sleep 5
done

# --- 阶段 3: 执行时间同步 ---
log "System IDLE. Enabling NTP service..."

# 1. 确保先关闭再开启，触发一次立即的同步尝试
timedatectl set-ntp false
timedatectl set-ntp true

log "Waiting for sync status..."

start_time=$(date +%s)
sync_success=false

while true; do
    # 检查同步状态
    # timedatectl show -p NTPSynchronized --value 会返回 'yes' 或 'no'
    status=$(timedatectl show -p NTPSynchronized --value 2>/dev/null)
    
    if [ "$status" == "yes" ]; then
        sync_success=true
        break
    fi
    
    # 再次检查锁：如果在等待同步过程中用户突然开始了录制，我们应该怎么做？
    # 策略：为了数据安全，如果在同步中途检测到录制开始，立即中止同步并关闭NTP
    if [ -f "$LOCK_FILE" ]; then
        log "INTERRUPT: Recording started during sync wait! Aborting."
        timedatectl set-ntp false
        exit 0
    fi

    # 超时检查
    current_time=$(date +%s)
    elapsed=$((current_time - start_time))
    if [ "$elapsed" -ge "$MAX_SYNC_WAIT" ]; then
        log "Timeout waiting for NTP sync."
        break
    fi

    sleep 1
done

# --- 阶段 4: 收尾 ---
if [ "$sync_success" = true ]; then
    log "Time sync SUCCESS."
    # 打印一下当前时间确认
    log "Current time: $(date)"
else
    log "Time sync FAILED (Timeout or Error)."
fi

# 为了防止录制期间后台产生网络包或时间漂移，同步完成后再次关闭
log "Disabling NTP service to protect future recordings."
timedatectl set-ntp false

exit 0
