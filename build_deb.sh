#!/bin/bash
set -euo pipefail
script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# ================= 变量定义区域 =================
APP_NAME="ugripper"
BASE_VERSION="${BASE_VERSION:-2.0.8}"
VERSION_SUFFIX="${VERSION_SUFFIX:-}"
VERSION="${VERSION:-${BASE_VERSION}${VERSION_SUFFIX}}"
ARCH="arm64"
INSTALL_DIR="/opt/${APP_NAME}"
BUILD_ROOT="temp_build_deb"
BUILD_AUX_ROOT="${BUILD_ROOT}_aux"
PACK_SCRIPT_DIR="pack_script"

# 默认行为变量
QUICK_MODE=false
DPKG_DEB_COMPRESSOR="${DPKG_DEB_COMPRESSOR:-}"
DPKG_DEB_LEVEL="${DPKG_DEB_LEVEL:-}"
DPKG_DEB_STRATEGY="${DPKG_DEB_STRATEGY:-}"
DPKG_DEB_UNIFORM_COMPRESSION="${DPKG_DEB_UNIFORM_COMPRESSION:-}"
PACKAGED_VENV_SOURCE="${PACKAGED_VENV_SOURCE:-.venv}"
DEFAULT_PACKAGED_VENV_URL="http://nexus.dmrobot.com:8081/repository/dmrobot_raw_hosted/ugripper-v2-uv-venv/py311-v1/ugripper_venv_20260423_143813.tar.gz"
PACKAGED_VENV_URL="${PACKAGED_VENV_URL-$DEFAULT_PACKAGED_VENV_URL}"
PACKAGED_BUILD_DIR="${PACKAGED_BUILD_DIR:-build}"
TARGET_INSTALL_ROOT=""
ASSEMBLE_DURATION=0
VENV_DURATION=0
PACKAGE_DURATION=0
RESOLVED_PACKAGED_VENV_SOURCE="$PACKAGED_VENV_SOURCE"

require_host_tool() {
    local tool_name="$1"
    if ! command -v "$tool_name" >/dev/null 2>&1; then
        echo "❌ Required command not found: $tool_name" >&2
        exit 1
    fi
}

log_duration() {
    local label="$1"
    local start="$2"
    local duration=$((SECONDS - start))
    printf -- "--> %s: %ss\n" "$label" "$duration"
}

resolve_expected_venv_machine_pattern() {
    case "$ARCH" in
        amd64)
            printf '%s\n' 'x86-64|x86_64'
            ;;
        arm64)
            printf '%s\n' 'aarch64|arm64'
            ;;
        *)
            echo "Unsupported package architecture for .venv validation: $ARCH" >&2
            exit 1
            ;;
    esac
}

validate_packaged_venv() {
    local source_venv="$1"
    local python_bin="$source_venv/bin/python3"
    local file_output=""
    local expected_pattern=""

    if [ ! -d "$source_venv" ]; then
        return 0
    fi

    if [ ! -e "$python_bin" ]; then
        echo "❌ Packaged .venv is missing python3 entrypoint: $python_bin" >&2
        exit 1
    fi

    require_host_tool file

    file_output="$(file -L "$python_bin")"
    expected_pattern="$(resolve_expected_venv_machine_pattern)"
    if ! printf '%s\n' "$file_output" | grep -Eiq "$expected_pattern"; then
        echo "❌ Packaged .venv architecture mismatch for package arch: $ARCH" >&2
        echo "   - source: $source_venv" >&2
        echo "   - python: $python_bin" >&2
        echo "   - file:   $file_output" >&2
        echo "   - action: build or copy an architecture-matching .venv, then retry packaging." >&2
        exit 1
    fi
}

