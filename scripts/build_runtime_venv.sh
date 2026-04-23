#!/bin/bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "$script_dir/.." && pwd)"

TARGET_DIR="$repo_root/.venv"
PY_VER=""
VERSION_TAG="${VENV_VERSION_TAG:-}"
MANIFEST_NAME=".ugripper-venv-manifest.json"

usage() {
    cat <<'EOF'
Usage: scripts/build_runtime_venv.sh [options]

Build a relocatable uv-managed .venv for packaging and write a manifest with
version / architecture metadata.

Options:
  --target-dir <path>     Target .venv directory. Default: repo_root/.venv
  --python-version <ver>  Override Python version. Default: read from pyproject.toml
  --version-tag <tag>     Optional artifact version tag written into manifest
  -h, --help              Show this help
EOF
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --target-dir)
            TARGET_DIR="$2"
            shift 2
            ;;
        --python-version)
            PY_VER="$2"
            shift 2
            ;;
        --version-tag)
            VERSION_TAG="$2"
            shift 2
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "Unknown option: $1" >&2
            usage >&2
            exit 1
            ;;
    esac
done

require_cmd() {
    local cmd="$1"
    if ! command -v "$cmd" >/dev/null 2>&1; then
        echo "Required command not found: $cmd" >&2
        exit 1
    fi
}

extract_python_version() {
    sed -n 's/^requires-python = "==\([^"]*\)"$/\1/p' "$repo_root/pyproject.toml" | head -n 1
}

map_machine_to_package_arch() {
    case "$1" in
        x86_64|amd64)
            printf '%s\n' 'amd64'
            ;;
        aarch64|arm64)
            printf '%s\n' 'arm64'
            ;;
        *)
            printf '%s\n' 'unknown'
            ;;
    esac
}

file_sha256() {
    sha256sum "$1" | awk '{print $1}'
}

detect_fs_type() {
    local target_path="$1"
    local parent_dir

    parent_dir="$(dirname "$target_path")"
    mkdir -p "$parent_dir"

    if command -v findmnt >/dev/null 2>&1; then
        findmnt -T "$parent_dir" -no FSTYPE 2>/dev/null | head -n 1
        return 0
    fi

    stat -f -c %T "$parent_dir" 2>/dev/null || true
}

ensure_target_fs_supports_symlinks() {
    local target_path="$1"
    local fs_type="$2"

    case "$fs_type" in
        exfat|vfat|msdos|fat|fuseblk)
            cat >&2 <<EOF
Target directory is on filesystem '$fs_type', which does not support the symlinks required by uv-managed virtual environments.
Choose an ext4/xfs target for --target-dir, build the .venv there, then archive/copy the result elsewhere if needed.
Current target_dir: $target_path
EOF
            exit 1
            ;;
    esac
}

require_cmd uv
require_cmd sed
require_cmd find
require_cmd realpath
require_cmd sha256sum
require_cmd python3
require_cmd mkdir

if [ -z "$PY_VER" ]; then
    PY_VER="$(extract_python_version)"
fi

if [ -z "$PY_VER" ]; then
    echo "Failed to resolve Python version from pyproject.toml" >&2
    exit 1
fi

PY_MM="$(printf '%s' "$PY_VER" | cut -d. -f1,2)"
HOST_MACHINE="$(uname -m)"
PACKAGE_ARCH_HINT="$(map_machine_to_package_arch "$HOST_MACHINE")"
TMP_PY_DIR="$(mktemp -d)"
TARGET_DIR="$(realpath -m "$TARGET_DIR")"
MANIFEST_PATH="$TARGET_DIR/$MANIFEST_NAME"
TARGET_FS_TYPE="$(detect_fs_type "$TARGET_DIR")"

ensure_target_fs_supports_symlinks "$TARGET_DIR" "$TARGET_FS_TYPE"

cleanup() {
    rm -rf "$TMP_PY_DIR"
}
trap cleanup EXIT

echo "==> repo_root: $repo_root"
echo "==> target_dir: $TARGET_DIR"
echo "==> python_version: $PY_VER"
echo "==> host_machine: $HOST_MACHINE"
echo "==> package_arch_hint: $PACKAGE_ARCH_HINT"
if [ -n "$TARGET_FS_TYPE" ]; then
    echo "==> target_fs_type: $TARGET_FS_TYPE"
fi

rm -rf "$TARGET_DIR"
mkdir -p "$(dirname "$TARGET_DIR")"

uv python install --install-dir "$TMP_PY_DIR" "$PY_VER"

PY_BIN="$(find "$TMP_PY_DIR" -path "*/bin/python${PY_MM}" -type f | head -n 1)"
if [ -z "$PY_BIN" ]; then
    echo "Failed to locate python${PY_MM} inside uv-installed runtime" >&2
    exit 1
fi

UV_LINK_MODE=copy uv venv --relocatable --python "$PY_BIN" "$TARGET_DIR"
(
    cd "$repo_root"
    VIRTUAL_ENV="$TARGET_DIR" UV_LINK_MODE=copy uv sync \
        --active \
        --frozen \
        --no-editable \
        --no-install-project \
        --python "$PY_BIN"
)

