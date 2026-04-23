#!/usr/bin/env bash
set -euo pipefail

usage() {
    cat <<'EOF'
Usage: setup_arm_build_env.sh [options]

Prepare a Debian/Ubuntu environment for ugripper ARM package builds.
This script is intended to run inside the build container or on an ARM-capable
Debian/Ubuntu host used for packaging.

Options:
  --skip-update   Skip apt-get update
  --help          Show this help
EOF
}

SKIP_UPDATE=0

while (($# > 0)); do
    case "$1" in
        --skip-update)
            SKIP_UPDATE=1
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

if ! command -v apt-get >/dev/null 2>&1; then
    echo "apt-get is required" >&2
    exit 1
fi

if ! command -v dpkg >/dev/null 2>&1; then
    echo "dpkg is required" >&2
    exit 1
fi

SUDO=""
if [[ "${EUID}" -ne 0 ]]; then
    if command -v sudo >/dev/null 2>&1; then
        SUDO="sudo"
    else
        echo "Run this script as root or install sudo first." >&2
        exit 1
    fi
fi

ensure_foreign_architecture() {
    local arch="$1"
    if ! dpkg --print-foreign-architectures | grep -Fxq "$arch"; then
        echo "==> Adding foreign architecture: $arch"
        ${SUDO} dpkg --add-architecture "$arch"
        return 0
    fi
    return 1
}

NEED_UPDATE=0
if ensure_foreign_architecture arm64; then
    NEED_UPDATE=1
fi

if [[ "$SKIP_UPDATE" -eq 0 || "$NEED_UPDATE" -eq 1 ]]; then
    echo "==> apt-get update"
    ${SUDO} apt-get update
fi

PACKAGES=(
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
    pkg-config
    rsync
    sed
    tar
    binutils-aarch64-linux-gnu
    gcc-aarch64-linux-gnu
    g++-aarch64-linux-gnu
    libyaml-cpp-dev:arm64
    nlohmann-json3-dev
    liblz4-dev:arm64
    libzstd-dev:arm64
    libserialport-dev:arm64
    libusb-1.0-0-dev:arm64
    libgstreamer1.0-dev:arm64
    libgstreamer-plugins-base1.0-dev:arm64
)

echo "==> Installing ARM build prerequisites"
${SUDO} apt-get install -y "${PACKAGES[@]}"

cat <<'EOF'
==> ARM build environment is ready.
Recommended follow-up:
  export PKG_CONFIG_PATH=/usr/lib/aarch64-linux-gnu/pkgconfig:${PKG_CONFIG_PATH:-}

Toolchain expected by cmake/arm-linux-toolchain.cmake:
  /usr/bin/aarch64-linux-gnu-gcc
  /usr/bin/aarch64-linux-gnu-g++
  /usr/bin/aarch64-linux-gnu-as
  /usr/bin/aarch64-linux-gnu-ld
  /usr/bin/aarch64-linux-gnu-ar
EOF
