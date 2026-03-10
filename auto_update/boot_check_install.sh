#!/bin/bash
set -uo pipefail

# ================= 配置 =================
UGRIPPER_PKG_NAME="ugripper"
UPDATER_PKG_NAME="ugripper-usb-updater"

# 搜索备份包的路径 (可以配置多个，用空格分隔)
# 例如：/opt/backup /usr/local/src /mnt/data
SEARCH_PATHS="/opt/backup"

# 包名匹配模式
UGRIPPER_DEB_PATTERN="ugripper_*_arm64.deb"
UPDATER_DEB_PATTERN="ugripper-usb-updater*_all.deb"

# 日志文件
LOG_FILE="/var/log/ugripper/boot_install.log"
# =======================================

mkdir -p "$(dirname "$LOG_FILE")"

log() {
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] $*" | tee -a "$LOG_FILE"
}

get_pkg_status() {
    local pkg_name="$1"
    dpkg-query -W -f='${Status}' "$pkg_name" 2>/dev/null || true
}

get_installed_version() {
    local pkg_name="$1"
    local status

    status=$(get_pkg_status "$pkg_name")
    if [[ "$status" == "install ok installed" ]]; then
        dpkg-query -W -f='${Version}' "$pkg_name" 2>/dev/null || true
    fi
}

find_best_backup_deb() {
    local pattern="$1"
    local best_deb=""
    local best_ver=""

    for path in $SEARCH_PATHS; do
        if [ ! -d "$path" ]; then
            continue
        fi

        while IFS= read -r deb_file; do
            [ -z "$deb_file" ] && continue

            local deb_ver
            deb_ver=$(dpkg-deb -f "$deb_file" Version 2>/dev/null || true)
            [ -z "$deb_ver" ] && continue

            if [ -z "$best_ver" ] || dpkg --compare-versions "$deb_ver" gt "$best_ver"; then
                best_deb="$deb_file"
                best_ver="$deb_ver"
            fi
        done < <(find "$path" -maxdepth 2 -type f -name "$pattern" 2>/dev/null)
    done

    echo "$best_deb"
}

install_deb_with_log() {
    local pkg_label="$1"
    local deb_file="$2"

    log "安装 [$pkg_label]，deb=$deb_file"
    if dpkg -i "$deb_file"; then
        log "[$pkg_label] 安装成功。"
        return 0
    fi

    log "[$pkg_label] 安装失败。"
    return 1
}

log "===== boot_check_install start ====="

# =====================================================
# A) 先检查/升级 usb-updater（若 backup 版本更高）
# =====================================================
BEST_UPDATER_DEB=$(find_best_backup_deb "$UPDATER_DEB_PATTERN")
if [ -n "$BEST_UPDATER_DEB" ]; then
    BACKUP_UPDATER_VER=$(dpkg-deb -f "$BEST_UPDATER_DEB" Version 2>/dev/null || true)
    INSTALLED_UPDATER_VER=$(get_installed_version "$UPDATER_PKG_NAME")

    if [ -z "$INSTALLED_UPDATER_VER" ]; then
        log "[$UPDATER_PKG_NAME] 未安装，backup 版本=$BACKUP_UPDATER_VER，准备安装。"
        if ! install_deb_with_log "$UPDATER_PKG_NAME" "$BEST_UPDATER_DEB"; then
            log "[$UPDATER_PKG_NAME] 安装失败，继续执行 ugripper 检查。"
        fi
    elif dpkg --compare-versions "$BACKUP_UPDATER_VER" gt "$INSTALLED_UPDATER_VER"; then
        log "[$UPDATER_PKG_NAME] 检测到可升级：installed=$INSTALLED_UPDATER_VER, backup=$BACKUP_UPDATER_VER"
        if ! install_deb_with_log "$UPDATER_PKG_NAME" "$BEST_UPDATER_DEB"; then
            log "[$UPDATER_PKG_NAME] 升级失败，继续执行 ugripper 检查。"
        fi
    else
        log "[$UPDATER_PKG_NAME] 无需升级：installed=${INSTALLED_UPDATER_VER:-N/A}, backup=$BACKUP_UPDATER_VER"
    fi
else
    log "未在 backup 中找到 [$UPDATER_PKG_NAME] 匹配包 ($UPDATER_DEB_PATTERN)，跳过 updater 检查。"
fi

# =====================================================
# B) 再检查/升级 ugripper
#    - 已安装且 backup 版本更高：升级
#    - 未安装或状态异常：恢复安装
# =====================================================
BEST_UGRIPPER_DEB=$(find_best_backup_deb "$UGRIPPER_DEB_PATTERN")
if [ -z "$BEST_UGRIPPER_DEB" ]; then
    log "未在 backup 中找到 [$UGRIPPER_PKG_NAME] 匹配包 ($UGRIPPER_DEB_PATTERN)，跳过 ugripper 检查。"
    exit 0
fi

BACKUP_UGRIPPER_VER=$(dpkg-deb -f "$BEST_UGRIPPER_DEB" Version 2>/dev/null || true)
UGRIPPER_STATUS=$(get_pkg_status "$UGRIPPER_PKG_NAME")
INSTALLED_UGRIPPER_VER=$(get_installed_version "$UGRIPPER_PKG_NAME")

if [[ "$UGRIPPER_STATUS" == "install ok installed" ]]; then
    if dpkg --compare-versions "$BACKUP_UGRIPPER_VER" gt "$INSTALLED_UGRIPPER_VER"; then
        log "[$UGRIPPER_PKG_NAME] 检测到可升级：installed=$INSTALLED_UGRIPPER_VER, backup=$BACKUP_UGRIPPER_VER"
        if install_deb_with_log "$UGRIPPER_PKG_NAME" "$BEST_UGRIPPER_DEB"; then
            log "[$UGRIPPER_PKG_NAME] 升级成功。"
        else
            log "严重错误：[$UGRIPPER_PKG_NAME] 升级失败，请检查 backup 包是否损坏。"
            exit 1
        fi
    else
        log "[$UGRIPPER_PKG_NAME] 无需升级：installed=$INSTALLED_UGRIPPER_VER, backup=$BACKUP_UGRIPPER_VER"
    fi
    exit 0
fi

log "警告：检测到 [$UGRIPPER_PKG_NAME] 未安装或状态异常（status=${UGRIPPER_STATUS:-unknown}），准备执行自动恢复..."
log "找到 [$UGRIPPER_PKG_NAME] 恢复包：$BEST_UGRIPPER_DEB (version=${BACKUP_UGRIPPER_VER:-unknown})"

if install_deb_with_log "$UGRIPPER_PKG_NAME" "$BEST_UGRIPPER_DEB"; then
    log "[$UGRIPPER_PKG_NAME] 恢复安装成功。"
else
    log "严重错误：[$UGRIPPER_PKG_NAME] 恢复安装失败，请检查 backup 包是否损坏。"
    exit 1
fi

exit 0
