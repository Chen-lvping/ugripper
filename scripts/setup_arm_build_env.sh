#!/usr/bin/env bash
# UGripper ARM package build environment.
#
# Host tools installed by this script are intentionally unpinned because they
# run on the build host only: bash, build-essential, ca-certificates, cmake,
# curl, dpkg-dev, file, findutils, git, grep, make, ninja-build, pkg-config,
# rsync, sed, tar, binutils-aarch64-linux-gnu, gcc-aarch64-linux-gnu, and
# g++-aarch64-linux-gnu.
#
# ARM64 sysroot direct dependencies are pinned to Ubuntu Jammy package versions:
#   libyaml-cpp-dev:arm64=0.7.0+dfsg-8build1
#   nlohmann-json3-dev=3.10.5-2
#   liblz4-dev:arm64=1.9.3-2build2
#   libserialport-dev:arm64=0.1.1-4
#   libusb-1.0-0-dev:arm64=2:1.0.25-1ubuntu2
#   libfmt-dev:arm64=8.1.1+ds1-2
#   libspdlog-dev:arm64=1:1.9.2+ds-0.2
#   libgstreamer1.0-dev:arm64=1.20.3-0ubuntu1.1
#   libgstreamer-plugins-base1.0-dev:arm64=1.20.1-1ubuntu0.6
#
# The private apt root resolves transitive ARM64 dependencies from the same
# Jammy ports repositories and extracts them into .local-deps/sysroot-arm64.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
LOCAL_DEPS_DIR="${LOCAL_DEPS_DIR:-${REPO_ROOT}/.local-deps}"
APT_ROOT="${APT_ROOT:-${LOCAL_DEPS_DIR}/apt-arm64}"
APT_ARCHIVES="${APT_ARCHIVES:-${LOCAL_DEPS_DIR}/downloads/apt}"
SYSROOT="${SYSROOT:-${LOCAL_DEPS_DIR}/sysroot-arm64}"
ENV_FILE="${ENV_FILE:-${LOCAL_DEPS_DIR}/arm-build-env.sh}"

SKIP_HOST_UPDATE=0
SKIP_HOST_INSTALL=0
SKIP_ARM_INDEX_UPDATE=0

usage() {
    cat <<'EOF'
Usage: setup_arm_build_env.sh [options]

Prepare an isolated ARM64 build sysroot for UGripper packaging.

This script intentionally does not add foreign dpkg architectures and does not
modify system apt sources. Host build tools are installed with the host apt
configuration; ARM64 packages are only downloaded through a private apt root and
extracted into .local-deps/sysroot-arm64.

Options:
  --skip-host-update       Skip host apt-get update before host tool install.
  --skip-host-install      Do not install host tools such as rsync/toolchain.
  --skip-arm-index-update  Reuse the private ARM apt index if already present.
  --help                   Show this help.

Outputs:
  .local-deps/sysroot-arm64      ARM64 headers/libs/pkg-config files.
  .local-deps/downloads/apt      Downloaded ARM64 deb cache.
  .local-deps/arm-build-env.sh   Sourceable build environment.
EOF
}

while (($# > 0)); do
    case "$1" in
        --skip-host-update|--skip-update)
            SKIP_HOST_UPDATE=1
            shift
            ;;
        --skip-host-install)
            SKIP_HOST_INSTALL=1
            shift
            ;;
        --skip-arm-index-update)
            SKIP_ARM_INDEX_UPDATE=1
            shift
            ;;
        --help|-h)
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

require_tool() {
    local tool="$1"
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "Required command not found: $tool" >&2
        exit 1
    fi
}

sudo_cmd=()
if [[ "${EUID}" -ne 0 ]]; then
    require_tool sudo
    sudo_cmd=(sudo)
fi

HOST_PACKAGES=(
    bash
    build-essential
    ca-certificates
    cmake
    curl
    dpkg-dev
    file
    findutils
    git
    grep
    make
    ninja-build
    pkg-config
    rsync
    sed
    tar
    binutils-aarch64-linux-gnu
    gcc-aarch64-linux-gnu
    g++-aarch64-linux-gnu
)

