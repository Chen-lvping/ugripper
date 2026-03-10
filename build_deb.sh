#!/bin/bash
set -e  # 遇到错误立即停止
script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# ================= 变量定义区域 =================
APP_NAME="ugripper"
VERSION="1.1.13"       # 每次发布前修改这里
ARCH="arm64"
INSTALL_DIR="/opt/${APP_NAME}"
BUILD_ROOT="temp_build_deb"
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

echo "=== [2/5] 编译 C++ 模块 (统一构建) ==="

if [ "$QUICK_MODE" = true ]; then
    echo "--> [SKIP] Skipping C++ compilation."
    if [ ! -f "build/src/sensor_recorder/sensor_recorder" ] || \
       [ ! -f "build/src/sensor_recorder/zeroing" ] || \
       [ ! -f "build/faysSense_vi_kit/fays_record_example" ]; then
        echo "⚠️  警告: 核心 C++ 二进制缺失！打包可能不可用。"
    fi
else
    echo "--> Building C++ modules (root CMake)..."
    rm -rf build
    mkdir -p build
    cd build
    cmake .. -DCMAKE_BUILD_TYPE=Release
    make -j$(nproc)
    cd ..
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
    --exclude='faysSense_vi_kit'
    --exclude='build'
    --exclude='src/sensor_recorder'
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

# 2. 手动补回编译好的二进制文件 (from unified build/ directory)
echo "--> Restoring compiled binaries..."
mkdir -p "$BUILD_ROOT/$INSTALL_DIR/build/faysSense_vi_kit/scripts"
mkdir -p "$BUILD_ROOT/$INSTALL_DIR/build/faysSense_vi_kit/config"
mkdir -p "$BUILD_ROOT/$INSTALL_DIR/build/faysSense_vi_kit/lib"
cp build/faysSense_vi_kit/fays_record_example "$BUILD_ROOT/$INSTALL_DIR/build/faysSense_vi_kit/" || true
# Use source scripts so quick mode still gets latest runtime fixes.
cp faysSense_vi_kit/scripts/*.sh "$BUILD_ROOT/$INSTALL_DIR/build/faysSense_vi_kit/scripts/" || true
cp build/faysSense_vi_kit/config/*.yaml "$BUILD_ROOT/$INSTALL_DIR/build/faysSense_vi_kit/config/" || true
cp -a faysSense_vi_kit/lib/fays_atrak "$BUILD_ROOT/$INSTALL_DIR/build/faysSense_vi_kit/lib/" || true
chmod +x "$BUILD_ROOT/$INSTALL_DIR/build/faysSense_vi_kit/scripts/"*.sh 2>/dev/null || true
mkdir -p "$BUILD_ROOT/$INSTALL_DIR/build/src/sensor_recorder"
cp build/src/sensor_recorder/sensor_recorder "$BUILD_ROOT/$INSTALL_DIR/build/src/sensor_recorder/" || true
cp build/src/sensor_recorder/zeroing "$BUILD_ROOT/$INSTALL_DIR/build/src/sensor_recorder/" || true

# py_script is excluded from rsync by default; restore required runtime checker explicitly.
mkdir -p "$BUILD_ROOT/$INSTALL_DIR/py_script"
cp py_script/fays_tail_imu_check.py "$BUILD_ROOT/$INSTALL_DIR/py_script/" || true

# 4. 部署 Udev 规则
cp camera_record/99-fixed-usb-map.rules "$BUILD_ROOT/etc/udev/rules.d/" || true
cp src/sensor_recorder/99-serial.rules "$BUILD_ROOT/etc/udev/rules.d/" || true

echo "=== [4/5] 处理配置脚本与变量替换 ==="
# 1. 拷贝 Systemd Service
cp "$PACK_SCRIPT_DIR/ugripper.service" "$BUILD_ROOT/etc/systemd/system/${APP_NAME}.service"
cp "auto_calibration/ugripper-calibration.service" "$BUILD_ROOT/etc/systemd/system/ugripper-calibration.service"
cp "auto_calibration/ugripper-network-monitor.service" "$BUILD_ROOT/etc/systemd/system/ugripper-network-monitor.service"
cp "time_sync/ugripper-ntp-sync.service" "$BUILD_ROOT/etc/systemd/system/ugripper-ntp-sync.service"
cp "time_sync/ugripper-ptp-monitor.service" "$BUILD_ROOT/etc/systemd/system/ugripper-ptp-monitor.service"
cp "auto_update/umi-shutdown-trigger.service" "$BUILD_ROOT/etc/systemd/system/umi-shutdown-trigger.service"
cp "auto_update/umi-shutdown-trigger.path" "$BUILD_ROOT/etc/systemd/system/umi-shutdown-trigger.path"

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
    "$BUILD_ROOT/etc/systemd/system/umi-shutdown-trigger.service"
)

for file in "${FILES_TO_PATCH[@]}"; do
    sed -i "s|{{APP_NAME}}|$APP_NAME|g" "$file"
    sed -i "s|{{VERSION}}|$VERSION|g" "$file"
    sed -i "s|{{ARCH}}|$ARCH|g" "$file"
    sed -i "s|{{INSTALL_DIR}}|$INSTALL_DIR|g" "$file"
done

echo "=== [5/5] 生成 DEB 包 ==="
dpkg-deb --build "$BUILD_ROOT" "${APP_NAME}_${VERSION}_${ARCH}.deb"

# Refresh source baseline manifest for auto-release-deb diff detection.
MANIFEST_WRITER=".codex/skills/auto-release-deb/scripts/write_source_manifest.sh"
if [ -x "$MANIFEST_WRITER" ]; then
    bash "$MANIFEST_WRITER" --target ugripper --output "$BUILD_ROOT/.auto_release_ugripper.manifest" || true
fi

echo "Build Success: ${APP_NAME}_${VERSION}_${ARCH}.deb"
if [ "$QUICK_MODE" = true ]; then
    echo "Note: Quick Mode used. .env excluded. .venv reused if present."
fi
