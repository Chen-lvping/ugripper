#!/bin/bash

INTERFACE="end0"
CARRIER_PATH="/sys/class/net/$INTERFACE/carrier"

# ================= 配置 =================
# 脚本启动时（如果网线插着）不触发，只在"拔掉再插"时触发，
if [ -f "$CARRIER_PATH" ]; then
    LAST_STATE=$(cat "$CARRIER_PATH")
else
    LAST_STATE=0
fi

echo "Network Monitor Started for $INTERFACE. Initial State: $LAST_STATE"

while true; do
    # 1. 检查文件是否存在 (防止网卡驱动未加载报错)
    if [ -f "$CARRIER_PATH" ]; then
        CURRENT_STATE=$(cat "$CARRIER_PATH")
        
        # 2. 检测 "上升沿" (从 0 变为 1)
        if [ "$LAST_STATE" -eq 0 ] && [ "$CURRENT_STATE" -eq 1 ]; then
            echo "[$(date)] Cable Insertion Detected! Triggering calibration..."
            
            # 异步触发校准服务 (不阻塞循环)
            systemctl start ugripper-calibration.service --no-block
        fi
        
        # 3. 更新状态
        LAST_STATE="$CURRENT_STATE"
    else
        echo "Warning: $INTERFACE not found."
        LAST_STATE=0
    fi
    
    # 4. 轮询间隔 (1秒)
    sleep 1
done
