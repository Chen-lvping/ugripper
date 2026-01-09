#!/bin/bash
set -e  # 遇到错误立即停止
script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# ================= 变量定义区域 =================
APP_NAME="ugripper"
VERSION="1.0.1"       # 每次发布前修改这里
ARCH="arm64"
INSTALL_DIR="/opt/${APP_NAME}"
BUILD_ROOT="build_deb_temp"
PACK_SCRIPT_DIR="pack_script"

# 默认行为变量
QUICK_MODE=false

# ================= 参数解析 =================
for arg in "$@"; do
  case $arg in
    -q|--quick)
      QUICK_MODE=true
      shift 
      ;;
    *)
      ;;
  esac
done

if [ "$QUICK_MODE" = true ]; then
    echo "⚡ 已启用快速构建模式 (Quick Mode)"
    echo "   - 保留临时构建目录 (不执行 rm -rf)"
    echo "   - 跳过 C++ 编译"
    echo "   - 智能排除 .env 和已存在的 .venv"
else
    echo "🐢 标准构建模式 (Standard Mode)"
    echo "   - 清理并重建构建目录"
    echo "   - 执行全量编译"
fi

# ===============================================

echo "=== [1/5] 初始化构建环境 ==="
# 始终清理旧的 .deb 文件
rm -f "${APP_NAME}_${VERSION}_${ARCH}.deb"

if [ "$QUICK_MODE" = true ]; then
    # 快速模式：不删除 BUILD_ROOT，但确保目录结构存在
    echo "--> [Quick Mode] Keeping existing build directory..."
else
    # 标准模式：彻底清理
    echo "--> Cleaning old build directory..."
    rm -rf "$BUILD_ROOT"
fi

# 创建目录结构
mkdir -p "$BUILD_ROOT/DEBIAN"
mkdir -p "$BUILD_ROOT/$INSTALL_DIR"
mkdir -p "$BUILD_ROOT/usr/local/bin"
mkdir -p "$BUILD_ROOT/etc/systemd/system"
mkdir -p "$BUILD_ROOT/etc/udev/rules.d"

echo "=== [2/5] 编译 C++ 模块 ==="

if [ "$QUICK_MODE" = true ]; then
    echo "--> [SKIP] Skipping C++ compilation."
    if [ ! -f "dm_imu_alone/build/dm_imu" ] || [ ! -f "encoder_refactor/build/main" ]; then
        echo "⚠️  警告: 二进制文件缺失！打包可能不可用。"
    fi
else
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
fi

echo "=== [3/5] 组装文件资源 ==="

# 定义基础排除项
EXCLUDE_LIST=(
    --exclude='ASR'
    --exclude='py_script'
    --exclude='.git'
    --exclude='build_deb.sh'
    --exclude='pack_script'
    --exclude="$BUILD_ROOT"
    --exclude='dm_imu_alone'
    --exclude='encoder_refactor'
    --exclude='*.deb'
)

# --- 快速模式特有的智能排除逻辑 ---
if [ "$QUICK_MODE" = true ]; then
    # 1. 排除 .env (防止覆盖配置)
    echo "--> [Exclude] Skipping .env (preserve config)"
    EXCLUDE_LIST+=( --exclude='.env' )

    # 2. 检查临时目录里是否已经有 .venv
    #    路径是: build_deb_temp/opt/ugripper/.venv
    if [ -d "$BUILD_ROOT/$INSTALL_DIR/.venv" ]; then
        echo "--> [Exclude] Found existing .venv in temp dir, skipping copy to save time..."
        EXCLUDE_LIST+=( --exclude='.venv' )
    else
        echo "--> [Info] No .venv found in temp dir, will copy from source..."
    fi
fi

# 1. 拷贝项目主体文件
echo "--> Copying project files..."
rsync -av "${EXCLUDE_LIST[@]}" . "$BUILD_ROOT/$INSTALL_DIR/"

# 2. 手动补回编译好的二进制文件
echo "--> Restoring compiled binaries..."
mkdir -p "$BUILD_ROOT/$INSTALL_DIR/dm_imu_alone/build"
cp dm_imu_alone/build/dm_imu "$BUILD_ROOT/$INSTALL_DIR/dm_imu_alone/build/" || true

mkdir -p "$BUILD_ROOT/$INSTALL_DIR/encoder_refactor/build"
cp encoder_refactor/build/main "$BUILD_ROOT/$INSTALL_DIR/encoder_refactor/build/" || true
cp encoder_refactor/build/zeroing "$BUILD_ROOT/$INSTALL_DIR/encoder_refactor/build/" || true

# 4. 部署 Udev 规则
cp camera_record/99-fixed-usb-map.rules "$BUILD_ROOT/etc/udev/rules.d/" || true
cp encoder_refactor/99-serial.rules "$BUILD_ROOT/etc/udev/rules.d/" || true

echo "=== [4/5] 处理配置脚本与变量替换 ==="
# 1. 拷贝 Systemd Service
cp "$PACK_SCRIPT_DIR/ugripper.service" "$BUILD_ROOT/etc/systemd/system/${APP_NAME}.service"
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

# 4. 执行变量替换
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
if [ "$QUICK_MODE" = true ]; then
    echo "Note: Quick Mode used. .env excluded. .venv reused if present."
fi
