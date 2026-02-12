#!/bin/bash
# TODO service重启时如果有插入状态记录 可能产生“伪上升沿”暂时不影响功能
INTERFACE="end0"
CARRIER_PATH="/sys/class/net/$INTERFACE/carrier"

read_carrier_state() {
    if [ ! -r "$CARRIER_PATH" ]; then
        echo "0"
        return
    fi

    local state
    state=$(cat "$CARRIER_PATH" 2>/dev/null || true)
    if [ "$state" != "0" ] && [ "$state" != "1" ]; then
        state="0"
    fi
    echo "$state"
}

# ================= 初始化状态 =================
LAST_STATE=$(read_carrier_state)

echo "Network Monitor Started for $INTERFACE. Initial State: $LAST_STATE"

# ================= 信号处理 =================
cleanup() {
    echo "Network Monitor: Received stop signal. Exiting..."
    exit 0
}
trap cleanup SIGTERM SIGINT

restart_ugripper() {
    echo "Restarting ugripper.service..."
    systemctl restart ugripper.service || {
        echo "WARNING: failed to restart ugripper.service"
    }
}

# ================= 主循环 =================
while true; do
    if [ -f "$CARRIER_PATH" ]; then
        CURRENT_STATE=$(read_carrier_state)

        # ================= 下降沿：插着 → 拔掉 =================
        if [ "$LAST_STATE" -eq 1 ] && [ "$CURRENT_STATE" -eq 0 ]; then
            echo "[$(date)] Cable Removal Detected!"

            restart_ugripper
        fi

        # ================= 上升沿：拔掉 → 插入 =================
        if [ "$LAST_STATE" -eq 0 ] && [ "$CURRENT_STATE" -eq 1 ]; then
            echo "[$(date)] Cable Insertion Detected!"

            echo "Triggering udev block rules..."
            udevadm trigger --subsystem-match=block --action=add || true

            restart_ugripper
        fi

        LAST_STATE="$CURRENT_STATE"
    else
        echo "Warning: interface $INTERFACE not found."
        LAST_STATE=0
    fi

    sleep 1
done
