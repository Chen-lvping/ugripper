#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

APP_NAME="ugripper"
ARCH="arm64"
BASE_VERSION="${BASE_VERSION:-1.2.8}"
VERSION_SUFFIX="${VERSION_SUFFIX:-}"
VERSION="${VERSION:-${BASE_VERSION}${VERSION_SUFFIX}}"
PACKAGE_DEB_NAME="${APP_NAME}_${VERSION}_${ARCH}.deb"

TOOLCHAIN_FILE="${TOOLCHAIN_FILE:-${REPO_ROOT}/cmake/arm-linux-toolchain.cmake}"
PACKAGED_BUILD_DIR="${PACKAGED_BUILD_DIR:-build/arm_container_release}"
PARALLEL="${PARALLEL:-8}"
HOST_UID="${HOST_UID:-$(stat -c %u "${REPO_ROOT}")}"
HOST_GID="${HOST_GID:-$(stat -c %g "${REPO_ROOT}")}"

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

if [[ ! -f ".local-deps/arm-build-env.sh" ]]; then
    ./scripts/setup_arm_build_env.sh
fi
source ".local-deps/arm-build-env.sh"

rm -rf "${PACKAGED_BUILD_DIR}"
cmake -S . -B "${PACKAGED_BUILD_DIR}" \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=OFF \
  -DUGRIPPER_ENABLE_MCAP_BUILDER=OFF \
  -DCMAKE_TOOLCHAIN_FILE="${TOOLCHAIN_FILE}" \
  "${UGRIPPER_ARM_CMAKE_ARGS[@]}"

cmake --build "${PACKAGED_BUILD_DIR}" \
  --target CameraRecorder SensorRecorder zeroing GripperHmiTool UgripperRuntime \
  --parallel "${PARALLEL}"

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
