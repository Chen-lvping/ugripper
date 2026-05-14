#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "${script_dir}"

SRC_DIR="${SRC_DIR:-${script_dir}/auto_update}"
PKG_NAME="ugripper-usb-updater"
PKG_VERSION_BASE="${PKG_VERSION_BASE:-1.2.5}"
PKG_VERSION_SUFFIX="${PKG_VERSION_SUFFIX:-}"
PKG_VERSION="${PKG_VERSION:-${PKG_VERSION_BASE}${PKG_VERSION_SUFFIX}}"
ARCH="all"
MAINTAINER="User <user@example.com>"
DESC="Transition updater from ugripper-usb-updater to das-usb-updater"

BUILD_DIR="${BUILD_DIR:-${script_dir}/temp_build_usb_updater_transition}"
OUTPUT_DIR="${OUTPUT_DIR:-${script_dir}/build/package/updater-transition}"
OUTPUT_DEB="${OUTPUT_DEB:-${OUTPUT_DIR}/${PKG_NAME}_${PKG_VERSION}_${ARCH}.deb}"

DPKG_DEB_COMPRESSOR="${DPKG_DEB_COMPRESSOR:-xz}"
DPKG_DEB_LEVEL="${DPKG_DEB_LEVEL:-1}"
DPKG_DEB_STRATEGY="${DPKG_DEB_STRATEGY:-}"
DPKG_DEB_UNIFORM_COMPRESSION="${DPKG_DEB_UNIFORM_COMPRESSION:-}"

resolve_dpkg_deb_args() {
    DPKG_DEB_BUILD_ARGS=(--root-owner-group)

    if [ -n "${DPKG_DEB_COMPRESSOR}" ]; then
        DPKG_DEB_BUILD_ARGS+=("-Z${DPKG_DEB_COMPRESSOR}")
    fi
    if [ -n "${DPKG_DEB_LEVEL}" ]; then
        DPKG_DEB_BUILD_ARGS+=("-z${DPKG_DEB_LEVEL}")
    fi
    if [ -n "${DPKG_DEB_STRATEGY}" ]; then
        DPKG_DEB_BUILD_ARGS+=("-S${DPKG_DEB_STRATEGY}")
    fi

    case "${DPKG_DEB_UNIFORM_COMPRESSION}" in
        true|1|yes)
            DPKG_DEB_BUILD_ARGS+=(--uniform-compression)
            ;;
        false|0|no|"")
            ;;
        *)
            echo "错误：非法 DPKG_DEB_UNIFORM_COMPRESSION 值: ${DPKG_DEB_UNIFORM_COMPRESSION}" >&2
            exit 1
            ;;
    esac
}

echo "检查过渡包源文件..."
REQUIRED_FILES=(
    "usb_auto_update.sh"
    "mount_data_disk.sh"
    "99-usb-auto-update.rules"
    "usb-auto-update@.service"
    "boot_check_install.sh"
    "ugripper-boot-install.service"
)

for file in "${REQUIRED_FILES[@]}"; do
    if [ ! -f "$SRC_DIR/$file" ]; then
        echo "错误：在 $SRC_DIR 中找不到文件 $file" >&2
        exit 1
    fi
done

echo "正在清理并创建构建目录 $BUILD_DIR ..."
rm -rf "$BUILD_DIR"
mkdir -p "${OUTPUT_DIR}"
rm -f "${OUTPUT_DEB}"

mkdir -p "${BUILD_DIR}/usr/local/bin"
mkdir -p "${BUILD_DIR}/usr/local/lib/${PKG_NAME}"
mkdir -p "${BUILD_DIR}/usr/local/scripts/lib"
mkdir -p "${BUILD_DIR}/etc/udev/rules.d"
mkdir -p "${BUILD_DIR}/lib/systemd/system"
mkdir -p "${BUILD_DIR}/DEBIAN"

echo "复制过渡 updater 文件..."
awk '
    { print }
    $0 ~ /^[[:space:]]*"ugripper-usb-updater"[[:space:]]*$/ && inserted == 0 {
        print "  \"das-usb-updater\""
        inserted = 1
    }
' "$SRC_DIR/usb_auto_update.sh" > "${BUILD_DIR}/usr/local/bin/usb_auto_update.sh"
perl -0pi -e 's/dpkg -i --force-overwrite/dpkg --auto-deconfigure -i --force-overwrite/g' \
    "${BUILD_DIR}/usr/local/bin/usb_auto_update.sh"
chmod 755 "${BUILD_DIR}/usr/local/bin/usb_auto_update.sh"

