#!/bin/bash
INTERFACE="end0"
CARRIER_PATH="/sys/class/net/$INTERFACE/carrier"
UPGRADE_GUARD_FILE="/run/ugripper_installing_from_usb.lock"
ENV_FILE="/etc/environment"
RIGHT_MASTER_IP="192.168.1.100"
ROLE_SWITCH_CONFIRM_COUNT=2

get_env_value() {
    local key="$1"
    local line value

    [ -f "$ENV_FILE" ] || return 1
    line=$(grep -E "^${key}=" "$ENV_FILE" | head -n1 || true)
    [ -n "$line" ] || return 1

    value="${line#*=}"
    value="$(printf '%s' "$value" | tr -d '"' | xargs)"
    printf '%s' "$value"
}

get_device_side_lower() {
    local device_side=""
    device_side="$(get_env_value "DEVICE_SIDE" || true)"
    printf '%s' "${device_side,,}"
}

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

is_left_side() {
    [ "$(get_device_side_lower)" = "left" ]
}

is_right_master_reachable() {
    ping -c 1 -W 1 "$RIGHT_MASTER_IP" >/dev/null 2>&1
}

get_target_role() {
    local carrier_state="$1"

    if ! is_left_side; then
        printf 'master'
        return 0
    fi

    if [ "$carrier_state" = "1" ] && is_right_master_reachable; then
        printf 'slave'
        return 0
    fi

    printf 'master'
}

# ================= 初始化状态 =================
LAST_STATE=$(read_carrier_state)
if [ -z "$LAST_STATE" ]; then
    LAST_STATE=0
fi

LAST_TARGET_ROLE="$(get_target_role "$LAST_STATE")"
PENDING_TARGET_ROLE=""
PENDING_TARGET_ROLE_COUNT=0

echo "Network Monitor Started for $INTERFACE. Initial State: $LAST_STATE"
if is_left_side; then
    echo "Initial LEFT target role: $LAST_TARGET_ROLE"
fi

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

        # ================= 上升沿：拔掉 → 插入 =================
        if [ "$LAST_STATE" -eq 0 ] && [ "$CURRENT_STATE" -eq 1 ]; then
            echo "[$(date)] Cable Insertion Detected!"

            echo "Triggering udev block rules..."
            udevadm trigger --subsystem-match=block --action=add || true
        fi

        # ================= 下降沿：插着 → 拔掉 =================
        if [ "$LAST_STATE" -eq 1 ] && [ "$CURRENT_STATE" -eq 0 ]; then
            echo "[$(date)] Cable Removal Detected!"
            if ! is_left_side; then
                restart_ugripper
            fi
        fi

        LAST_STATE="$CURRENT_STATE"
    fi

    if is_left_side; then
        if is_upgrade_in_progress; then
            echo "[$(date)] Upgrade guard active, skip LEFT auto-role check"
            sleep 1
            continue
        fi

        CURRENT_TARGET_ROLE="$(get_target_role "$CURRENT_STATE")"
        if [ "$CURRENT_TARGET_ROLE" = "$LAST_TARGET_ROLE" ]; then
            PENDING_TARGET_ROLE=""
            PENDING_TARGET_ROLE_COUNT=0
        else
            if [ "$CURRENT_TARGET_ROLE" != "$PENDING_TARGET_ROLE" ]; then
                PENDING_TARGET_ROLE="$CURRENT_TARGET_ROLE"
                PENDING_TARGET_ROLE_COUNT=1
                echo "[$(date)] LEFT target role candidate changed: ${LAST_TARGET_ROLE} -> ${CURRENT_TARGET_ROLE} (1/${ROLE_SWITCH_CONFIRM_COUNT})"
            else
                PENDING_TARGET_ROLE_COUNT=$((PENDING_TARGET_ROLE_COUNT + 1))
                echo "[$(date)] LEFT target role candidate stable: ${LAST_TARGET_ROLE} -> ${CURRENT_TARGET_ROLE} (${PENDING_TARGET_ROLE_COUNT}/${ROLE_SWITCH_CONFIRM_COUNT})"
            fi

            if [ "$PENDING_TARGET_ROLE_COUNT" -ge "$ROLE_SWITCH_CONFIRM_COUNT" ]; then
                echo "[$(date)] LEFT target role switch confirmed: ${LAST_TARGET_ROLE} -> ${CURRENT_TARGET_ROLE}, restarting ugripper.service"
                LAST_TARGET_ROLE="$CURRENT_TARGET_ROLE"
                PENDING_TARGET_ROLE=""
                PENDING_TARGET_ROLE_COUNT=0
                restart_ugripper
            fi
        fi
    fi

    sleep 1
done
