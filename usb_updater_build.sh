#!/bin/bash
set -e

# ================= 配置区域 =================
# 源文件所在目录
SRC_DIR="./auto_update"

# 软件包信息
PKG_NAME="ugripper-usb-updater"
PKG_VERSION="1.2.2"
ARCH="all"
MAINTAINER="User <user@example.com>"
DESC="Auto update ugripper via USB and Boot Check"

# 构建临时目录
BUILD_DIR="temp_build_usb_updater"
PKG_DIR="${PKG_NAME}_${PKG_VERSION}_${ARCH}"

# ================= 0. 检查源文件 =================
echo "检查源文件..."
if [ ! -d "$SRC_DIR" ]; then
    echo "错误：找不到源目录 $SRC_DIR"
    exit 1
fi

# 核心文件列表
REQUIRED_FILES=(
    "usb_auto_update.sh"
    "99-usb-auto-update.rules"
    "usb-auto-update@.service"
    "boot_check_install.sh"
    "ugripper-boot-install.service"
)

for file in "${REQUIRED_FILES[@]}"; do
    if [ ! -f "$SRC_DIR/$file" ]; then
        echo "错误：在 $SRC_DIR 中找不到文件 $file"
        exit 1
    fi
done

# ================= 1. 创建目录结构 =================
echo "正在清理并创建构建目录 $BUILD_DIR ..."
rm -rf "$BUILD_DIR"

mkdir -p "${BUILD_DIR}/usr/local/bin"
mkdir -p "${BUILD_DIR}/etc/udev/rules.d"
mkdir -p "${BUILD_DIR}/lib/systemd/system"
mkdir -p "${BUILD_DIR}/DEBIAN"

# ================= 2. 复制文件并设置权限 =================
echo "复制业务文件..."

# --- A. USB 自动更新功能文件 ---
# 脚本
cp "$SRC_DIR/usb_auto_update.sh" "${BUILD_DIR}/usr/local/bin/"
chmod 755 "${BUILD_DIR}/usr/local/bin/usb_auto_update.sh"
# Udev 规则
cp "$SRC_DIR/99-usb-auto-update.rules" "${BUILD_DIR}/etc/udev/rules.d/"
chmod 644 "${BUILD_DIR}/etc/udev/rules.d/99-usb-auto-update.rules"
# Service
cp "$SRC_DIR/usb-auto-update@.service" "${BUILD_DIR}/lib/systemd/system/"
chmod 644 "${BUILD_DIR}/lib/systemd/system/usb-auto-update@.service"

# --- B. 开机自检安装功能文件 ---
# 脚本
cp "$SRC_DIR/boot_check_install.sh" "${BUILD_DIR}/usr/local/bin/"
chmod 755 "${BUILD_DIR}/usr/local/bin/boot_check_install.sh"
# Service
cp "$SRC_DIR/ugripper-boot-install.service" "${BUILD_DIR}/lib/systemd/system/"
chmod 644 "${BUILD_DIR}/lib/systemd/system/ugripper-boot-install.service"

# ================= 3. 生成 DEBIAN 元数据文件 =================
echo "生成控制文件和维护脚本..."

# --- control ---
cat > "${BUILD_DIR}/DEBIAN/control" << EOF
Package: ${PKG_NAME}
Version: ${PKG_VERSION}
Section: admin
Priority: optional
Architecture: ${ARCH}
Depends: bash, systemd, udev, util-linux
Maintainer: ${MAINTAINER}
Description: ${DESC}
 This package installs:
 1. USB auto-update mechanism for ugripper.
 2. Boot-time check to restore ugripper if missing.
EOF

# --- postinst (关键更新：启用新服务) ---
cat > "${BUILD_DIR}/DEBIAN/postinst" << EOF
#!/bin/sh
set -e