mkdir -p "$TARGET_DIR/.python-runtime"
PY_HOME_DIR="$(dirname "$(dirname "$PY_BIN")")"
RUNTIME_BASENAME="$(basename "$PY_HOME_DIR")"
rm -rf "$TARGET_DIR/.python-runtime/$RUNTIME_BASENAME"
mv "$PY_HOME_DIR" "$TARGET_DIR/.python-runtime/"

REL_PY="$(realpath --relative-to="$TARGET_DIR/bin" "$TARGET_DIR/.python-runtime/$RUNTIME_BASENAME/bin/python${PY_MM}")"
ln -snf "$REL_PY" "$TARGET_DIR/bin/python"
ln -snf python "$TARGET_DIR/bin/python3"
ln -snf python "$TARGET_DIR/bin/python${PY_MM}"

PYTHON_EXEC="$TARGET_DIR/bin/python3"
if [ ! -x "$PYTHON_EXEC" ]; then
    echo "Packaged python entrypoint missing or not executable: $PYTHON_EXEC" >&2
    exit 1
fi

PYGAME_IMPORT_OUTPUT="$("$PYTHON_EXEC" -c 'import pygame, sys; print(sys.executable); print(sys.base_prefix)')"
echo "==> pygame import check passed"
printf '%s\n' "$PYGAME_IMPORT_OUTPUT"

PYTHON_FILE_DESC=""
if command -v file >/dev/null 2>&1; then
    PYTHON_FILE_DESC="$(file -L "$PYTHON_EXEC")"
    echo "==> python file: $PYTHON_FILE_DESC"
fi

GENERATED_AT_UTC="$(date -u +"%Y-%m-%dT%H:%M:%SZ")"
PYPROJECT_SHA256="$(file_sha256 "$repo_root/pyproject.toml")"
UV_LOCK_SHA256="$(file_sha256 "$repo_root/uv.lock")"
GIT_COMMIT="$(git -C "$repo_root" rev-parse HEAD 2>/dev/null || true)"

export BUILD_RUNTIME_VENV_VERSION_TAG="$VERSION_TAG"
export BUILD_RUNTIME_VENV_GENERATED_AT_UTC="$GENERATED_AT_UTC"
export BUILD_RUNTIME_VENV_REPO_ROOT="$repo_root"
export BUILD_RUNTIME_VENV_TARGET_DIR="$TARGET_DIR"
export BUILD_RUNTIME_VENV_PY_VER="$PY_VER"
export BUILD_RUNTIME_VENV_PY_MM="$PY_MM"
export BUILD_RUNTIME_VENV_HOST_MACHINE="$HOST_MACHINE"
export BUILD_RUNTIME_VENV_PACKAGE_ARCH_HINT="$PACKAGE_ARCH_HINT"
export BUILD_RUNTIME_VENV_PYTHON_FILE_DESC="$PYTHON_FILE_DESC"
export BUILD_RUNTIME_VENV_PYPROJECT_SHA256="$PYPROJECT_SHA256"
export BUILD_RUNTIME_VENV_UV_LOCK_SHA256="$UV_LOCK_SHA256"
export BUILD_RUNTIME_VENV_GIT_COMMIT="$GIT_COMMIT"

python3 - "$MANIFEST_PATH" <<'PY'
import json
import os
import sys

manifest_path = sys.argv[1]
payload = {
    "format_version": 1,
    "artifact_version_tag": os.environ.get("BUILD_RUNTIME_VENV_VERSION_TAG", ""),
    "generated_at_utc": os.environ["BUILD_RUNTIME_VENV_GENERATED_AT_UTC"],
    "repo_root": os.environ["BUILD_RUNTIME_VENV_REPO_ROOT"],
    "target_dir": os.environ["BUILD_RUNTIME_VENV_TARGET_DIR"],
    "python_version": os.environ["BUILD_RUNTIME_VENV_PY_VER"],
    "python_mm": os.environ["BUILD_RUNTIME_VENV_PY_MM"],
    "host_machine": os.environ["BUILD_RUNTIME_VENV_HOST_MACHINE"],
    "package_arch_hint": os.environ["BUILD_RUNTIME_VENV_PACKAGE_ARCH_HINT"],
    "python_file": os.environ.get("BUILD_RUNTIME_VENV_PYTHON_FILE_DESC", ""),
    "pyproject_sha256": os.environ["BUILD_RUNTIME_VENV_PYPROJECT_SHA256"],
    "uv_lock_sha256": os.environ["BUILD_RUNTIME_VENV_UV_LOCK_SHA256"],
    "git_commit": os.environ.get("BUILD_RUNTIME_VENV_GIT_COMMIT", ""),
}

with open(manifest_path, "w", encoding="utf-8") as fh:
    json.dump(payload, fh, ensure_ascii=True, indent=2, sort_keys=True)
    fh.write("\n")
PY

echo "==> manifest written: $MANIFEST_PATH"
cat "$MANIFEST_PATH"
