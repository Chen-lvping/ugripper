#!/bin/bash
set -euo pipefail

usage() {
  cat <<'USAGE'
Usage:
  bash .codex/skills/add-feature/scripts/bump_release_versions.sh \
    [--target ugripper|updater|both] [--bump .z|.y|.x] [--reason "text"] [--no-bump]

Options:
  --target   Which package version(s) to update. Default: both.
             ugripper -> build_deb.sh BASE_VERSION and scripts/build_arm_deb_in_pp_arm_dev.sh BASE_VERSION
             updater  -> ../DASUsbUpdater/usb_updater_build.sh PKG_VERSION_BASE
             both     -> update both files
  --bump     Version bump level. Default: .z
             Accepted values: .z/.y/.x, z/y/x, patch/minor/major.
  --reason   Optional decision note for reporting.
  --no-bump  Do not change version numbers; only print a skip summary.
  -h, --help Show this help message.
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

bump_semver() {
  local version="$1"
  local bump="$2"
  local major minor patch

  IFS='.' read -r major minor patch <<< "$version"

  case "$bump" in
    z)
      patch=$((patch + 1))
      ;;
    y)
      minor=$((minor + 1))
      patch=0
      ;;
    x)
      major=$((major + 1))
      minor=0
      patch=0
      ;;
  esac

  echo "${major}.${minor}.${patch}"
}

parse_version_file() {
  local file="$1"
  local key="$2"

  if [[ ! -f "$file" ]]; then
    echo "Required file not found: $file" >&2
    exit 1
  fi

  local line
  line="$(grep -E "^${key}=\"\\$\\{${key}:-[0-9]+\\.[0-9]+\\.[0-9]+\\}\"" "$file" | head -n 1 || true)"
  if [[ -z "$line" ]]; then
    line="$(grep -E "^${key}=\"[0-9]+\\.[0-9]+\\.[0-9]+\"" "$file" | head -n 1 || true)"
  fi
  if [[ -z "$line" ]]; then
    echo "Failed to parse ${key} from ${file}" >&2
    exit 1
  fi

  sed -n -E "s/^${key}=\"\\$\\{${key}:-([0-9]+\\.[0-9]+\\.[0-9]+)\\}\".*/\\1/p; s/^${key}=\"([0-9]+\\.[0-9]+\\.[0-9]+)\".*/\\1/p" "$file" | head -n 1
}

update_version_file_to() {
  local file="$1"
  local key="$2"
  local new_version="$3"

  if [[ ! -f "$file" ]]; then
    echo "Required file not found: $file" >&2
    exit 1
  fi

  local current_version
  current_version="$(parse_version_file "$file" "$key")"

  local tmp
  tmp="$(mktemp)"
  awk -v key="$key" -v new_version="$new_version" '
    BEGIN {
      fallback_pattern = "^" key "=\"\\$\\{" key ":-[0-9]+\\.[0-9]+\\.[0-9]+\\}\""
      literal_pattern = "^" key "=\"[0-9]+\\.[0-9]+\\.[0-9]+\""
      replacement_done = 0
    }
    replacement_done == 0 && $0 ~ fallback_pattern {
      print key "=\"${" key ":-" new_version "}\""
      replacement_done = 1
      next
    }
    replacement_done == 0 && $0 ~ literal_pattern {
      print key "=\"" new_version "\""
      replacement_done = 1
      next
    }
    { print }
    END {
      if (replacement_done == 0) {
        exit 1
      }
    }
  ' "$file" > "$tmp" || {
    rm -f "$tmp"
    echo "Failed to update ${key} in ${file}" >&2
    exit 1
  }
  mv "$tmp" "$file"

  echo "${file}:${key}:${current_version}:${new_version}"
}

update_version_file() {
  local file="$1"
  local key="$2"
  local bump="$3"

  local current_version
  current_version="$(parse_version_file "$file" "$key")"
  local new_version
  new_version="$(bump_semver "$current_version" "$bump")"

  update_version_file_to "$file" "$key" "$new_version"
}

update_ugripper_versions() {
  local bump="$1"
  local primary_file="build_deb.sh"
  local helper_file="scripts/build_arm_deb_in_pp_arm_dev.sh"
  local primary_current
  primary_current="$(parse_version_file "$primary_file" "BASE_VERSION")"
  local helper_current
  helper_current="$(parse_version_file "$helper_file" "BASE_VERSION")"
  if [[ "$primary_current" != "$helper_current" ]]; then
    echo "Ugripper version mismatch before bump: ${primary_file}=${primary_current}, ${helper_file}=${helper_current}" >&2
    echo "Refusing to bump from an ambiguous base; align the two files first." >&2
    exit 1
  fi

  local new_version
  new_version="$(bump_semver "$primary_current" "$bump")"

  update_version_file_to "$primary_file" "BASE_VERSION" "$new_version"
  update_version_file_to "$helper_file" "BASE_VERSION" "$new_version"
}

TARGET="both"
BUMP="z"
REASON=""
NO_BUMP=false

while [[ $# -gt 0 ]]; do
  case "$1" in
    --target)
      if [[ $# -lt 2 ]]; then
        echo "--target requires a value" >&2
        exit 1
      fi
      case "$2" in
        ugripper|updater|both)
          TARGET="$2"
          ;;
        *)
          echo "Invalid --target value: $2" >&2
          exit 1
          ;;
      esac
      shift 2
      ;;
    --bump)
      if [[ $# -lt 2 ]]; then
        echo "--bump requires a value" >&2
        exit 1
      fi
      BUMP="$(normalize_bump "$2")"
      shift 2
      ;;
    --reason)
      if [[ $# -lt 2 ]]; then
        echo "--reason requires a value" >&2
        exit 1
      fi
      REASON="$2"
      shift 2
      ;;
    --no-bump)
      NO_BUMP=true
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

if [[ "$NO_BUMP" == true ]]; then
  echo "Version bump skipped (--no-bump)"
  if [[ -n "$REASON" ]]; then
    echo "Reason: ${REASON}"
  fi
  exit 0
fi

results=()

if [[ "$TARGET" == "ugripper" || "$TARGET" == "both" ]]; then
  ugripper_output="$(update_ugripper_versions "$BUMP")"
  while IFS= read -r line; do
    [[ -n "$line" ]] || continue
    results+=("$line")
  done <<< "$ugripper_output"
fi

if [[ "$TARGET" == "updater" || "$TARGET" == "both" ]]; then
  updater_output="$(update_version_file "../DASUsbUpdater/usb_updater_build.sh" "PKG_VERSION_BASE" "$BUMP")"
  results+=("$updater_output")
fi

case "$BUMP" in
  z) bump_label=".z" ;;
  y) bump_label=".y" ;;
  x) bump_label=".x" ;;
esac

echo "Version bump completed"
echo "Target: ${TARGET}"
echo "Bump level: ${bump_label}"
for entry in "${results[@]}"; do
  IFS=':' read -r file key old_v new_v <<< "$entry"
  echo "- ${file} ${key}: ${old_v} -> ${new_v}"
done
if [[ -n "$REASON" ]]; then
  echo "Reason: ${REASON}"
fi
