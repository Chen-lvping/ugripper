#!/usr/bin/env bash

ugripper_resolve_stereo_build_variant() {
    local requested="${UGRIPPER_ENABLE_STEREO:-ON}"
    local base_version="${VERSION:-${BASE_VERSION}${VERSION_SUFFIX:-}}"

    case "${requested,,}" in
        1|on|true|yes|enable|enabled)
            UGRIPPER_ENABLE_STEREO="ON"
            if [[ "$base_version" == *nostereo* ]]; then
                echo "nostereo is only valid when UGRIPPER_ENABLE_STEREO=OFF: $base_version" >&2
                return 1
            fi
            VERSION="$base_version"
            ;;
        0|off|false|no|disable|disabled)
            UGRIPPER_ENABLE_STEREO="OFF"
            if [[ "$base_version" == *nostereo* ]]; then
                VERSION="$base_version"
            else
                VERSION="${base_version}+nostereo"
            fi
            ;;
        *)
            echo "Invalid UGRIPPER_ENABLE_STEREO value: $requested" >&2
            return 1
            ;;
    esac

    export UGRIPPER_ENABLE_STEREO VERSION
}