cp "$SRC_DIR/mount_data_disk.sh" "${BUILD_DIR}/usr/local/bin/ugripper_mount_data_disk.sh"
chmod 755 "${BUILD_DIR}/usr/local/bin/ugripper_mount_data_disk.sh"
cp -a "${script_dir}/auto_calibration" "${BUILD_DIR}/usr/local/lib/${PKG_NAME}/"
find "${BUILD_DIR}/usr/local/lib/${PKG_NAME}/auto_calibration" -type f -name '*.sh' -exec chmod 755 {} \;
chmod 755 "${BUILD_DIR}/usr/local/lib/${PKG_NAME}/auto_calibration/generate_gripper_calibration_bin.py"
cp "${script_dir}/scripts/lib/ugripper_shell_common.sh" "${BUILD_DIR}/usr/local/scripts/lib/"
chmod 644 "${BUILD_DIR}/usr/local/scripts/lib/ugripper_shell_common.sh"
cp "$SRC_DIR/99-usb-auto-update.rules" "${BUILD_DIR}/etc/udev/rules.d/"
chmod 644 "${BUILD_DIR}/etc/udev/rules.d/99-usb-auto-update.rules"
cp "$SRC_DIR/usb-auto-update@.service" "${BUILD_DIR}/lib/systemd/system/"
chmod 644 "${BUILD_DIR}/lib/systemd/system/usb-auto-update@.service"
cp "$SRC_DIR/boot_check_install.sh" "${BUILD_DIR}/usr/local/bin/"
chmod 755 "${BUILD_DIR}/usr/local/bin/boot_check_install.sh"
cp "$SRC_DIR/ugripper-boot-install.service" "${BUILD_DIR}/lib/systemd/system/"
chmod 644 "${BUILD_DIR}/lib/systemd/system/ugripper-boot-install.service"

echo "生成过渡包控制文件和维护脚本..."
cat > "${BUILD_DIR}/DEBIAN/control" << EOF
Package: ${PKG_NAME}
Version: ${PKG_VERSION}
Section: admin
Priority: optional
Architecture: ${ARCH}
Depends: bash, systemd, udev, util-linux, python3
Maintainer: ${MAINTAINER}
Description: ${DESC}
 This temporary package keeps the legacy package name so deployed
 ugripper boards can install it with the old USB updater, then rerun
 once and install das-usb-updater from the same USB drive.
EOF

cat > "${BUILD_DIR}/DEBIAN/postinst" << EOF
#!/bin/sh
set -e

if [ "\$1" = "configure" ]; then
    echo "Configuring ${PKG_NAME} transition package..."
    udevadm control --reload-rules
    systemctl daemon-reload
    systemctl enable ugripper-boot-install.service

    # Old updater versions create this marker before installing an updater
    # package. For this transition package we still want the rerun, because
    # the rerun is what lets the new payload install das-usb-updater.
    rm -f /run/ugripper_usb_update_skip_postinst_rerun

    USB_UPDATE_DEV=\$(ps -eo args= | awk '
        /[u]sb_auto_update\.sh/ && \$0 !~ /--rerun-after-self-update/ {
            for (i = 1; i <= NF; ++i) {
                if (\$i ~ /^\\/dev\\//) {
                    print \$i
                    exit
                }
            }
        }
    ')

    if [ -n "\$USB_UPDATE_DEV" ]; then
        echo "Detected active usb_auto_update context on \$USB_UPDATE_DEV, scheduling transition rerun..."
        mkdir -p /var/log/ugripper
        USB_UPDATE_UNIT_SUFFIX=\$(printf '%s' "\$USB_UPDATE_DEV" | tr '/:@' '___')
        if command -v systemd-run >/dev/null 2>&1; then
            systemd-run --quiet --no-block --collect \\
                --unit="ugripper-usb-update-transition-rerun-\${USB_UPDATE_UNIT_SUFFIX}" \\
                --property=Type=oneshot \\
                --setenv=USB_UPDATE_DEV="\$USB_UPDATE_DEV" \\
                /bin/sh -lc '
                    mkdir -p /var/log/ugripper
                    echo "[\$(date "+%Y-%m-%d %H:%M:%S")] transition rerun for \$USB_UPDATE_DEV" >> /var/log/ugripper/usb_auto_update.log
                    attempts=0
                    while [ "\$attempts" -lt 90 ]; do
                        /usr/local/bin/usb_auto_update.sh "\$USB_UPDATE_DEV" --rerun-after-self-update
                        rc=\$?
                        if [ "\$rc" -ne 75 ]; then
                            exit 0
                        fi
                        attempts=\$((attempts + 1))
                        sleep 2
                    done
                    echo "[\$(date "+%Y-%m-%d %H:%M:%S")] transition rerun timed out waiting for lock: \$USB_UPDATE_DEV" >> /var/log/ugripper/usb_auto_update.log
                    exit 0
                '
        fi
    fi
fi

exit 0
EOF
chmod 755 "${BUILD_DIR}/DEBIAN/postinst"

cat > "${BUILD_DIR}/DEBIAN/prerm" << EOF
#!/bin/sh
set -e
if [ "\$1" = "remove" ]; then
    systemctl disable --now ugripper-boot-install.service || true
fi
exit 0
EOF
chmod 755 "${BUILD_DIR}/DEBIAN/prerm"

cat > "${BUILD_DIR}/DEBIAN/postrm" << EOF
#!/bin/sh
set -e
if [ "\$1" = "remove" ] || [ "\$1" = "purge" ]; then
    udevadm control --reload-rules
    systemctl daemon-reload
fi
exit 0
EOF
chmod 755 "${BUILD_DIR}/DEBIAN/postrm"

echo "开始构建过渡 .deb 包..."
resolve_dpkg_deb_args
dpkg-deb "${DPKG_DEB_BUILD_ARGS[@]}" --build "$BUILD_DIR" "${OUTPUT_DEB}"

echo "========================================"
echo "构建完成！"
ls -lh "${OUTPUT_DEB}"
echo "OUTPUT_DEB=${OUTPUT_DEB}"
echo "========================================"