ARM_PACKAGES=(
    libyaml-cpp-dev:arm64=0.7.0+dfsg-8build1
    nlohmann-json3-dev=3.10.5-2
    liblz4-dev:arm64=1.9.3-2build2
    libserialport-dev:arm64=0.1.1-4
    libusb-1.0-0-dev:arm64=2:1.0.25-1ubuntu2
    libfmt-dev:arm64=8.1.1+ds1-2
    libspdlog-dev:arm64=1:1.9.2+ds-0.2
    libgstreamer1.0-dev:arm64=1.20.3-0ubuntu1.1
    libgstreamer-plugins-base1.0-dev:arm64=1.20.1-1ubuntu0.6
)

install_host_tools() {
    if [[ "$SKIP_HOST_INSTALL" -eq 1 ]]; then
        echo "==> Skipping host tool install"
        return
    fi

    require_tool apt-get
    if [[ "$SKIP_HOST_UPDATE" -eq 0 ]]; then
        echo "==> Updating host apt index"
        "${sudo_cmd[@]}" apt-get update
    fi

    echo "==> Installing host build tools"
    "${sudo_cmd[@]}" apt-get install -y "${HOST_PACKAGES[@]}"
}

prepare_private_apt_root() {
    mkdir -p \
        "${APT_ROOT}/etc/apt/sources.list.d" \
        "${APT_ROOT}/var/lib/apt/lists/partial" \
        "${APT_ROOT}/var/lib/dpkg" \
        "${APT_ROOT}/var/cache/apt/archives/partial" \
        "${APT_ARCHIVES}/partial"
    : > "${APT_ROOT}/var/lib/dpkg/status"

    cat > "${APT_ROOT}/etc/apt/sources.list" <<'EOF'
deb [arch=arm64] http://ports.ubuntu.com/ubuntu-ports jammy main universe restricted multiverse
deb [arch=arm64] http://ports.ubuntu.com/ubuntu-ports jammy-updates main universe restricted multiverse
deb [arch=arm64] http://ports.ubuntu.com/ubuntu-ports jammy-security main universe restricted multiverse
EOF
}

apt_arm() {
    apt-get \
        -o Dir::Etc::sourcelist="${APT_ROOT}/etc/apt/sources.list" \
        -o Dir::Etc::sourceparts="${APT_ROOT}/etc/apt/sources.list.d" \
        -o Dir::State::lists="${APT_ROOT}/var/lib/apt/lists" \
        -o Dir::State::status="${APT_ROOT}/var/lib/dpkg/status" \
        -o Dir::Cache="${APT_ROOT}/var/cache/apt" \
        -o Dir::Cache::archives="${APT_ARCHIVES}" \
        -o APT::Architecture=arm64 \
        -o APT::Architectures=arm64 \
        "$@"
}

download_arm_packages() {
    require_tool apt-get
    prepare_private_apt_root

    if [[ "$SKIP_ARM_INDEX_UPDATE" -eq 0 ]]; then
        echo "==> Updating private ARM64 apt index"
        apt_arm update
    fi

    echo "==> Downloading ARM64 build/runtime packages into ${APT_ARCHIVES}"
    apt_arm -y --download-only --no-install-recommends install "${ARM_PACKAGES[@]}"
}

