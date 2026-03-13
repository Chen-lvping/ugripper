#!/bin/bash

list_physical_ethernet_interfaces() {
    local sys_path iface type

    for sys_path in /sys/class/net/*; do
        iface="${sys_path##*/}"

        [ -n "$iface" ] || continue
        [ "$iface" != "lo" ] || continue
        [ -L "$sys_path/device" ] || [ -d "$sys_path/device" ] || continue
        [ -r "$sys_path/type" ] || continue

        type="$(cat "$sys_path/type" 2>/dev/null || true)"
        [ "$type" = "1" ] || continue
        [ -r "$sys_path/carrier" ] || continue

        printf '%s\n' "$iface"
    done
}

read_iface_carrier() {
    local iface="$1"
    local carrier_path="/sys/class/net/$iface/carrier"
    local state

    if [ -z "$iface" ] || [ ! -r "$carrier_path" ]; then
        return 1
    fi

    state="$(cat "$carrier_path" 2>/dev/null || true)"
    if [ "$state" != "0" ] && [ "$state" != "1" ]; then
        return 1
    fi

    printf '%s\n' "$state"
}

detect_preferred_ethernet_interface() {
    local iface fallback_iface=""
    local carrier_state=""

    while IFS= read -r iface; do
        [ -n "$iface" ] || continue

        if [ -z "$fallback_iface" ]; then
            fallback_iface="$iface"
        fi

        carrier_state="$(read_iface_carrier "$iface" 2>/dev/null || true)"
        if [ "$carrier_state" = "1" ]; then
            printf '%s\n' "$iface"
            return 0
        fi
    done < <(list_physical_ethernet_interfaces)

    [ -n "$fallback_iface" ] || return 1
    printf '%s\n' "$fallback_iface"
}
