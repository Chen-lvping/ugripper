#!/bin/bash
set -euo pipefail
script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# ================= 变量定义区域 =================
APP_NAME="ugripper"
VERSION="1.2.11"       # 每次发布前修改这里
ARCH="arm64"
INSTALL_DIR="/opt/${APP_NAME}"
BUILD_ROOT="temp_build_deb"
PACK_SCRIPT_DIR="pack_script"

# 默认行为变量
QUICK_MODE=false
DPKG_DEB_COMPRESSOR="${DPKG_DEB_COMPRESSOR:-}"
DPKG_DEB_LEVEL="${DPKG_DEB_LEVEL:-}"
DPKG_DEB_STRATEGY="${DPKG_DEB_STRATEGY:-}"
DPKG_DEB_UNIFORM_COMPRESSION="${DPKG_DEB_UNIFORM_COMPRESSION:-}"
PACKAGED_VENV_SOURCE="${PACKAGED_VENV_SOURCE:-.venv}"
PACKAGED_BUILD_DIR="${PACKAGED_BUILD_DIR:-build}"
TARGET_INSTALL_ROOT=""
ASSEMBLE_DURATION=0
VENV_DURATION=0
PACKAGE_DURATION=0

log_duration() {
    local label="$1"
    local start="$2"
    local duration=$((SECONDS - start))
    printf -- "--> %s: %ss\n" "$label" "$duration"
}

ensure_dir() {
    mkdir -p "$1"
}

copy_if_exists() {
    local source="$1"
    local target="$2"

    if [ ! -e "$source" ]; then
        rm -f "$target"
        return 0
    fi

    ensure_dir "$(dirname "$target")"
    if [ -e "$target" ] && cmp -s "$source" "$target"; then
        return 0
    fi

    cp -a "$source" "$target"
}

sync_project_tree() {
    echo "--> Incremental sync project files..."
    rm -rf "$TARGET_INSTALL_ROOT/.uv"
    rsync -a --delete "${EXCLUDE_LIST[@]}" ./ "$TARGET_INSTALL_ROOT/"
}

sync_packaged_venv() {
    local start="$SECONDS"
    local target_venv="$TARGET_INSTALL_ROOT/.venv"
    local source_venv="$PACKAGED_VENV_SOURCE"

    if [ ! -d "$source_venv" ]; then
        echo "--> Packaged .venv source not found at $source_venv; removing stale staged copy if present."
        rm -rf "$target_venv"
        VENV_DURATION=$((SECONDS - start))
        return 0
    fi

    echo "--> Incremental sync packaged .venv from $source_venv..."
    ensure_dir "$target_venv"
    # Keep .venv in a dedicated rsync pass so unchanged interpreter files can be reused.
    rsync -a --delete "$source_venv"/ "$target_venv"/
    VENV_DURATION=$((SECONDS - start))
}

prepare_generated_dirs() {
    rm -rf "$BUILD_ROOT/DEBIAN" \
           "$BUILD_ROOT/usr/local/bin" \
           "$BUILD_ROOT/etc/systemd/system" \
           "$BUILD_ROOT/etc/udev/rules.d" \
           "$TARGET_INSTALL_ROOT/build" \
           "$TARGET_INSTALL_ROOT/py_script"

    ensure_dir "$BUILD_ROOT/DEBIAN"
    ensure_dir "$TARGET_INSTALL_ROOT"
    ensure_dir "$BUILD_ROOT/usr/local/bin"
    ensure_dir "$BUILD_ROOT/etc/systemd/system"
    ensure_dir "$BUILD_ROOT/etc/udev/rules.d"
}

