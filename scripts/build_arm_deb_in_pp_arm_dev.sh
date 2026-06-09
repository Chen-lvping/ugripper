#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

APP_NAME="ugripper"
ARCH="arm64"
BASE_VERSION="${BASE_VERSION:-2.0.7}"
VERSION_SUFFIX="${VERSION_SUFFIX:-}"
VERSION="${VERSION:-${BASE_VERSION}${VERSION_SUFFIX}}"
PACKAGE_DEB_NAME="${APP_NAME}_${VERSION}_${ARCH}.deb"

TOOLCHAIN_FILE="${TOOLCHAIN_FILE:-${REPO_ROOT}/cmake/arm-linux-toolchain.cmake}"
PACKAGED_BUILD_DIR="${PACKAGED_BUILD_DIR:-build/arm_container_release}"
PARALLEL="${PARALLEL:-8}"
HOST_UID="${HOST_UID:-$(stat -c %u "${REPO_ROOT}")}"
HOST_GID="${HOST_GID:-$(stat -c %g "${REPO_ROOT}")}"
QUICK_MODE=false
CLEAN_BUILD=true

usage() {
    cat <<'EOF'
Usage:
  ./scripts/build_arm_deb_in_pp_arm_dev.sh [options]

Build the ugripper arm64 package from inside the pp-arm-dev container.

Modes:
  default           Cross-compile required C++ targets, then package with build_deb.sh -q.
  -q, --quick       Skip C++ build and repackage using existing ARM binaries in PACKAGED_BUILD_DIR.

Options:
  --build-dir PATH  Override PACKAGED_BUILD_DIR (default: build/arm_container_release).
  -j, --parallel N  Override CMake build parallelism (default: 8 or PARALLEL env).
  --clean           Remove PACKAGED_BUILD_DIR before a full C++ build (default).
  --no-clean        Reuse PACKAGED_BUILD_DIR during a full C++ build.
  -h, --help        Show this help message.

Common env:
  VERSION=1.2.12 ./scripts/build_arm_deb_in_pp_arm_dev.sh
  VERSION=1.2.12 ./scripts/build_arm_deb_in_pp_arm_dev.sh --quick
  PACKAGED_VENV_URL='' PACKAGED_VENV_SOURCE=/path/to/.venv ./scripts/build_arm_deb_in_pp_arm_dev.sh --quick
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        -q|--quick)
            QUICK_MODE=true
            shift
            ;;
        --build-dir)
            if [[ $# -lt 2 ]]; then
                echo "Missing value for --build-dir" >&2
                exit 1
            fi
            PACKAGED_BUILD_DIR="$2"
            shift 2
            ;;
        -j|--parallel)
            if [[ $# -lt 2 ]]; then
                echo "Missing value for $1" >&2
                exit 1
            fi
            PARALLEL="$2"
            shift 2
            ;;
        --clean)
            CLEAN_BUILD=true
            shift
            ;;
        --no-clean)
            CLEAN_BUILD=false
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "Unknown argument: $1" >&2
            usage >&2
            exit 1
            ;;
    esac
done

is_container_runtime() {
    [[ -f "/.dockerenv" || -f "/run/.containerenv" ]] && return 0
    grep -qaE "(docker|containerd|kubepods|libpod)" /proc/1/cgroup 2>/dev/null
}

if [[ "${ALLOW_HOST_RUN:-0}" != "1" ]] && ! is_container_runtime; then
    cat >&2 <<'EOF'
This script is intended to run inside the ARM build container.

Enter the container first, for example:
  docker exec -it pp-arm-dev bash
  cd /home/dm/proj/pp-main/standalone/UGripper
  ./scripts/build_arm_deb_in_pp_arm_dev.sh

Set ALLOW_HOST_RUN=1 only if you really want to run it outside a container.
EOF
    exit 1
fi

cd "${REPO_ROOT}"

export PKG_CONFIG_PATH="/usr/lib/aarch64-linux-gnu/pkgconfig:${PKG_CONFIG_PATH:-}"

if [[ "${QUICK_MODE}" == true ]]; then
  echo "Quick ARM package mode:"
  echo "  build_dir=${REPO_ROOT}/${PACKAGED_BUILD_DIR}"
  echo "  action=skip C++ cross-build, repackage existing ARM payload"
else
  echo "Full ARM package mode:"
  echo "  build_dir=${REPO_ROOT}/${PACKAGED_BUILD_DIR}"
  echo "  toolchain=${TOOLCHAIN_FILE}"
  echo "  parallel=${PARALLEL}"

  if [[ ! -f ".local-deps/arm-build-env.sh" ]]; then
      ./scripts/setup_arm_build_env.sh
  fi
  source ".local-deps/arm-build-env.sh"

  NEED_SYSROOT_REFRESH=false
  for module in fmt spdlog gstreamer-1.0 gstreamer-app-1.0 gstreamer-base-1.0; do
      if ! pkg-config --exists "${module}"; then
          echo "Missing ARM sysroot pkg-config module: ${module}"
          NEED_SYSROOT_REFRESH=true
      fi
  done
  for lib in \
      "${UGRIPPER_ARM_SYSROOT}/usr/lib/aarch64-linux-gnu/libopencv_core.so" \
      "${UGRIPPER_ARM_SYSROOT}/usr/lib/aarch64-linux-gnu/libopencv_imgproc.so"; do
      if [[ ! -f "${lib}" ]]; then
          echo "Missing ARM sysroot library: ${lib}"
          NEED_SYSROOT_REFRESH=true
      fi
  done
  if [[ "${NEED_SYSROOT_REFRESH}" == true ]]; then
      ./scripts/setup_arm_build_env.sh --skip-host-install --skip-arm-index-update
      source ".local-deps/arm-build-env.sh"
  fi

  if [[ "${CLEAN_BUILD}" == true ]]; then
      rm -rf "${PACKAGED_BUILD_DIR}"
  fi
  cmake -S . -B "${PACKAGED_BUILD_DIR}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_TESTING=OFF \
    -DUGRIPPER_ENABLE_MCAP_BUILDER=OFF \
    -DCMAKE_TOOLCHAIN_FILE="${TOOLCHAIN_FILE}" \
    "${UGRIPPER_ARM_CMAKE_ARGS[@]}"

  cmake --build "${PACKAGED_BUILD_DIR}" \
    --target CameraRecorder main_camera_xu_tool SensorRecorder zeroing GripperHmiTool UgripperRuntime fays_record_example \
    --parallel "${PARALLEL}"
fi

BASE_VERSION="${BASE_VERSION}" \
VERSION_SUFFIX="${VERSION_SUFFIX}" \
VERSION="${VERSION}" \
PACKAGED_BUILD_DIR="${PACKAGED_BUILD_DIR}" \
./build_deb.sh -q

for path in \
  "${PACKAGED_BUILD_DIR}" \
  "temp_build_deb" \
  "temp_build_deb_aux" \
  "${PACKAGE_DEB_NAME}"; do
  if [[ "${EUID}" -eq 0 && -e "$path" ]]; then
    chown -R "${HOST_UID}:${HOST_GID}" "$path"
  fi
done

echo "ARM package build finished:"
echo "  repo=${REPO_ROOT}"
echo "  build_dir=${REPO_ROOT}/${PACKAGED_BUILD_DIR}"
echo "  deb=${REPO_ROOT}/${PACKAGE_DEB_NAME}"
