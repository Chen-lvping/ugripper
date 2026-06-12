#!/bin/sh

# Resolve the logical UGripper symlink for a CH9344 tty node.
# udev passes DEVPATH (%p) and KERNEL (%k). Keep this helper small because it
# runs in the udev hot path.

if [ "$#" -ne 2 ]; then
    exit 1
fi

devpath="$1"
kernel="$2"

case "$kernel" in
    ttyCH9344USB*) ;;
    *) exit 1 ;;
esac

current_num="${kernel#ttyCH9344USB}"
case "$current_num" in
    ''|*[!0-9]*) exit 1 ;;
esac

side=""
case "$devpath" in
    */1-1.[12]/*|*/1-1.[12]:*|*/5-1.[12]/*|*/5-1.[12]:*)
        side="right"
        ;;
    */9-1.[12]/*|*/9-1.[12]:*|*/11-1.[12]/*|*/11-1.[12]:*)
        side="left"
        ;;
    *)
        exit 1
        ;;
esac

if [ -e "$devpath" ] || [ -d "${devpath%/*}" ]; then
    sys_path="$devpath"
else
    case "$devpath" in
        /sys/*)
            sys_path="$devpath"
            ;;
        /*)
            sys_path="/sys$devpath"
            ;;
        *)
            sys_path="/sys/$devpath"
            ;;
    esac
fi

sibling_dir="${sys_path%/*}"
[ -d "$sibling_dir" ] || exit 1

rank=1
seen_current=0
for node in "$sibling_dir"/ttyCH9344USB*; do
    [ -e "$node" ] || continue
    name="${node##*/}"
    num="${name#ttyCH9344USB}"
    case "$num" in
        ''|*[!0-9]*) continue ;;
    esac
    if [ "$name" = "$kernel" ]; then
        seen_current=1
    elif [ "$num" -lt "$current_num" ]; then
        rank=$((rank + 1))
    fi
done

[ "$seen_current" -eq 1 ] || exit 1

case "$rank" in
    1)
        printf '%s_gripper\n' "$side"
        ;;
    2)
        printf '%s_encoder\n' "$side"
        ;;
    *)
        exit 1
        ;;
esac