resolve_dpkg_deb_args() {
    DPKG_DEB_BUILD_ARGS=(--root-owner-group)

    if [ -z "$DPKG_DEB_COMPRESSOR" ]; then
        DPKG_DEB_COMPRESSOR="xz"
    fi
    if [ -z "$DPKG_DEB_LEVEL" ]; then
        DPKG_DEB_LEVEL="1"
    fi

    if [ -n "$DPKG_DEB_COMPRESSOR" ]; then
        DPKG_DEB_BUILD_ARGS+=("-Z${DPKG_DEB_COMPRESSOR}")
    fi
    if [ -n "$DPKG_DEB_LEVEL" ]; then
        DPKG_DEB_BUILD_ARGS+=("-z${DPKG_DEB_LEVEL}")
    fi
    if [ -n "$DPKG_DEB_STRATEGY" ]; then
        DPKG_DEB_BUILD_ARGS+=("-S${DPKG_DEB_STRATEGY}")
    fi
    case "$DPKG_DEB_UNIFORM_COMPRESSION" in
        true|1|yes)
            DPKG_DEB_BUILD_ARGS+=(--uniform-compression)
            ;;
        false|0|no|"")
            ;;
        *)
            echo "Invalid DPKG_DEB_UNIFORM_COMPRESSION value: $DPKG_DEB_UNIFORM_COMPRESSION" >&2
            exit 1
            ;;
    esac
}

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
    echo "   - 复用临时构建目录，按 rsync 增量同步 staging"
    echo "   - 跳过 C++ 编译"
    echo "   - 排除 .env，增量复用已维护好的 .venv"
else
    echo "🐢 标准构建模式 (Standard Mode)"
    echo "   - 执行全量编译"
    echo "   - 打包 staging 改为增量复用，仅覆盖变化内容"
    echo "   - 默认使用 dpkg-deb xz -1 压缩口径"
fi

# ===============================================

TARGET_INSTALL_ROOT="$BUILD_ROOT/$INSTALL_DIR"
resolve_dpkg_deb_args

echo "=== [1/5] 初始化构建环境 ==="
# 始终清理旧的 .deb 文件
rm -f "${APP_NAME}_${VERSION}_${ARCH}.deb"
echo "--> Reusing build root: $BUILD_ROOT"
prepare_generated_dirs

echo "=== [2/5] 编译 C++ 模块 (统一构建) ==="

if [ "$QUICK_MODE" = true ]; then
    echo "--> [SKIP] Skipping C++ compilation."
    if [ ! -f "$PACKAGED_BUILD_DIR/src/sensor_recorder/sensor_recorder" ] || \
       [ ! -f "$PACKAGED_BUILD_DIR/src/sensor_recorder/zeroing" ] || \
       [ ! -f "$PACKAGED_BUILD_DIR/src/camera_recorder/camera_recorder" ] || \
       [ ! -f "$PACKAGED_BUILD_DIR/src/record_runtime/record_runtime" ]; then
        echo "⚠️  警告: 核心 C++ 二进制缺失！打包可能不可用。"
    fi
    if [ ! -f "$PACKAGED_BUILD_DIR/src/gripper_hmi/gripper_hmi_test" ]; then
        echo "ℹ️  提示: gripper_hmi_test 未生成，包内将缺少该测试工具。"
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
assemble_start="$SECONDS"

# 定义基础排除项
EXCLUDE_LIST=(
    --exclude='ASR'
    --exclude='py_script'
    --exclude='.git'
    --exclude='build_deb.sh'
    --exclude='pack_script'
    --exclude="$BUILD_ROOT"
    --exclude='build'
    --exclude='.venv'
    --exclude='.uv'
    --exclude='ref_src'
    --exclude='src/sensor_recorder'
    --exclude='src/camera_recorder'
    --exclude='*.deb'
)

# --- 快速模式特有的智能排除逻辑 ---
if [ "$QUICK_MODE" = true ]; then
    # 1. 排除 .env (防止覆盖配置)
    echo "--> [Exclude] Skipping .env (preserve config)"
    EXCLUDE_LIST+=( --exclude='.env' )
fi

# 1. 拷贝项目主体文件
sync_project_tree
sync_packaged_venv
ASSEMBLE_DURATION=$((SECONDS - assemble_start - VENV_DURATION))
log_duration "Project staging sync (including .venv pass)" "$assemble_start"

# 2. 手动补回编译好的二进制文件 (from unified build/ directory)
echo "--> Restoring compiled binaries..."
copy_if_exists "$PACKAGED_BUILD_DIR/src/sensor_recorder/sensor_recorder" "$TARGET_INSTALL_ROOT/build/src/sensor_recorder/sensor_recorder"
copy_if_exists "$PACKAGED_BUILD_DIR/src/sensor_recorder/zeroing" "$TARGET_INSTALL_ROOT/build/src/sensor_recorder/zeroing"
copy_if_exists "$PACKAGED_BUILD_DIR/src/camera_recorder/camera_recorder" "$TARGET_INSTALL_ROOT/build/src/camera_recorder/camera_recorder"
copy_if_exists "$PACKAGED_BUILD_DIR/src/gripper_hmi/gripper_hmi_test" "$TARGET_INSTALL_ROOT/build/src/gripper_hmi/gripper_hmi_test"
copy_if_exists "$PACKAGED_BUILD_DIR/src/record_runtime/record_runtime" "$TARGET_INSTALL_ROOT/build/src/record_runtime/record_runtime"

