#!/bin/bash
# TODO service重启时如果有插入状态记录 可能产生“伪上升沿”暂时不影响功能
INTERFACE="end0"
CARRIER_PATH="/sys/class/net/$INTERFACE/carrier"

DATA_DEV="/mnt/data_disk"
UMOUNT_TIMEOUT=5

# ================= 初始化状态 =================
if [ -f "$CARRIER_PATH" ]; then
    LAST_STATE=$(cat "$CARRIER_PATH")
else
    LAST_STATE=0
fi

echo "Network Monitor Started for $INTERFACE. Initial State: $LAST_STATE"

while true; do
    if [ -f "$CARRIER_PATH" ]; then
        CURRENT_STATE=$(cat "$CARRIER_PATH")

        # ================= 下降沿：插着 → 拔掉 =================
        if [ "$LAST_STATE" -eq 1 ] && [ "$CURRENT_STATE" -eq 0 ]; then
            echo "[$(date)] Cable Removal Detected!"

            if mountpoint -q "$DATA_DEV"; then
                echo "Unmounting $DATA_DEV ..."
                timeout "$UMOUNT_TIMEOUT" umount "$DATA_DEV" || {
                    echo "WARNING: umount timeout or failed for $DATA_DEV"
                }
            else
                echo "$DATA_DEV not mounted, skip umount."
            fi
        fi

        # ================= 上升沿：拔掉 → 插入 =================
        if [ "$LAST_STATE" -eq 0 ] && [ "$CURRENT_STATE" -eq 1 ]; then
            echo "[$(date)] Cable Insertion Detected!"

            echo "Triggering udev block rules..."
            udevadm trigger --subsystem-match=block --action=add || true

            echo "Starting calibration service..."
            systemctl start ugripper-calibration.service --no-block
        fi

        LAST_STATE="$CURRENT_STATE"
    else
        echo "Warning: interface $INTERFACE not found."
        LAST_STATE=0
    fi

    sleep 1
done