extract_sysroot() {
    require_tool dpkg-deb

    echo "==> Extracting ARM64 packages into ${SYSROOT}"
    rm -rf "${SYSROOT}"
    mkdir -p "${SYSROOT}"

    shopt -s nullglob
    local deb
    local count=0
    for deb in "${APT_ARCHIVES}"/*.deb; do
        dpkg-deb -x "$deb" "${SYSROOT}"
        count=$((count + 1))
    done
    shopt -u nullglob

    if [[ "$count" -eq 0 ]]; then
        echo "No ARM64 debs found under ${APT_ARCHIVES}" >&2
        exit 1
    fi
    echo "==> Extracted ${count} ARM64 packages"
}

write_env_file() {
    mkdir -p "$(dirname "${ENV_FILE}")"
    cat > "${ENV_FILE}" <<EOF
# Generated by scripts/setup_arm_build_env.sh. Source this before manual ARM builds.
export UGRIPPER_ARM_SYSROOT='${SYSROOT}'
export PKG_CONFIG_SYSROOT_DIR="\${UGRIPPER_ARM_SYSROOT}"
export PKG_CONFIG_LIBDIR="\${UGRIPPER_ARM_SYSROOT}/usr/lib/aarch64-linux-gnu/pkgconfig:\${UGRIPPER_ARM_SYSROOT}/usr/lib/pkgconfig:\${UGRIPPER_ARM_SYSROOT}/usr/share/pkgconfig"
export PKG_CONFIG_PATH=
export CMAKE_PREFIX_PATH=
export LD_LIBRARY_PATH=

UGRIPPER_ARM_CMAKE_ARGS=(
  -DCMAKE_FIND_ROOT_PATH="\${UGRIPPER_ARM_SYSROOT}"
  -DCMAKE_PREFIX_PATH="\${UGRIPPER_ARM_SYSROOT}/usr"
  -DPKG_CONFIG_EXECUTABLE=/usr/bin/pkg-config
  -DPKG_CONFIG_USE_CMAKE_PREFIX_PATH=FALSE
  -DCMAKE_CXX_FLAGS="-isystem \${UGRIPPER_ARM_SYSROOT}/usr/include"
  -DCMAKE_EXE_LINKER_FLAGS="-Wl,-rpath-link,\${UGRIPPER_ARM_SYSROOT}/usr/lib/aarch64-linux-gnu -Wl,-rpath-link,\${UGRIPPER_ARM_SYSROOT}/lib/aarch64-linux-gnu"
)
EOF
    echo "==> Wrote ${ENV_FILE}"
}

validate_sysroot() {
    require_tool pkg-config
    require_tool aarch64-linux-gnu-g++
    require_tool aarch64-linux-gnu-readelf
    require_tool rsync

    # shellcheck disable=SC1090
    source "${ENV_FILE}"

    echo "==> Validating ARM64 sysroot pkg-config modules"
    pkg-config --exists yaml-cpp
    pkg-config --exists libserialport
    pkg-config --exists liblz4
    pkg-config --exists libusb-1.0
    pkg-config --exists fmt
    pkg-config --exists spdlog
    pkg-config --exists gstreamer-1.0
    pkg-config --exists gstreamer-app-1.0
    pkg-config --exists gstreamer-base-1.0

    test -f "${SYSROOT}/usr/lib/cmake/nlohmann_json/nlohmann_jsonConfig.cmake" || {
        echo "Missing nlohmann_json CMake config in ${SYSROOT}" >&2
        exit 1
    }
    test -f "${SYSROOT}/usr/include/gstreamer-1.0/gst/gst.h" || {
        echo "Missing GStreamer headers in ${SYSROOT}" >&2
        exit 1
    }
    test -f "${SYSROOT}/usr/lib/aarch64-linux-gnu/libgstreamer-1.0.so" || {
        echo "Missing GStreamer library in ${SYSROOT}" >&2
        exit 1
    }
}

cat_summary() {
    cat <<EOF
==> ARM build environment is ready.

Sysroot:
  ${SYSROOT}

To build manually:
  cd ${REPO_ROOT}
  source ${ENV_FILE}
  cmake -S . -B build/arm_container_release \\
    -DCMAKE_BUILD_TYPE=Release \\
    -DBUILD_TESTING=OFF \\
    -DUGRIPPER_ENABLE_MCAP_BUILDER=OFF \\
    -DCMAKE_TOOLCHAIN_FILE="\${PWD}/cmake/arm-linux-toolchain.cmake" \\
    "\${UGRIPPER_ARM_CMAKE_ARGS[@]}"
  cmake --build build/arm_container_release \\
    --target CameraRecorder main_camera_xu_tool SensorRecorder zeroing GripperHmiTool UgripperRuntime \\
    --parallel 8
  ./scripts/build_arm_deb_in_pp_arm_dev.sh
EOF
}

install_host_tools
download_arm_packages
extract_sysroot
write_env_file
validate_sysroot
cat_summary
