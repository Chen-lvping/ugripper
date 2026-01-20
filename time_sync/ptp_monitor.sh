#!/bin/bash

# ================= 配置 =================
STATUS_FILE="/dev/shm/umi_ptp_status"
TEMP_FILE="${STATUS_FILE}.tmp"
INTERVAL=1

# 确保文件存在并可读写
touch "$STATUS_FILE"
chmod 666 "$STATUS_FILE"

# ================= 信号处理 =================
cleanup() {
    echo "PTP Monitor: Received stop signal, exiting..."
    # 退出前可以把状态标记为 UNKNOWN
    echo "{\"timestamp\": \"$(date +%s)\", \"state\": \"UNKNOWN\", \"offset\": 0.0, \"path_delay\": 0.0}" > "$STATUS_FILE"
    exit 0
}
trap cleanup SIGTERM SIGINT

# ================= 主循环 =================
while true; do
    # 1. 获取端口状态 (Master/Slave/Uncalibrated)
    PORT_STATE_RAW=$(pmc -u -b 0 'GET PORT_DATA_SET' 2>/dev/null)
    PORT_STATE=$(echo "$PORT_STATE_RAW" | grep 'portState' | awk '{print $2}')

    # 2. 获取时间偏移 (Offset)
    CURRENT_DATA_RAW=$(pmc -u -b 0 'GET CURRENT_DATA_SET' 2>/dev/null)
    OFFSET=$(echo "$CURRENT_DATA_RAW" | grep 'offsetFromMaster' | awk '{print $2}')
    DELAY=$(echo "$CURRENT_DATA_RAW" | grep 'meanPathDelay' | awk '{print $2}')

    # 3. 数据清洗
    [ -z "$PORT_STATE" ] && PORT_STATE="UNKNOWN"
    [ -z "$OFFSET" ] && OFFSET="0.0"
    [ -z "$DELAY" ] && DELAY="0.0"

    # 4. 写 JSON (原子操作)
    echo "{\"timestamp\": \"$(date +%s)\", \"state\": \"$PORT_STATE\", \"offset\": $OFFSET, \"path_delay\": $DELAY}" > "$TEMP_FILE"
    mv "$TEMP_FILE" "$STATUS_FILE"
    chmod 666 "$STATUS_FILE"

    sleep "$INTERVAL"
done
