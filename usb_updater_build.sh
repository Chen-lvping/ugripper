#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export OUTPUT_DIR="${OUTPUT_DIR:-${script_dir}/build/package/updater}"

build_dir="${BUILD_DIR:-${script_dir}/temp_build_usb_updater}"
case "$build_dir" in
    /*) ;;
    *) build_dir="$(pwd)/$build_dir" ;;
esac
export BUILD_DIR="$build_dir"

"${script_dir}/../DASUsbUpdater/usb_updater_build.sh" "$@"

# Keep the auto-release diff baseline aligned with the updater build that just
# succeeded. Without this, every later release analysis sees the same source
# delta and keeps recommending an updater rebuild.
MANIFEST_WRITER="${script_dir}/.codex/skills/auto-release-deb/scripts/write_source_manifest.sh"
if [ -x "$MANIFEST_WRITER" ]; then
    bash "$MANIFEST_WRITER" --target updater --output "${BUILD_DIR}/.auto_release_updater.manifest" || true
fi
