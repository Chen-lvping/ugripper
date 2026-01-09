#!/bin/bash
set -uo pipefail

# ================= 配置 =================
# 软件包名称
PKG_NAME="ugripper"
# 搜索备份包的路径 (可以配置多个，用空格分隔)
# 例如：/opt/backup /usr/local/src /mnt/data
SEARCH_PATHS="/opt/backup"
# 包名匹配模式
DEB_PATTERN="ugripper_*_arm64.deb"
# 日志文件
LOG_FILE="/var/log/ugripper/boot_install.log"
# =======================================

mkdir -p "$(dirname "$LOG_FILE")"

log() {
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] $*" | tee -a "$LOG_FILE"
}

# 1. 检查是否已安装
if dpkg -s "$PKG_NAME" >/dev/null 2>&1; then
    # 检查状态是否为 install ok installed
    STATUS=$(dpkg-query -W -f='${Status}' "$PKG_NAME" 2>/dev/null || true)
    if [[ "$STATUS" == "install ok installed" ]]; then
        echo "[$PKG_NAME] 已经安装，跳过检查。"
        exit 0
    fi
fi

log "警告：检测到 [$PKG_NAME] 未安装或状态异常，准备执行自动恢复..."

# 2. 搜索安装包
FOUND_DEB=""
for path in $SEARCH_PATHS; do
    if [ -d "$path" ]; then
        # 查找最新的一个 deb 包
        FOUND_DEB=$(find "$path" -maxdepth 2 -type f -name "$DEB_PATTERN" | head -n 1)
        if [ -n "$FOUND_DEB" ]; then
            break
        fi
    fi
done

if [ -z "$FOUND_DEB" ]; then
    log "错误：在搜索路径中未找到匹配的安装包 ($DEB_PATTERN)。无法恢复。"
    exit 1
fi

# 3. 执行安装
log "找到安装包：$FOUND_DEB，开始安装..."
# 防止 apt 锁占用，稍微等待或使用 dpkg 直接安装
if dpkg -i "$FOUND_DEB"; then
    log "恢复安装成功！"
else
    log "严重错误：安装失败，请检查包文件是否损坏。"
    exit 1
fi

exit 0
