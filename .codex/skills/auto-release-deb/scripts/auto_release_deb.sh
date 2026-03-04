#!/bin/bash
set -euo pipefail

usage() {
  cat <<'USAGE'
Usage:
  bash .codex/skills/auto-release-deb/scripts/auto_release_deb.sh [--no-files]

Options:
  --no-files  Do not print the changed file list.
  -h, --help  Show this help message.
USAGE
}

collect_changed_files() {
  if ! git rev-parse --is-inside-work-tree >/dev/null 2>&1; then
    return 0
  fi

  {
    git diff --name-only --relative HEAD 2>/dev/null || true
    git ls-files --others --exclude-standard 2>/dev/null || true
  } | sed '/^$/d' | sort -u
}

is_meta_only_file() {
  local file="$1"
  case "$file" in
    docs/*|.codex/*|*.md|*.txt|LICENSE|.gitignore)
      return 0
      ;;
    *)
      return 1
      ;;
  esac
}

PRINT_FILES=true

while [[ $# -gt 0 ]]; do
  case "$1" in
    --no-files)
      PRINT_FILES=false
      shift
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

if git_root="$(git rev-parse --show-toplevel 2>/dev/null)"; then
  cd "$git_root"
else
  script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
  cd "$(cd "$script_dir/../../../.." && pwd)"
fi

mapfile -t changed_files < <(collect_changed_files)

affects_ugripper=false
affects_updater=false

needs_full_ugripper=false
ugripper_reason=""
full_build_trigger=""

for file in "${changed_files[@]}"; do
  if [[ "$file" == auto_update/* || "$file" == "usb_updater_build.sh" ]]; then
    affects_updater=true
  fi

  if ! is_meta_only_file "$file"; then
    affects_ugripper=true
  fi

  case "$file" in
    *.c|*.cc|*.cpp|*.cxx|*.h|*.hh|*.hpp|*.cmake|CMakeLists.txt|*/CMakeLists.txt)
      needs_full_ugripper=true
      full_build_trigger="$file"
      ;;
  esac
done

missing_binaries=()
for bin_path in \
  "build/src/sensor_recorder/sensor_recorder" \
  "build/src/sensor_recorder/zeroing" \
  "build/faysSense_vi_kit/fays_record_example"; do
  if [[ ! -f "$bin_path" ]]; then
    missing_binaries+=("$bin_path")
  fi
done

ugripper_mode="skip"
if [[ "$affects_ugripper" == true ]]; then
  if [[ "$needs_full_ugripper" == true ]]; then
    ugripper_mode="full"
    ugripper_reason="detected C/C++/CMake change at ${full_build_trigger}"
  elif [[ ${#missing_binaries[@]} -gt 0 ]]; then
    ugripper_mode="full"
    ugripper_reason="quick-mode binaries missing: ${missing_binaries[*]}"
  else
    ugripper_mode="quick"
    ugripper_reason="no C/C++/CMake changes and quick-mode binaries are ready"
  fi
else
  ugripper_reason="no runtime/package-impacting changes for ugripper"
fi

updater_mode="skip"
if [[ "$affects_updater" == true ]]; then
  updater_mode="full"
  updater_reason="detected updater-related changes (auto_update/* or usb_updater_build.sh)"
else
  updater_reason="no updater-related changes"
fi

if [[ "$affects_ugripper" == true && "$affects_updater" == true ]]; then
  scope="both"
elif [[ "$affects_ugripper" == true ]]; then
  scope="ugripper"
elif [[ "$affects_updater" == true ]]; then
  scope="updater"
else
  scope="none"
fi

echo "Release scope analysis completed"
echo "Version bump: not performed by auto-release-deb"
echo "Scope: ${scope}"

echo "Ugripper build mode: ${ugripper_mode}"
echo "Ugripper reason: ${ugripper_reason}"

echo "Updater build mode: ${updater_mode}"
echo "Updater reason: ${updater_reason}"

if [[ "$PRINT_FILES" == true ]]; then
  echo "Changed files (${#changed_files[@]}):"
  for file in "${changed_files[@]}"; do
    echo "- ${file}"
  done
fi

echo "Recommended commands:"
if [[ "$ugripper_mode" == "full" ]]; then
  echo "- ./build_deb.sh"
elif [[ "$ugripper_mode" == "quick" ]]; then
  echo "- ./build_deb.sh -q"
else
  echo "- (skip ugripper build)"
fi

if [[ "$updater_mode" == "full" ]]; then
  echo "- ./usb_updater_build.sh"
else
  echo "- (skip updater build)"
fi