validate_binary_arch() {
    local binary_path="$1"
    local description="$2"
    local file_output=""
    local expected_pattern=""

    if [ ! -x "$binary_path" ]; then
        echo "❌ Missing required executable for packaging: $description ($binary_path)" >&2
        exit 1
    fi

    require_host_tool file
    file_output="$(file -L "$binary_path")"
    expected_pattern="$(resolve_expected_venv_machine_pattern)"
    if ! printf '%s\n' "$file_output" | grep -Eiq "$expected_pattern"; then
        echo "❌ Executable architecture mismatch for package arch: $ARCH" >&2
        echo "   - target: $description" >&2
        echo "   - path:   $binary_path" >&2
        echo "   - file:   $file_output" >&2
        exit 1
    fi
}

validate_packaged_binaries() {
    validate_binary_arch "$PACKAGED_BUILD_DIR/standalone/CameraRecorder/CameraRecorder" "CameraRecorder"
    validate_binary_arch "$PACKAGED_BUILD_DIR/standalone/CameraRecorder/main_camera_xu_tool" "CameraRecorder main_camera_xu_tool"
    validate_binary_arch "$PACKAGED_BUILD_DIR/standalone/SensorRecorder/SensorRecorder" "SensorRecorder"
    validate_binary_arch "$PACKAGED_BUILD_DIR/standalone/SensorRecorder/zeroing" "SensorRecorder zeroing"
    validate_binary_arch "$PACKAGED_BUILD_DIR/standalone/GripperHmiTool/GripperHmiTool" "GripperHmiTool"
    validate_binary_arch "$PACKAGED_BUILD_DIR/standalone/UgripperRuntime/UgripperRuntime" "UgripperRuntime"
    validate_binary_arch "$PACKAGED_BUILD_DIR/standalone/FaysStereoRecorder/fays_record_example" "Fays stereo recorder"
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

copy_first_existing() {
    local target="$1"
    shift

    local source=""
    for source in "$@"; do
        if [ -e "$source" ]; then
            copy_if_exists "$source" "$target"
            return 0
        fi
    done

    rm -rf "$target"
    return 0
}

has_any_existing() {
    local source=""
    for source in "$@"; do
        if [ -e "$source" ]; then
            return 0
        fi
    done
    return 1
}

sync_project_tree() {
    echo "--> Syncing package whitelist..."
    copy_if_exists "run_record.sh" "$TARGET_INSTALL_ROOT/run_record.sh"
    copy_if_exists "scripts" "$TARGET_INSTALL_ROOT/scripts"
    copy_if_exists "auto_calibration" "$TARGET_INSTALL_ROOT/auto_calibration"
    copy_if_exists "auto_update/trigger_shutdown.sh" "$TARGET_INSTALL_ROOT/auto_update/trigger_shutdown.sh"
    find "$TARGET_INSTALL_ROOT" \
        \( -type d -name '__pycache__' -o -type f -name '*.pyc' \) \
        -exec rm -rf {} +
}

clean_target_install_root() {
    local path=""
    local name=""

    ensure_dir "$TARGET_INSTALL_ROOT"
    shopt -s dotglob nullglob
    for path in "$TARGET_INSTALL_ROOT"/*; do
        name="$(basename "$path")"
        if [ "$name" = ".venv" ]; then
            continue
        fi
        rm -rf "$path"
    done
    shopt -u dotglob nullglob
}

resolve_downloaded_venv_dir() {
    local extract_dir="$1"
    local -a candidates=()
    local candidate=""
    local marker=""

    if [ -x "$extract_dir/bin/python3" ]; then
        printf '%s\n' "$extract_dir"
        return 0
    fi

    if [ -x "$extract_dir/.venv/bin/python3" ]; then
        printf '%s\n' "$extract_dir/.venv"
        return 0
    fi

    while IFS= read -r marker; do
        candidate="$(dirname "$(dirname "$marker")")"
        candidates+=("$candidate")
    done < <(find "$extract_dir" -mindepth 2 -maxdepth 4 -path '*/bin/python3' -type f | sort)

    if [ "${#candidates[@]}" -eq 1 ]; then
        printf '%s\n' "${candidates[0]}"
        return 0
    fi

    if [ "${#candidates[@]}" -eq 0 ]; then
        echo "❌ Failed to locate extracted .venv root under $extract_dir" >&2
        return 1
    fi

    echo "❌ Multiple extracted .venv candidates found under $extract_dir" >&2
    printf '   - %s\n' "${candidates[@]}" >&2
    return 1
}

prepare_packaged_venv_source() {
    local archive_name=""
    local archive_path=""
    local extract_dir=""
    local resolved_dir=""

    RESOLVED_PACKAGED_VENV_SOURCE="$PACKAGED_VENV_SOURCE"

    if [ -z "$PACKAGED_VENV_URL" ]; then
        return 0
    fi

    if ! command -v curl >/dev/null 2>&1; then
        echo "❌ Required command not found for PACKAGED_VENV_URL: curl" >&2
        exit 1
    fi

    if ! command -v tar >/dev/null 2>&1; then
        echo "❌ Required command not found for PACKAGED_VENV_URL: tar" >&2
        exit 1
    fi

    archive_name="${PACKAGED_VENV_URL##*/}"
    archive_name="${archive_name%%\?*}"
    if [ -z "$archive_name" ]; then
        archive_name="packaged_venv.tar.gz"
    fi

    archive_path="$BUILD_AUX_ROOT/.packaged_venv_download/$archive_name"
    extract_dir="$BUILD_AUX_ROOT/.packaged_venv_extract"

    rm -rf "$BUILD_AUX_ROOT/.packaged_venv_download" "$extract_dir"
    ensure_dir "$(dirname "$archive_path")"
    ensure_dir "$extract_dir"

    echo "--> Downloading packaged .venv from $PACKAGED_VENV_URL ..."
    curl -fL --retry 3 --retry-delay 2 -o "$archive_path" "$PACKAGED_VENV_URL"
    echo "--> Extracting packaged .venv archive..."
    tar -xf "$archive_path" -C "$extract_dir"

    resolved_dir="$(resolve_downloaded_venv_dir "$extract_dir")"
    RESOLVED_PACKAGED_VENV_SOURCE="$resolved_dir"
    echo "--> Using downloaded packaged .venv from $RESOLVED_PACKAGED_VENV_SOURCE"
}

sync_packaged_venv() {
    local start="$SECONDS"
    local target_venv="$TARGET_INSTALL_ROOT/.venv"
    local source_venv="$RESOLVED_PACKAGED_VENV_SOURCE"

    if [ ! -d "$source_venv" ]; then
        echo "--> Packaged .venv source not found at $source_venv; removing stale staged copy if present."
        rm -rf "$target_venv"
        VENV_DURATION=$((SECONDS - start))
        return 0
    fi

    echo "--> Incremental sync packaged .venv from $source_venv..."
    validate_packaged_venv "$source_venv"
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
           "$BUILD_ROOT/.packaged_venv_download" \
           "$BUILD_ROOT/.packaged_venv_extract" \
           "$BUILD_ROOT/.auto_release_ugripper.manifest" \
           "$BUILD_AUX_ROOT"

    ensure_dir "$BUILD_ROOT/DEBIAN"
    ensure_dir "$TARGET_INSTALL_ROOT"
    ensure_dir "$BUILD_ROOT/usr/local/bin"
    ensure_dir "$BUILD_ROOT/etc/systemd/system"
    ensure_dir "$BUILD_ROOT/etc/udev/rules.d"
    clean_target_install_root
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
    echo "   - 执行主包所需目标编译"
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
    validate_packaged_binaries
else
    echo "--> Building package-required C++ modules..."
    rm -rf "$PACKAGED_BUILD_DIR"
    cmake -S . -B "$PACKAGED_BUILD_DIR" \
        -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_TESTING=OFF \
        -DUGRIPPER_ENABLE_MCAP_BUILDER=OFF
    cmake --build "$PACKAGED_BUILD_DIR" \
        --target CameraRecorder main_camera_xu_tool SensorRecorder zeroing GripperHmiTool UgripperRuntime fays_record_example \
        --parallel "$(nproc)"
    validate_packaged_binaries
fi

echo "=== [3/5] 组装文件资源 ==="
assemble_start="$SECONDS"

# 1. 拷贝项目主体文件
sync_project_tree
prepare_packaged_venv_source
sync_packaged_venv
ASSEMBLE_DURATION=$((SECONDS - assemble_start - VENV_DURATION))
log_duration "Project staging sync (including .venv pass)" "$assemble_start"

# 2. 手动补回编译好的二进制文件与 standalone runtime 资源
echo "--> Restoring compiled standalone payload..."
copy_first_existing "$TARGET_INSTALL_ROOT/bin/SensorRecorder/SensorRecorder" \
    "$PACKAGED_BUILD_DIR/standalone/SensorRecorder/SensorRecorder"
copy_first_existing "$TARGET_INSTALL_ROOT/bin/SensorRecorder/zeroing" \
    "$PACKAGED_BUILD_DIR/standalone/SensorRecorder/zeroing"
copy_first_existing "$TARGET_INSTALL_ROOT/bin/SensorRecorder/99-serial.rules" \
    "standalone/SensorRecorder/99-serial.rules"
copy_first_existing "$TARGET_INSTALL_ROOT/bin/CameraRecorder/CameraRecorder" \
    "$PACKAGED_BUILD_DIR/standalone/CameraRecorder/CameraRecorder"
copy_first_existing "$TARGET_INSTALL_ROOT/bin/CameraRecorder/main_camera_xu_tool" \
    "$PACKAGED_BUILD_DIR/standalone/CameraRecorder/main_camera_xu_tool"
copy_first_existing "$TARGET_INSTALL_ROOT/bin/GripperHmiTool/GripperHmiTool" \
    "$PACKAGED_BUILD_DIR/standalone/GripperHmiTool/GripperHmiTool"
copy_first_existing "$TARGET_INSTALL_ROOT/bin/UgripperRuntime/UgripperRuntime" \
    "$PACKAGED_BUILD_DIR/standalone/UgripperRuntime/UgripperRuntime"
copy_first_existing "$TARGET_INSTALL_ROOT/bin/FaysStereoRecorder/fays_record_example" \
    "$PACKAGED_BUILD_DIR/standalone/FaysStereoRecorder/fays_record_example"
copy_if_exists "scripts/hws" "$BUILD_ROOT/usr/local/bin/hws"

copy_if_exists "standalone/CameraRecorder/config" "$TARGET_INSTALL_ROOT/bin/CameraRecorder/config"
copy_if_exists "$PACKAGED_BUILD_DIR/standalone/FaysStereoRecorder/config" \
    "$TARGET_INSTALL_ROOT/bin/FaysStereoRecorder/config"
copy_if_exists "$PACKAGED_BUILD_DIR/standalone/FaysStereoRecorder/scripts" \
    "$TARGET_INSTALL_ROOT/bin/FaysStereoRecorder/scripts"
copy_if_exists "$PACKAGED_BUILD_DIR/standalone/FaysStereoRecorder/lib" \
    "$TARGET_INSTALL_ROOT/bin/FaysStereoRecorder/lib"
copy_if_exists "config/ensure_main_camera_packet_size_once.sh" \
    "$TARGET_INSTALL_ROOT/config/ensure_main_camera_packet_size_once.sh"
copy_if_exists "standalone/UgripperRuntime/audio" "$TARGET_INSTALL_ROOT/bin/UgripperRuntime/audio"
copy_if_exists "standalone/UgripperRuntime/audio_en" "$TARGET_INSTALL_ROOT/bin/UgripperRuntime/audio_en"
copy_if_exists "standalone/UgripperRuntime/ego" "$TARGET_INSTALL_ROOT/bin/UgripperRuntime/ego"
rm -rf "$TARGET_INSTALL_ROOT/bin/UgripperRuntime/ego/__pycache__"
copy_if_exists "standalone/UgripperRuntime/adb" "$TARGET_INSTALL_ROOT/bin/UgripperRuntime/adb"
copy_if_exists "standalone/UgripperRuntime/config/fakeCamCalib.json" \
    "$TARGET_INSTALL_ROOT/bin/UgripperRuntime/config/fakeCamCalib.json"

# py_script is excluded from rsync by default; restore required runtime/test scripts explicitly.
copy_if_exists "py_script/usb_audio_play_test.py" "$TARGET_INSTALL_ROOT/py_script/usb_audio_play_test.py"
copy_if_exists "py_script/usb_audio_mic_test.py" "$TARGET_INSTALL_ROOT/py_script/usb_audio_mic_test.py"
copy_if_exists "py_script/usb_audio_noise_profile.py" "$TARGET_INSTALL_ROOT/py_script/usb_audio_noise_profile.py"
copy_if_exists "py_script/fays_tail_imu_check.py" "$TARGET_INSTALL_ROOT/py_script/fays_tail_imu_check.py"

# 4. 部署 Udev 规则
copy_if_exists "config/99-fixed-usb-map.rules" "$BUILD_ROOT/etc/udev/rules.d/99-fixed-usb-map.rules"
copy_first_existing "$BUILD_ROOT/etc/udev/rules.d/99-serial.rules" \
    "standalone/SensorRecorder/99-serial.rules"
echo "--> .venv sync: ${VENV_DURATION}s"

echo "=== [4/5] 处理配置脚本与变量替换 ==="
# 1. 拷贝 Systemd Service
copy_if_exists "$PACK_SCRIPT_DIR/ugripper.service" "$BUILD_ROOT/etc/systemd/system/${APP_NAME}.service"
copy_if_exists "auto_calibration/ugripper-calibration.service" "$BUILD_ROOT/etc/systemd/system/ugripper-calibration.service"
copy_if_exists "auto_calibration/ugripper-network-monitor.service" "$BUILD_ROOT/etc/systemd/system/ugripper-network-monitor.service"
copy_if_exists "auto_update/umi-shutdown-trigger.service" "$BUILD_ROOT/etc/systemd/system/umi-shutdown-trigger.service"
copy_if_exists "auto_update/umi-shutdown-trigger.path" "$BUILD_ROOT/etc/systemd/system/umi-shutdown-trigger.path"
copy_if_exists "time_sync/ugripper-ntp-sync.service" "$BUILD_ROOT/etc/systemd/system/ugripper-ntp-sync.service"
copy_if_exists "time_sync/safe_ntp_sync.sh" "$TARGET_INSTALL_ROOT/time_sync/safe_ntp_sync.sh"

# 2. 拷贝 DEBIAN 控制文件
copy_if_exists "$PACK_SCRIPT_DIR/control"  "$BUILD_ROOT/DEBIAN/control"
copy_if_exists "$PACK_SCRIPT_DIR/postinst" "$BUILD_ROOT/DEBIAN/postinst"
copy_if_exists "$PACK_SCRIPT_DIR/prerm"    "$BUILD_ROOT/DEBIAN/prerm"
copy_if_exists "$PACK_SCRIPT_DIR/postrm"   "$BUILD_ROOT/DEBIAN/postrm"

# 3. 赋予脚本执行权限
chmod 755 "$BUILD_ROOT/DEBIAN/postinst"
chmod 755 "$BUILD_ROOT/DEBIAN/prerm"
chmod 755 "$BUILD_ROOT/DEBIAN/postrm"
chmod 755 "$BUILD_ROOT/usr/local/bin/hws"
chmod 755 "$TARGET_INSTALL_ROOT/time_sync/safe_ntp_sync.sh"

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
rm -rf "$BUILD_AUX_ROOT"

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