# py_script is excluded from rsync by default; restore required runtime/test scripts explicitly.
copy_if_exists "py_script/usb_audio_play_test.py" "$TARGET_INSTALL_ROOT/py_script/usb_audio_play_test.py"
copy_if_exists "py_script/usb_audio_mic_test.py" "$TARGET_INSTALL_ROOT/py_script/usb_audio_mic_test.py"
copy_if_exists "py_script/usb_audio_noise_profile.py" "$TARGET_INSTALL_ROOT/py_script/usb_audio_noise_profile.py"

# 4. 部署 Udev 规则
copy_if_exists "config/99-fixed-usb-map.rules" "$BUILD_ROOT/etc/udev/rules.d/99-fixed-usb-map.rules"
copy_if_exists "src/sensor_recorder/99-serial.rules" "$BUILD_ROOT/etc/udev/rules.d/99-serial.rules"
echo "--> .venv sync: ${VENV_DURATION}s"

echo "=== [4/5] 处理配置脚本与变量替换 ==="
# 1. 拷贝 Systemd Service
copy_if_exists "$PACK_SCRIPT_DIR/ugripper.service" "$BUILD_ROOT/etc/systemd/system/${APP_NAME}.service"
copy_if_exists "auto_calibration/ugripper-calibration.service" "$BUILD_ROOT/etc/systemd/system/ugripper-calibration.service"
copy_if_exists "auto_calibration/ugripper-network-monitor.service" "$BUILD_ROOT/etc/systemd/system/ugripper-network-monitor.service"
copy_if_exists "auto_update/umi-shutdown-trigger.service" "$BUILD_ROOT/etc/systemd/system/umi-shutdown-trigger.service"
copy_if_exists "auto_update/umi-shutdown-trigger.path" "$BUILD_ROOT/etc/systemd/system/umi-shutdown-trigger.path"

# 2. 拷贝 DEBIAN 控制文件
copy_if_exists "$PACK_SCRIPT_DIR/control"  "$BUILD_ROOT/DEBIAN/control"
copy_if_exists "$PACK_SCRIPT_DIR/postinst" "$BUILD_ROOT/DEBIAN/postinst"
copy_if_exists "$PACK_SCRIPT_DIR/prerm"    "$BUILD_ROOT/DEBIAN/prerm"
copy_if_exists "$PACK_SCRIPT_DIR/postrm"   "$BUILD_ROOT/DEBIAN/postrm"

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
package_start="$SECONDS"
echo "--> dpkg-deb args: ${DPKG_DEB_BUILD_ARGS[*]}"
dpkg-deb "${DPKG_DEB_BUILD_ARGS[@]}" --build "$BUILD_ROOT" "${APP_NAME}_${VERSION}_${ARCH}.deb"
PACKAGE_DURATION=$((SECONDS - package_start))
log_duration "dpkg-deb build" "$package_start"

# Refresh source baseline manifest for auto-release-deb diff detection.
MANIFEST_WRITER=".codex/skills/auto-release-deb/scripts/write_source_manifest.sh"
if [ -x "$MANIFEST_WRITER" ]; then
    bash "$MANIFEST_WRITER" --target ugripper --output "$BUILD_ROOT/.auto_release_ugripper.manifest" || true
fi

echo "Build Success: ${APP_NAME}_${VERSION}_${ARCH}.deb"
if [ "$QUICK_MODE" = true ]; then
    echo "Note: Quick Mode used. .env excluded. Maintained .venv synced incrementally into package."
fi
echo "Timing summary:"
echo "  - project staging sync: ${ASSEMBLE_DURATION}s"
echo "  - packaged .venv sync: ${VENV_DURATION}s"
echo "  - dpkg-deb build: ${PACKAGE_DURATION}s"
