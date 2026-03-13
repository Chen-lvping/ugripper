#!/bin/bash
set -euo pipefail

usage() {
  cat <<'USAGE'
Usage:
  bash .codex/skills/auto-release-deb/scripts/auto_release_deb.sh [--no-files] [--ugripper-manifest PATH] [--updater-manifest PATH]

Options:
  --no-files                 Do not print the changed file list.
  --ugripper-manifest PATH   Baseline manifest path for ugripper (default: temp_build_deb/.auto_release_ugripper.manifest).
  --updater-manifest PATH    Baseline manifest path for updater (default: temp_build_usb_updater/.auto_release_updater.manifest).
  -h, --help                 Show this help message.
USAGE
}

manifest_changed_paths() {
  local old_manifest="$1"
  local new_manifest="$2"

  awk -F '\t' '
    NR==FNR {
      old[$2]=$1
      next
    }
    {
      new[$2]=$1
    }
    END {
      for (p in old) {
        if (!(p in new) || old[p] != new[p]) {
          print p
        }
      }
      for (p in new) {
        if (!(p in old)) {
          print p
        }
      }
    }
  ' "$old_manifest" "$new_manifest" | sed '/^$/d' | sort -u
}

is_full_build_trigger_file() {
  local file="$1"
  case "$file" in
    *.c|*.cc|*.cpp|*.cxx|*.h|*.hh|*.hpp|*.cmake|CMakeLists.txt|*/CMakeLists.txt)
      return 0
      ;;
    *)
      return 1
      ;;
  esac
}

PRINT_FILES=true
UGRIPPER_BASELINE_MANIFEST="temp_build_deb/.auto_release_ugripper.manifest"
UPDATER_BASELINE_MANIFEST="temp_build_usb_updater/.auto_release_updater.manifest"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --no-files)
      PRINT_FILES=false
      shift
      ;;
    --ugripper-manifest)
      if [[ $# -lt 2 ]]; then
        echo "Missing value for --ugripper-manifest" >&2
        exit 1
      fi
      UGRIPPER_BASELINE_MANIFEST="$2"
      shift 2
      ;;
    --updater-manifest)
      if [[ $# -lt 2 ]]; then
        echo "Missing value for --updater-manifest" >&2
        exit 1
      fi
      UPDATER_BASELINE_MANIFEST="$2"
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

if git_root="$(git rev-parse --show-toplevel 2>/dev/null)"; then
  cd "$git_root"
else
  script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
  cd "$(cd "$script_dir/../../../.." && pwd)"
fi

manifest_writer=".codex/skills/auto-release-deb/scripts/write_source_manifest.sh"
if [[ ! -x "$manifest_writer" ]]; then
  echo "ERROR: manifest writer not found or not executable: $manifest_writer" >&2
  exit 1
fi

current_ugripper_manifest="$(mktemp)"
current_updater_manifest="$(mktemp)"
trap 'rm -f "$current_ugripper_manifest" "$current_updater_manifest"' EXIT

bash "$manifest_writer" --target ugripper --output "$current_ugripper_manifest"
bash "$manifest_writer" --target updater --output "$current_updater_manifest"

affects_ugripper=false
affects_updater=false
needs_full_ugripper=false
ugripper_reason=""
updater_reason=""
full_build_trigger=""

ugripper_baseline_status="ready"
updater_baseline_status="ready"

ugripper_changed_files=()
updater_changed_files=()
changed_files=()

if [[ ! -f "$UGRIPPER_BASELINE_MANIFEST" ]]; then
  ugripper_baseline_status="missing"
  affects_ugripper=true
  needs_full_ugripper=true
  full_build_trigger="manifest-missing"
else
  if ! cmp -s "$UGRIPPER_BASELINE_MANIFEST" "$current_ugripper_manifest"; then
    affects_ugripper=true
    mapfile -t ugripper_changed_files < <(manifest_changed_paths "$UGRIPPER_BASELINE_MANIFEST" "$current_ugripper_manifest")
    for file in "${ugripper_changed_files[@]}"; do
      if is_full_build_trigger_file "$file"; then
        needs_full_ugripper=true
        full_build_trigger="$file"
        break
      fi
    done
  fi
fi

if [[ ! -f "$UPDATER_BASELINE_MANIFEST" ]]; then
  updater_baseline_status="missing"
  affects_updater=true
else
  if ! cmp -s "$UPDATER_BASELINE_MANIFEST" "$current_updater_manifest"; then
    affects_updater=true
    mapfile -t updater_changed_files < <(manifest_changed_paths "$UPDATER_BASELINE_MANIFEST" "$current_updater_manifest")
  fi
fi

all_changed_files=()
all_changed_files+=("${ugripper_changed_files[@]}")
all_changed_files+=("${updater_changed_files[@]}")
if [[ ${#all_changed_files[@]} -gt 0 ]]; then
  mapfile -t changed_files < <(printf '%s\n' "${all_changed_files[@]}" | sed '/^$/d' | sort -u)
fi

missing_binaries=()
for bin_path in \
  "build/src/sensor_recorder/sensor_recorder" \
  "build/src/sensor_recorder/zeroing" \
  "build/src/camera_recorder/camera_recorder"; do
  if [[ ! -f "$bin_path" ]]; then
    missing_binaries+=("$bin_path")
  fi
done

ugripper_mode="skip"
if [[ "$affects_ugripper" == true ]]; then
  if [[ "$ugripper_baseline_status" == "missing" ]]; then
    ugripper_mode="full"
    ugripper_reason="baseline manifest missing: ${UGRIPPER_BASELINE_MANIFEST}; force full build"
  elif [[ "$needs_full_ugripper" == true ]]; then
    ugripper_mode="full"
    ugripper_reason="detected C/C++/CMake change at ${full_build_trigger}"
  elif [[ ${#missing_binaries[@]} -gt 0 ]]; then
    ugripper_mode="full"
    ugripper_reason="quick-mode binaries missing: ${missing_binaries[*]}"
  else
    ugripper_mode="quick"
    ugripper_reason="source delta detected against temp_build_deb manifest; no C/C++/CMake changes"
  fi
else
  ugripper_reason="no source delta against temp_build_deb manifest"
fi

updater_mode="skip"
if [[ "$affects_updater" == true ]]; then
  updater_mode="full"
  if [[ "$updater_baseline_status" == "missing" ]]; then
    updater_reason="baseline manifest missing: ${UPDATER_BASELINE_MANIFEST}; force updater build"
  else
    updater_reason="source delta detected against temp_build_usb_updater manifest"
  fi
else
  updater_reason="no source delta against temp_build_usb_updater manifest"
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
echo "Diff base: temp_build manifests"
echo "Ugripper baseline manifest: ${UGRIPPER_BASELINE_MANIFEST} (${ugripper_baseline_status})"
echo "Updater baseline manifest: ${UPDATER_BASELINE_MANIFEST} (${updater_baseline_status})"
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
