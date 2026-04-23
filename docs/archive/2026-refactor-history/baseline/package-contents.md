# Package Contents Baseline

本文档冻结当前 `pp_main/package_ugripper_stage.sh` + `pp_main/package_ugripper_deb.sh` 的真实收包行为。

## 1. 包元信息

| 项目 | 当前值 | source |
| --- | --- | --- |
| `APP_NAME` | `ugripper` | `package_ugripper_deb.sh` |
| `VERSION` | `1.2.8` | `package_ugripper_deb.sh` |
| `ARCH` | `arm64` | `package_ugripper_deb.sh` |
| `INSTALL_DIR` | `/opt/ugripper` | `package_ugripper_stage.sh` |
| Debian package name | `{{APP_NAME}}` -> `ugripper` | `pack_script/control`, `package_ugripper_deb.sh` |
| 运行时依赖 | `bash, systemd, udev, kmod, pulseaudio-utils, sox, alsa-utils, libusb-1.0-0` | `pack_script/control` |

## 2. 当前组包方式

当前打包主流程：

1. 从 `pp_main/install/<platform>` 的 standalone install tree 收集 `CameraRecorder`、`SensorRecorder`、`GripperHmiTool`、`UgripperRuntime` 四个迁移对象的新布局内容
2. 再补回 `ugripper` 仓库真源脚本与少量 `py_script` 文件
3. 再复制 systemd unit、udev 规则和 `DEBIAN` 脚本
4. 最终由 `dpkg-deb` 产出 `.deb`

source: `pp_main/package_ugripper_stage.sh`, `pp_main/package_ugripper_deb.sh`

补充说明：

- 当前默认交付口径已经固定为纯新布局
- 不再存在 `UGRIPPER_PACKAGE_INCLUDE_LEGACY_COMPAT`
- 不再从 install tree 收 `build/src/`、顶层 `audio/`、顶层 `audio_en/`、顶层 `config/`

## 3. 当前默认会进入包的 app/install 内容

以下内容当前会从 standalone install tree 进入 `/opt/ugripper`：

- `bin/CameraRecorder/CameraRecorder`
- `bin/CameraRecorder/config/camera_recorder.yaml`
- `bin/SensorRecorder/SensorRecorder`
- `bin/SensorRecorder/zeroing`
- `bin/SensorRecorder/99-serial.rules`
- `bin/GripperHmiTool/GripperHmiTool`
- `bin/UgripperRuntime/UgripperRuntime`
- `bin/UgripperRuntime/audio/*`
- `bin/UgripperRuntime/audio_en/*`
- `bin/UgripperRuntime/config/fakeCamCalib.json`
- `lib/` 下 standalone 产出的共享库（若存在）

source: `pp_main/package_ugripper_stage.sh`, `pp_main/standalone/*/CMakeLists.txt`

## 4. 当前默认会进入包的仓库真源脚本/资源

以下内容当前会从 `ugripper` 仓库直接进入 `/opt/ugripper`：

- `run_record.sh`
- `scripts/`
- `auto_update/`
- `auto_calibration/`
- `py_script/usb_audio_play_test.py`
- `py_script/usb_audio_mic_test.py`
- `py_script/usb_audio_noise_profile.py`

source: `pp_main/package_ugripper_stage.sh`

## 5. 当前不会进入包的 legacy 兼容内容

以下内容当前默认不会进入包：

- `/opt/ugripper/build/src/...`
- `/opt/ugripper/audio/...`
- `/opt/ugripper/audio_en/...`
- `/opt/ugripper/config/fakeCamCalib.json`

source: `pp_main/package_ugripper_stage.sh`, `pp_main/standalone/*/CMakeLists.txt`

## 6. 当前安装的 systemd 单元

当前 package staging 会复制到 `/etc/systemd/system/` 的文件：

- `ugripper.service`
- `ugripper-calibration.service`
- `ugripper-network-monitor.service`
- `umi-shutdown-trigger.service`
- `umi-shutdown-trigger.path`

source: `pp_main/package_ugripper_stage.sh`

## 7. 当前安装的 udev 规则

| 目标路径 | 来源 | source |
| --- | --- | --- |
| `/etc/udev/rules.d/99-fixed-usb-map.rules` | `config/99-fixed-usb-map.rules` | `pp_main/package_ugripper_stage.sh` |
| `/etc/udev/rules.d/99-serial.rules` | `src/sensor_recorder/99-serial.rules` | `pp_main/package_ugripper_stage.sh` |

## 8. 当前安装的 maintainer scripts

当前会进入 `DEBIAN/` 的文件：

- `control`
- `postinst`
- `prerm`
- `postrm`

source: `pp_main/package_ugripper_stage.sh`

## 9. 当前打包与 service 的已知边界

以下仍是当前主线之外、暂未纳入这轮切换的对象：

1. 仓库中存在 `auto_update/usb-auto-update@.service`
   - 其 `ExecStart` 指向 `/usr/local/bin/usb_auto_update.sh /dev/%I`
   - 当前已明确目标归属：由可选独立包 `ugripper-usb-updater` 提供，而不是主 `ugripper` 包提供

2. 仓库中存在 `auto_update/ugripper-boot-install.service`
   - 其依赖 `/usr/local/bin/boot_check_install.sh`
   - 当前已明确目标归属：由可选独立包 `ugripper-usb-updater` 提供，而不是主 `ugripper` 包提供

3. `ugripper-calibration.service` 会被复制进包
   - 但 `postinst` 当前不会自动 enable / restart 它

4. 当前仍有一个待继续收口的边界
   - 主 `ugripper` 包安装的 `config/99-fixed-usb-map.rules` 现已只负责数据盘挂载
   - 可选独立包 `ugripper-usb-updater` 安装的 `auto_update/99-usb-auto-update.rules` 现已单独负责通过 `SYSTEMD_WANTS` 拉起 `usb-auto-update@%k.service`
   - `auto_update/mount_data_disk.sh` 也已不再主动 `systemctl start usb-auto-update@...`
   - 后续重点转为验证“只装主包”和“主包 + updater 同装”两种安装场景

source: `auto_update/usb-auto-update@.service`, `auto_update/ugripper-boot-install.service`, `auto_update/99-usb-auto-update.rules`, `auto_update/mount_data_disk.sh`, `config/99-fixed-usb-map.rules`, `pp_main/package_ugripper_stage.sh`, `pack_script/postinst`

## 10. 当前包安装后自动动作

`postinst` 当前会：

- 停止并清理旧 `ugripper.service` / `ugripper-network-monitor.service`
- 初始化 `/etc/ugripper/config/calibration/calibration.json`
- 使用 `/opt/ugripper/bin/UgripperRuntime/config/fakeCamCalib.json` 作为默认恢复样例
- 配置 exfat 自动加载
- `daemon-reload`
- reload / trigger 部分 udev 子系统
- enable + restart：
  - `ugripper.service`
  - `ugripper-network-monitor.service`
  - `umi-shutdown-trigger.service`
  - `umi-shutdown-trigger.path`

source: `pack_script/postinst`
