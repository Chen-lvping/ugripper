#!/bin/bash
set -e

# ================= 配置区域 =================
# 源文件所在目录
SRC_DIR="./auto_update"

# 软件包信息
PKG_NAME="ugripper-usb-updater"
PKG_VERSION="1.0.0"
ARCH="all"
MAINTAINER="User <user@example.com>"
DESC="Auto update ugripper via USB and Boot Check"

# 构建临时目录
BUILD_DIR="build_usb_updater_temp"
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
    
    # 可选：立即启动一次检查（如果需要安装后立即生效，而不是等重启）
    # systemctl start ugripper-boot-install.service || true
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

echo "========================================"
echo "构建完成！"
ls -lh "${BUILD_DIR}.deb"
echo "========================================"
