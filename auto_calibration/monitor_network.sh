#!/bin/bash
INTERFACE="end0"
CARRIER_PATH="/sys/class/net/$INTERFACE/carrier"
UPGRADE_GUARD_FILE="/run/ugripper_installing_from_usb.lock"

read_carrier_state() {
    if [ ! -r "$CARRIER_PATH" ]; then
        echo ""
        return
    fi

    local state
    state=$(cat "$CARRIER_PATH" 2>/dev/null || true)
    if [ "$state" != "0" ] && [ "$state" != "1" ]; then
        echo ""
        return
    fi
    echo "$state"
}

is_upgrade_in_progress() {
    [ -f "$UPGRADE_GUARD_FILE" ]
}

# ================= 初始化状态 =================
LAST_STATE=$(read_carrier_state)
if [ -z "$LAST_STATE" ]; then
    LAST_STATE=0
fi

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
    CURRENT_STATE=$(read_carrier_state)

    # 网卡暂时不可读时不覆盖 LAST_STATE，避免制造伪边沿
    if [ -z "$CURRENT_STATE" ]; then
        echo "Warning: interface $INTERFACE not ready, keep last state=$LAST_STATE"
        sleep 1
        continue
    fi

    if [ "$LAST_STATE" -ne "$CURRENT_STATE" ]; then
        if is_upgrade_in_progress; then
            echo "[$(date)] Upgrade guard active, skip network edge action: ${LAST_STATE} -> ${CURRENT_STATE}"
            LAST_STATE="$CURRENT_STATE"
            sleep 1
            continue
        fi

        # ================= 下降沿：插着 → 拔掉 =================
        # 仅在拔线时重启 ugripper，避免插线现场难以抓取问题。
        if [ "$LAST_STATE" -eq 1 ] && [ "$CURRENT_STATE" -eq 0 ]; then
            echo "[$(date)] Cable Removal Detected!"
            restart_ugripper
        fi

        # ================= 上升沿：拔掉 → 插入 =================
        if [ "$LAST_STATE" -eq 0 ] && [ "$CURRENT_STATE" -eq 1 ]; then
            echo "[$(date)] Cable Insertion Detected!"

            echo "Triggering udev block rules..."
            udevadm trigger --subsystem-match=block --action=add || true
        fi

        LAST_STATE="$CURRENT_STATE"
    fi

    sleep 1
done
