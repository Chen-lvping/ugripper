#!/bin/bash
set -euo pipefail

usage() {
  cat <<'USAGE'
Usage:
  bash .codex/skills/auto-release-deb/scripts/write_source_manifest.sh --target <ugripper|updater> --output <manifest_path>

Options:
  --target <name>   Manifest target: ugripper or updater.
  --output <path>   Output manifest file path.
  -h, --help        Show this help message.
USAGE
}

TARGET=""
OUTPUT=""

while [[ $# -gt 0 ]]; do
  case "$1" in
    --target)
      if [[ $# -lt 2 ]]; then
        echo "Missing value for --target" >&2
        exit 1
      fi
      TARGET="$2"
      shift 2
      ;;
    --output)
      if [[ $# -lt 2 ]]; then
        echo "Missing value for --output" >&2
        exit 1
      fi
      OUTPUT="$2"
      shift 2
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

if [[ -z "$TARGET" || -z "$OUTPUT" ]]; then
  usage >&2
  exit 1
fi

if git_root="$(git rev-parse --show-toplevel 2>/dev/null)"; then
  cd "$git_root"
else
  script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
  cd "$(cd "$script_dir/../../../.." && pwd)"
fi

list_ugripper_inputs() {
  find . -type f \
    -not -path './.git/*' \
    -not -path './.venv/*' \
    -not -path './build/*' \
    -not -path './temp_build_deb/*' \
    -not -path './temp_build_usb_updater/*' \
    -not -path './docs/*' \
    -not -path './.codex/*' \
    -not -path './ASR/*' \
    -not -path './__pycache__/*' \
    -not -name '*.pyc' \
    -not -name '*.deb' \
    -not -name '*.md' \
    -not -name '*.txt' \
    -not -name '.DS_Store' \
    -printf '%P\n' \
    | sed '/^$/d' \
    | sort -u
}

list_updater_inputs() {
  {
    if [[ -f "usb_updater_build.sh" ]]; then
      echo "usb_updater_build.sh"
    fi
    if [[ -d "auto_update" ]]; then
      find auto_update -type f \
        -not -path '*/__pycache__/*' \
        -not -name '*.pyc' \
        -printf '%P\n' \
        | sed 's#^#auto_update/#'
    fi
  } | sed '/^$/d' | sort -u
}

write_manifest() {
  local list_func="$1"
  local output_path="$2"
  local tmp_file
  tmp_file="$(mktemp)"

  "$list_func" | while IFS= read -r rel_path; do
    [[ -n "$rel_path" ]] || continue
    [[ -f "$rel_path" ]] || continue
    printf '%s\t%s\n' "$(sha256sum -- "$rel_path" | awk '{print $1}')" "$rel_path"
  done > "$tmp_file"

  mkdir -p "$(dirname "$output_path")"
  mv "$tmp_file" "$output_path"
}

case "$TARGET" in
  ugripper)
    write_manifest list_ugripper_inputs "$OUTPUT"
    ;;
  updater)
    write_manifest list_updater_inputs "$OUTPUT"
    ;;
  *)
    echo "Unsupported target: $TARGET" >&2
    exit 1
    ;;
esac
