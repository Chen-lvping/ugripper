#!/usr/bin/env bash
set -euo pipefail

SCRIPT_NAME="$(basename "$0")"
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEFAULT_EPISODE="tmp/mcap/episode_20260625_0140"
INPUT_PATH="${DEFAULT_EPISODE}"
OPEN_DOWNLOAD_PAGE=0
RUN_EXTRACT=0

usage() {
    cat <<EOF
Usage: ${SCRIPT_NAME} [options] [episode_dir_or_mcap]

Open a UGripper episode MCAP in Foxglove when a local Foxglove client is
installed. If Foxglove is not installed, print the shortest next steps.

Examples:
  ${SCRIPT_NAME}
  ${SCRIPT_NAME} tmp/mcap/episode_20260625_0140
  ${SCRIPT_NAME} tmp/mcap/episode_20260625_0140/episode.mcap
  ${SCRIPT_NAME} --extract tmp/mcap/episode_20260625_0140

Options:
  --download     Open the Foxglove download page when Foxglove is missing
  --extract      Also run scripts/extract_mcap_episode.py as a fallback
  -h, --help     Show this help
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --download)
            OPEN_DOWNLOAD_PAGE=1
            shift
            ;;
        --extract)
            RUN_EXTRACT=1
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        -* )
            echo "Unknown option: $1" >&2
            usage >&2
            exit 2
            ;;
        *)
            INPUT_PATH="$1"
            shift
            ;;
    esac
done

require_tool() {
    local tool_name="$1"
    if ! command -v "$tool_name" >/dev/null 2>&1; then
        echo "Required command not found: $tool_name" >&2
        exit 1
    fi
}

resolve_mcap_path() {
    local candidate="$1"
    if [[ -f "$candidate" ]]; then
        printf '%s\n' "$(realpath "$candidate")"
        return 0
    fi

    if [[ -d "$candidate" && -f "$candidate/episode.mcap" ]]; then
        printf '%s\n' "$(realpath "$candidate/episode.mcap")"
        return 0
    fi

    return 1
}

find_foxglove_cmd() {
    local candidate
    local candidates=(
        "foxglove-studio"
        "foxglove"
        "$HOME/bin/Foxglove"
        "$HOME/Applications/Foxglove.AppImage"
        "$HOME/Downloads/Foxglove.AppImage"
        "/opt/Foxglove/foxglove"
    )

    for candidate in "${candidates[@]}"; do
        if command -v "$candidate" >/dev/null 2>&1; then
            command -v "$candidate"
            return 0
        fi
        if [[ -x "$candidate" ]]; then
            printf '%s\n' "$candidate"
            return 0
        fi
    done

    return 1
}

print_topics() {
    cat <<'EOF'
Recommended image topics in Foxglove Image panels:
  /cam_left/image_compressed
  /cam_right/image_compressed
  /stereo_left/image_compressed
  /stereo_right/image_compressed
  /tcam_left_l/image_compressed
  /tcam_left_r/image_compressed
  /tcam_right_l/image_compressed
  /tcam_right_r/image_compressed

Recommended non-image topics:
  /fays_left/imu
  /fays_right/imu
  /encoder_left
  /encoder_right
EOF
}

require_tool realpath

if ! MCAP_PATH="$(resolve_mcap_path "$INPUT_PATH")"; then
    echo "Could not resolve episode.mcap from: $INPUT_PATH" >&2
    usage >&2
    exit 1
fi

EPISODE_DIR="$(dirname "$MCAP_PATH")"

echo "==> Repo root:  $REPO_ROOT"
echo "==> Episode dir: $EPISODE_DIR"
echo "==> MCAP file:   $MCAP_PATH"

if [[ "$RUN_EXTRACT" -eq 1 ]]; then
    echo "==> Extracting episode with scripts/extract_mcap_episode.py"
    /usr/bin/python3 "$REPO_ROOT/scripts/extract_mcap_episode.py" "$MCAP_PATH"
    echo "==> Extracted outputs under: $EPISODE_DIR/extracted_mcap or sibling extracted_mcap"
fi

if FOXGLOVE_CMD="$(find_foxglove_cmd)"; then
    echo "==> Found Foxglove: $FOXGLOVE_CMD"
    echo "==> Launching Foxglove with the MCAP path"
    nohup "$FOXGLOVE_CMD" "$MCAP_PATH" >/tmp/open_foxglove_episode.log 2>&1 &
    echo "==> If Foxglove opens without loading the file, use Open local file and select:"
    echo "    $MCAP_PATH"
    print_topics
    exit 0
fi

echo "Foxglove is not installed on this machine, so I cannot open the app directly yet."
echo
echo "Shortest path:"
echo "  1. Install Foxglove Desktop from https://foxglove.dev/download"
echo "  2. Re-run: scripts/open_foxglove_episode.sh $(printf '%q' "$MCAP_PATH")"
echo "  3. Or open Foxglove manually, then choose Open local file -> $MCAP_PATH"
echo
print_topics
echo
echo "Fallback without Foxglove:"
echo "  /usr/bin/python3 scripts/extract_mcap_episode.py $(printf '%q' "$MCAP_PATH")"

if [[ "$OPEN_DOWNLOAD_PAGE" -eq 1 ]]; then
    if command -v xdg-open >/dev/null 2>&1; then
        nohup xdg-open "https://foxglove.dev/download" >/tmp/open_foxglove_download.log 2>&1 &
        echo
        echo "Opened Foxglove download page in the default browser."
    else
        echo
        echo "xdg-open is unavailable; please open https://foxglove.dev/download manually."
    fi
fi
