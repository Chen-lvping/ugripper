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

if ! git_root="$(git rev-parse --show-toplevel 2>/dev/null)"; then
  echo "ERROR: source manifest generation requires a Git worktree" >&2
  exit 1
fi
cd "$git_root"

list_ugripper_inputs() {
  local rel_path=""

  git ls-files --cached --others --exclude-standard -z -- . |
    while IFS= read -r -d '' rel_path; do
      case "$rel_path" in
        docs/*|.codex/*|graphify-out/*|ASR/*|.vscode/*|*/__pycache__/*|*.pyc|*.deb|*.md|*.txt|.DS_Store|*/.DS_Store)
          continue
          ;;
      esac
      [[ -f "$rel_path" ]] || continue
      printf '%s\0' "$rel_path"
    done | LC_ALL=C sort -zu
}

list_files_under() {
  local root="$1"

  [[ -d "$root" ]] || return 0
  find "$root" -type f \
    -not -path '*/.git/*' \
    -not -path '*/__pycache__/*' \
    -not -name '*.pyc' \
    -not -name '.DS_Store' \
    -print0
}

list_updater_inputs() {
  local das_updater_root="../DASUsbUpdater"

  {
    if [[ -f "usb_updater_build.sh" ]]; then
      printf '%s\0' "usb_updater_build.sh"
    fi

    if [[ -d "$das_updater_root" ]]; then
      if [[ -f "$das_updater_root/usb_updater_build.sh" ]]; then
        printf '%s\0' "$das_updater_root/usb_updater_build.sh"
      fi
      list_files_under "$das_updater_root/auto_update"
      list_files_under "$das_updater_root/product"
    elif [[ -d "auto_update" ]]; then
      list_files_under "auto_update"
    fi
  } | LC_ALL=C sort -zu
}

write_manifest() {
  local list_func="$1"
  local output_path="$2"
  local output_dir=""
  local paths_file=""
  local tmp_manifest=""
  local checksum_line=""
  local checksum=""
  local rel_path=""
  local file_count="0"
  local total_bytes="0"
  local start_seconds="$SECONDS"
  local duration_seconds="0"

  output_dir="$(dirname "$output_path")"
  mkdir -p "$output_dir"
  paths_file="$(mktemp)"
  tmp_manifest="$(mktemp "$output_dir/.source-manifest.XXXXXX")"

  cleanup_manifest_tmp() {
    [[ -z "$paths_file" ]] || rm -f -- "$paths_file"
    [[ -z "$tmp_manifest" ]] || rm -f -- "$tmp_manifest"
  }
  trap cleanup_manifest_tmp EXIT
  trap 'exit 129' HUP
  trap 'exit 130' INT
  trap 'exit 143' TERM

  if ! "$list_func" > "$paths_file"; then
    echo "ERROR: failed to enumerate source manifest inputs for target=$TARGET" >&2
    return 1
  fi

  file_count="$(tr -cd '\0' < "$paths_file" | wc -c | tr -d '[:space:]')"
  if ! total_bytes="$(xargs -0 -r stat -c '%s' -- < "$paths_file" | awk '{sum += $1} END {printf "%.0f", sum + 0}')"; then
    echo "ERROR: failed to inspect source manifest inputs for target=$TARGET" >&2
    return 1
  fi
  echo "--> Source manifest inputs: target=$TARGET files=$file_count bytes=$total_bytes" >&2

  if ! xargs -0 -r sha256sum -z -- < "$paths_file" |
    while IFS= read -r -d '' checksum_line; do
      if [[ ${#checksum_line} -lt 66 ]]; then
        echo "ERROR: invalid sha256sum output while writing target=$TARGET manifest" >&2
        exit 1
      fi
      checksum="${checksum_line:0:64}"
      rel_path="${checksum_line:66}"
      printf '%s\t%s\n' "$checksum" "$rel_path"
    done > "$tmp_manifest"; then
    echo "ERROR: failed to hash source manifest inputs for target=$TARGET" >&2
    return 1
  fi

  mv -f -- "$tmp_manifest" "$output_path"
  tmp_manifest=""
  rm -f -- "$paths_file"
  paths_file=""
  trap - EXIT HUP INT TERM

  duration_seconds=$((SECONDS - start_seconds))
  echo "--> Source manifest written: target=$TARGET files=$file_count bytes=$total_bytes elapsed=${duration_seconds}s output=$output_path" >&2
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
