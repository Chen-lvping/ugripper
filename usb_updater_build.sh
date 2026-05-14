#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export OUTPUT_DIR="${OUTPUT_DIR:-${script_dir}/build/package/updater}"
exec "${script_dir}/../DASUsbUpdater/usb_updater_build.sh" "$@"