if [ "\$1" = "configure" ]; then
    echo "Configuring ${PKG_NAME}..."
    
    # 1. Reload udev rules
    udevadm control --reload-rules
    
    # 2. Reload systemd daemon
    systemctl daemon-reload
    
    # 3. Enable the boot check service
    # 这样开机时就会自动运行检查脚本
    echo "Enabling ugripper-boot-install.service..."
    systemctl enable ugripper-boot-install.service

    if [ -f /run/ugripper_usb_update_skip_postinst_rerun ]; then
        rm -f /run/ugripper_usb_update_skip_postinst_rerun
    else
        USB_UPDATE_DEV=\$(ps -eo args= | awk '
            /[u]sb_auto_update\.sh/ && \$0 !~ /--rerun-after-self-update/ {
                for (i = 1; i <= NF; ++i) {
                    if (\$i ~ /^\\/dev\\//) {
                        print \$i
                        exit
                    }
                }
            }
        ')

        if [ -n "\$USB_UPDATE_DEV" ]; then
            echo "Detected active usb_auto_update context on \$USB_UPDATE_DEV, scheduling rerun with new updater logic..."
            mkdir -p /var/log/ugripper
            USB_UPDATE_UNIT_SUFFIX=\$(printf '%s' "\$USB_UPDATE_DEV" | tr '/:@' '___')
            if command -v systemd-run >/dev/null 2>&1; then
                systemd-run --quiet --no-block --collect \
                    --unit="ugripper-usb-update-rerun-\${USB_UPDATE_UNIT_SUFFIX}" \
                    --property=Type=oneshot \
                    --setenv=USB_UPDATE_DEV="\$USB_UPDATE_DEV" \
                    /bin/sh -lc '
                        mkdir -p /var/log/ugripper
                        echo "[\$(date "+%Y-%m-%d %H:%M:%S")] postinst scheduled rerun for \$USB_UPDATE_DEV" >> /var/log/ugripper/usb_auto_update.log
                        attempts=0
                        while [ "\$attempts" -lt 90 ]; do
                            /usr/local/bin/usb_auto_update.sh "\$USB_UPDATE_DEV" --rerun-after-self-update
                            rc=\$?
                            if [ "\$rc" -ne 75 ]; then
                                exit 0
                            fi
                            attempts=\$((attempts + 1))
                            sleep 2
                        done
                        echo "[\$(date "+%Y-%m-%d %H:%M:%S")] postinst rerun timed out waiting for lock: \$USB_UPDATE_DEV" >> /var/log/ugripper/usb_auto_update.log
                        exit 0
                    '
            else
                nohup /bin/sh -c '
                    dev="\$1"
                    attempts=0
                    mkdir -p /var/log/ugripper
                    echo "[\$(date "+%Y-%m-%d %H:%M:%S")] postinst fallback rerun for \$dev" >> /var/log/ugripper/usb_auto_update.log
                    while [ "\$attempts" -lt 90 ]; do
                        /usr/local/bin/usb_auto_update.sh "\$dev" --rerun-after-self-update
                        rc=\$?
                        if [ "\$rc" -ne 75 ]; then
                            exit 0
                        fi
                        attempts=\$((attempts + 1))
                        sleep 2
                    done
                    echo "[\$(date "+%Y-%m-%d %H:%M:%S")] postinst fallback rerun timed out waiting for lock: \$dev" >> /var/log/ugripper/usb_auto_update.log
                    exit 0
                ' postinst-rerun "\$USB_UPDATE_DEV" >/dev/null 2>&1 &
            fi
        fi
    fi
fi

exit 0
EOF
chmod 755 "${BUILD_DIR}/DEBIAN/postinst"

# --- prerm ---
cat > "${BUILD_DIR}/DEBIAN/prerm" << EOF
#!/bin/sh
set -e
if [ "\$1" = "remove" ]; then
    # 卸载前停止并禁用服务
    systemctl disable --now ugripper-boot-install.service || true
fi
exit 0
EOF
chmod 755 "${BUILD_DIR}/DEBIAN/prerm"

# --- postrm ---
cat > "${BUILD_DIR}/DEBIAN/postrm" << EOF
#!/bin/sh
set -e
if [ "\$1" = "remove" ] || [ "\$1" = "purge" ]; then
    udevadm control --reload-rules
    systemctl daemon-reload
fi
exit 0
EOF
chmod 755 "${BUILD_DIR}/DEBIAN/postrm"

# ================= 4. 打包 =================
echo "开始构建 .deb 包..."
dpkg-deb --build "$BUILD_DIR" "${PKG_DIR}.deb"

# Refresh source baseline manifest for auto-release-deb diff detection.
MANIFEST_WRITER=".codex/skills/auto-release-deb/scripts/write_source_manifest.sh"
if [ -x "$MANIFEST_WRITER" ]; then
    bash "$MANIFEST_WRITER" --target updater --output "$BUILD_DIR/.auto_release_updater.manifest" || true
fi

echo "========================================"
echo "构建完成！"
ls -lh "${PKG_DIR}.deb"
echo "========================================"
