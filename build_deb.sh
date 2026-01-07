#!/bin/bash
set -e  # 遇到错误立即停止
script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# ================= 变量定义区域 =================
APP_NAME="ugripper"
VERSION="1.0.0"       # 每次发布前修改这里
ARCH="arm64"
INSTALL_DIR="/opt/${APP_NAME}"
BUILD_ROOT="build_deb_temp"
PACK_SCRIPT_DIR="pack_script"
# ===============================================

echo "=== [1/5] 初始化构建环境 ==="
# 清理旧数据
rm -rf "$BUILD_ROOT"
rm -f "${APP_NAME}_${VERSION}_${ARCH}.deb"

# 创建目录结构 (模拟 Linux 文件系统)
mkdir -p "$BUILD_ROOT/DEBIAN"
mkdir -p "$BUILD_ROOT/$INSTALL_DIR"
mkdir -p "$BUILD_ROOT/usr/local/bin"
mkdir -p "$BUILD_ROOT/etc/systemd/system"
mkdir -p "$BUILD_ROOT/etc/udev/rules.d"

echo "=== [2/5] 编译 C++ 模块 ==="

# 编译 dm_imu_alone
echo "--> Building dm_imu_alone..."
cd dm_imu_alone
rm -rf build && mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
cd ../..

# 编译 encoder_refactor
echo "--> Building encoder_refactor..."
cd encoder_refactor
rm -rf build && mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
cd ../..

echo "=== [3/5] 组装文件资源 ==="

# 1. 拷贝项目主体文件
#    排除: ASR, py_script, .git, build脚本, 临时目录, C++源码里的build目录
echo "--> Copying project files..."
rsync -av \
    --exclude='ASR' \
    --exclude='py_script' \
    --exclude='.git' \
    --exclude='build_deb.sh' \
    --exclude='pack_script' \
    --exclude="$BUILD_ROOT" \
    --exclude='dm_imu_alone' \
    --exclude='encoder_refactor' \
    --exclude='*.deb' \
    . "$BUILD_ROOT/$INSTALL_DIR/"

# 2. 手动补回编译好的二进制文件 (保持原有目录结构以便 Python 调用)
echo "--> Restoring compiled binaries..."
mkdir -p "$BUILD_ROOT/$INSTALL_DIR/dm_imu_alone/build"
cp dm_imu_alone/build/dm_imu "$BUILD_ROOT/$INSTALL_DIR/dm_imu_alone/build/"

mkdir -p "$BUILD_ROOT/$INSTALL_DIR/encoder_refactor/build"
cp encoder_refactor/build/main "$BUILD_ROOT/$INSTALL_DIR/encoder_refactor/build/"
cp encoder_refactor/build/zeroing "$BUILD_ROOT/$INSTALL_DIR/encoder_refactor/build/"

# 3. 部署自动更新脚本到系统路径
cp auto_update/usb_auto_update.sh "$BUILD_ROOT/usr/local/bin/usb_auto_update.sh"
chmod +x "$BUILD_ROOT/usr/local/bin/usb_auto_update.sh"

# 4. 部署 Udev 规则
cp camera_record/99-fixed-usb-map.rules "$BUILD_ROOT/etc/udev/rules.d/"
cp encoder_refactor/99-serial.rules "$BUILD_ROOT/etc/udev/rules.d/"
cp auto_update/99-usb-auto-update.rules "$BUILD_ROOT/etc/udev/rules.d/"

echo "=== [4/5] 处理配置脚本与变量替换 ==="

# 1. 拷贝 Systemd Service
cp "$PACK_SCRIPT_DIR/ugripper.service" "$BUILD_ROOT/etc/systemd/system/${APP_NAME}.service"
cp "auto_update/usb-auto-update@.service" "$BUILD_ROOT/etc/systemd/system/usb-auto-update@.service"
cp "auto_calibration/ugripper-calibration.service" "$BUILD_ROOT/etc/systemd/system/ugripper-calibration.service"
cp "auto_calibration/ugripper-network-monitor.service" "$BUILD_ROOT/etc/systemd/system/ugripper-network-monitor.service"

# 2. 拷贝 DEBIAN 控制文件
cp "$PACK_SCRIPT_DIR/control"  "$BUILD_ROOT/DEBIAN/"
cp "$PACK_SCRIPT_DIR/postinst" "$BUILD_ROOT/DEBIAN/"
cp "$PACK_SCRIPT_DIR/prerm"    "$BUILD_ROOT/DEBIAN/"
cp "$PACK_SCRIPT_DIR/postrm"   "$BUILD_ROOT/DEBIAN/"

# 3. 赋予脚本执行权限
chmod 755 "$BUILD_ROOT/DEBIAN/postinst"
chmod 755 "$BUILD_ROOT/DEBIAN/prerm"
chmod 755 "$BUILD_ROOT/DEBIAN/postrm"

# 4. 执行变量替换 (sed)
#    将脚本文件中的 {{APP_NAME}} 等占位符替换为当前脚本开头定义的变量
echo "--> Injecting variables into scripts..."
FILES_TO_PATCH=(
    "$BUILD_ROOT/DEBIAN/control"
    "$BUILD_ROOT/DEBIAN/postinst"
    "$BUILD_ROOT/DEBIAN/prerm"
    "$BUILD_ROOT/DEBIAN/postrm"
    "$BUILD_ROOT/etc/systemd/system/${APP_NAME}.service"
)

for file in "${FILES_TO_PATCH[@]}"; do
    sed -i "s|{{APP_NAME}}|$APP_NAME|g" "$file"
    sed -i "s|{{VERSION}}|$VERSION|g" "$file"
    sed -i "s|{{ARCH}}|$ARCH|g" "$file"
    sed -i "s|{{INSTALL_DIR}}|$INSTALL_DIR|g" "$file"
done

echo "=== [5/5] 生成 DEB 包 ==="
dpkg-deb --build "$BUILD_ROOT" "${APP_NAME}_${VERSION}_${ARCH}.deb"

echo "Build Success: ${APP_NAME}_${VERSION}_${ARCH}.deb"
