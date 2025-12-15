#!/bin/bash
# list_mainpath_v2.sh
# 修复了解析逻辑，支持 Stepwise 和 Discrete 分辨率，以及 Continuous 帧率范围

VIDEO_NODES=(
    /dev/video11
    /dev/video12
    /dev/video13
    /dev/video14
    /dev/video15
    /dev/video16
    /dev/video19
)

for DEV in "${VIDEO_NODES[@]}"; do
    if [ ! -e "$DEV" ]; then
        continue
    fi

    echo "========================================================"
    echo "Device: $DEV"

    # 1. 获取驱动基础信息 (Driver Info)
    # 使用 -D 选项获取头部信息，干净快捷
    BASIC_INFO=$(sudo v4l2-ctl -d "$DEV" -D 2>/dev/null)
    DRIVER=$(echo "$BASIC_INFO" | grep 'Driver name' | cut -d':' -f2 | xargs)
    CARD=$(echo "$BASIC_INFO" | grep 'Card type' | cut -d':' -f2 | xargs)
    
    echo "  Driver: $DRIVER"
    echo "  Card:   $CARD"
    echo "--------------------------------------------------------"

    # 2. 获取格式详情并解析
    # 使用 awk 逐行状态机处理，区分 Stepwise 和 Discrete
    sudo v4l2-ctl -d "$DEV" --list-formats-ext 2>/dev/null | awk '
        # 匹配格式行，例如: [0]: 'UYVY' (UYVY 4:2:2)
        /\[[0-9]+\]:/ {
            # 去掉前面的索引 [0]: 
            sub(/.*\]: /, "")
            print "  Format: " $0
        }

        # 匹配分辨率行
        /Size:/ {
            # 情况 A: Stepwise (步进式，如 video11)
            # 例子: Size: Stepwise 32x32 - 3840x2160 with step 8/8
            if ($0 ~ /Stepwise/) {
                # 提取 Stepwise 后面的所有内容
                sub(/.*Size: Stepwise /, "")
                # 结果类似: 32x32 - 3840x2160 with step 8/8
                print "    Resolution: " $0 " (Scalable)"
            } 
            # 情况 B: Discrete (离散式，如 video14)
            # 例子: Size: Discrete 3840x2160
            else if ($0 ~ /Discrete/) {
                # 提取分辨率数值 ($3)
                print "    Resolution: " $3
            }
        }

        # 匹配帧率/间隔行
        # 例子: Interval: Continuous 0.017s - 1.000s (1.000-60.000 fps)
        # 例子: Interval: Discrete 0.033s (30.000 fps)
        /Interval:/ {
            # 提取括号里的内容 (...)
            if (match($0, /\(.*\)/)) {
                # RSTART 是括号开始的位置，RLENGTH 是长度
                # substr 取出括号里的内容，去掉首尾括号
                fps_info = substr($0, RSTART+1, RLENGTH-2)
                print "      FPS: " fps_info
            }
        }
    '

    echo ""
done
