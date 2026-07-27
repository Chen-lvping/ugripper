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
AUTO_RELEASE_STATE_DIR="${AUTO_RELEASE_STATE_DIR:-${script_dir}/temp_build_state}"
AUTO_RELEASE_MANIFEST_TIMEOUT_SECONDS="${AUTO_RELEASE_MANIFEST_TIMEOUT_SECONDS:-30}"
AUTO_RELEASE_UPDATER_MANIFEST="${AUTO_RELEASE_STATE_DIR}/.auto_release_updater.manifest"

"${script_dir}/../DASUsbUpdater/usb_updater_build.sh" "$@"

# Keep the auto-release diff baseline aligned with the updater build that just
# succeeded. Without this, every later release analysis sees the same source
# delta and keeps recommending an updater rebuild.
MANIFEST_WRITER="${script_dir}/.codex/skills/auto-release-deb/scripts/write_source_manifest.sh"
if [ -x "$MANIFEST_WRITER" ]; then
    echo "--> Refreshing auto-release updater manifest: ${AUTO_RELEASE_UPDATER_MANIFEST}"
    if timeout --kill-after=5s "${AUTO_RELEASE_MANIFEST_TIMEOUT_SECONDS}s" \
        bash "$MANIFEST_WRITER" --target updater --output "$AUTO_RELEASE_UPDATER_MANIFEST"; then
        echo "--> Auto-release updater manifest refreshed."
    else
        status=$?
        if [ "$status" -eq 124 ] || [ "$status" -eq 137 ]; then
            echo "WARNING: updater manifest refresh timed out after ${AUTO_RELEASE_MANIFEST_TIMEOUT_SECONDS}s; package build succeeded and the previous baseline was preserved." >&2
        else
            echo "WARNING: updater manifest refresh failed with status $status; package build succeeded and the previous baseline was preserved." >&2
        fi
    fi
else
    echo "WARNING: updater package build succeeded but manifest writer was not found: $MANIFEST_WRITER" >&2
fi
