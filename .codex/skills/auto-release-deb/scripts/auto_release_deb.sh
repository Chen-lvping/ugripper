#!/bin/bash
set -euo pipefail

usage() {
  cat <<'USAGE'
Usage:
  bash .codex/skills/auto-release-deb/scripts/auto_release_deb.sh [--bump .z|.y|.x] [--note "text"] [--no-build]

Options:
  --bump      Version bump level. Default is .z (patch).
              Accepted values: .z/.y/.x, z/y/x, patch/minor/major.
  --note      Deprecated. Kept for compatibility, ignored by this script.
  --no-build  Skip running build_deb.sh (only update VERSION in build_deb.sh).
  -h, --help  Show this help message.
USAGE
}

normalize_bump() {
  case "$1" in
    .z|z|patch) echo "z" ;;
    .y|y|minor) echo "y" ;;
    .x|x|major) echo "x" ;;
    *)
      echo "Invalid --bump value: $1" >&2
      exit 1
      ;;
  esac
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

BUMP="z"
NOTE=""
NO_BUILD=false

while [[ $# -gt 0 ]]; do
  case "$1" in
    --bump)
      if [[ $# -lt 2 ]]; then
        echo "--bump requires a value" >&2
        exit 1
      fi
      BUMP="$(normalize_bump "$2")"
      shift 2
      ;;
    --note)
      if [[ $# -lt 2 ]]; then
        echo "--note requires a value" >&2
        exit 1
      fi
      NOTE="$2"
      shift 2
      ;;
    --no-build)
      NO_BUILD=true
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

if [[ ! -f build_deb.sh ]]; then
  echo "build_deb.sh not found in repository root" >&2
  exit 1
fi

mapfile -t changed_files < <(collect_changed_files)

version_line="$(grep -E '^VERSION="[0-9]+\.[0-9]+\.[0-9]+"' build_deb.sh | head -n 1 || true)"
if [[ -z "$version_line" ]]; then
  echo "Failed to parse VERSION from build_deb.sh" >&2
  exit 1
fi

current_version="$(echo "$version_line" | sed -E 's/^VERSION="([0-9]+\.[0-9]+\.[0-9]+)".*/\1/')"
IFS='.' read -r major minor patch <<< "$current_version"

case "$BUMP" in
  z)
    patch=$((patch + 1))
    bump_label=".z"
    ;;
  y)
    minor=$((minor + 1))
    patch=0
    bump_label=".y"
    ;;
  x)
    major=$((major + 1))
    minor=0
    patch=0
    bump_label=".x"
    ;;
esac

new_version="${major}.${minor}.${patch}"
sed -i -E "s/^VERSION=\"[0-9]+\.[0-9]+\.[0-9]+\"/VERSION=\"${new_version}\"/" build_deb.sh

needs_full_build=false
full_build_trigger=""
for file in "${changed_files[@]}"; do
  case "$file" in
    *.c|*.cc|*.cpp|*.cxx|*.h|*.hh|*.hpp|*.cmake|CMakeLists.txt|*/CMakeLists.txt)
      needs_full_build=true
      full_build_trigger="$file"
      break
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

use_quick=false
build_reason=""
if [[ "$needs_full_build" == true ]]; then
  use_quick=false
  build_reason="detected C/C++/CMake change at ${full_build_trigger}"
elif [[ ${#missing_binaries[@]} -gt 0 ]]; then
  use_quick=false
  build_reason="quick-mode binaries missing: ${missing_binaries[*]}"
else
  use_quick=true
  build_reason="no C/C++/CMake changes and quick-mode binaries are ready"
fi

build_mode="skipped"
if [[ "$NO_BUILD" == false ]]; then
  if [[ "$use_quick" == true ]]; then
    build_mode="quick"
    ./build_deb.sh -q
  else
    build_mode="standard"
    ./build_deb.sh
  fi
fi

echo "Release automation completed"
echo "Version: ${current_version} -> ${new_version} (requested bump: ${bump_label})"
echo "Build mode: ${build_mode}"
echo "Reason: ${build_reason}"
if [[ -n "$NOTE" ]]; then
  echo "Note: --note is ignored by auto-release-deb; maintain feature changes in docs/CHANGELOG.md via add-feature workflow"
else
  echo "Note: feature changelog is maintained by add-feature workflow in docs/CHANGELOG.md"
fi
