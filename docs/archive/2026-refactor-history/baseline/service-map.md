# Service Map Baseline

本文档冻结当前仓库内与 `systemd` 相关的 unit 映射关系。
重点区分三类事实：

- 仓库里有哪些 unit 文件
- 当前 `build_deb.sh` 会安装哪些 unit
- 当前 `postinst` 会 enable / restart 哪些 unit

## 1. 主链服务

| Unit | 当前状态 | ExecStart / 触发 | User / WorkingDirectory | source | 备注 |
| --- | --- | --- | --- | --- | --- |
| `ugripper.service` | 被 `build_deb.sh` 安装；被 `postinst` enable + restart | `/bin/bash -lc '/opt/ugripper/run_record.sh'` | `User=ubuntu`, `WorkingDirectory=/opt/ugripper` | `pack_script/ugripper.service`, `build_deb.sh`, `pack_script/postinst` | 主服务入口 |

## 2. 校准与网络相关服务

| Unit | 当前状态 | ExecStart / 触发 | User / WorkingDirectory | source | 备注 |
| --- | --- | --- | --- | --- | --- |
| `ugripper-calibration.service` | 被 `build_deb.sh` 安装；当前未见 `postinst` 自动 enable | `/bin/bash /opt/ugripper/auto_calibration/run_calibration.sh` | `User=root`, `WorkingDirectory=/opt/ugripper` | `auto_calibration/ugripper-calibration.service`, `build_deb.sh`, `pack_script/postinst` | 手工/外部触发型校准任务 |
| `ugripper-network-monitor.service` | 被 `build_deb.sh` 安装；被 `postinst` enable + restart | `/bin/bash /opt/ugripper/auto_calibration/monitor_network.sh` | `User=root` | `auto_calibration/ugripper-network-monitor.service`, `build_deb.sh`, `pack_script/postinst` | 安装完成后自动运行 |

## 3. 关机触发链路

| Unit | 当前状态 | ExecStart / 触发 | User / WorkingDirectory | source | 备注 |
| --- | --- | --- | --- | --- | --- |
| `umi-shutdown-trigger.path` | 被 `build_deb.sh` 安装；被 `postinst` enable + restart | 监听 `PathExists=/tmp/umi_shutdown_request` | 无 | `auto_update/umi-shutdown-trigger.path`, `build_deb.sh`, `pack_script/postinst` | 由 `record_runtime` 侧写入关机请求文件触发 |
| `umi-shutdown-trigger.service` | 被 `build_deb.sh` 安装；被 `postinst` enable | `{{INSTALL_DIR}}/auto_update/trigger_shutdown.sh` | 无 | `auto_update/umi-shutdown-trigger.service`, `build_deb.sh`, `pack_script/postinst` | 与 path unit 配对 |

## 4. 仓库中存在但当前打包脚本未安装的 unit

| Unit | 当前状态 | ExecStart / 触发 | source | 备注 |
| --- | --- | --- | --- | --- |
| `usb-auto-update@.service` | 仓库存在；不属于主 `ugripper` 包当前 staging | `/usr/local/bin/usb_auto_update.sh /dev/%I` | `auto_update/usb-auto-update@.service`, `usb_updater_build.sh` | 已明确目标归属为可选独立包 `ugripper-usb-updater`；主包不应对其形成硬依赖 |
| `ugripper-boot-install.service` | 仓库存在；不属于主 `ugripper` 包当前 staging | `/usr/local/bin/boot_check_install.sh` | `auto_update/ugripper-boot-install.service`, `usb_updater_build.sh` | 同上，属于可选 updater / boot-restore 链 |

## 5. 维护脚本中的服务操作

### `postinst`

当前会操作这些 unit：

- stop:
  - `ugripper.service`
  - `ugripper-network-monitor.service`
- enable + restart:
  - `ugripper.service`
  - `ugripper-network-monitor.service`
  - `umi-shutdown-trigger.service`
  - `umi-shutdown-trigger.path`

source: `pack_script/postinst`

### `prerm`

当前会操作这些 unit：

- stop:
  - `ugripper.service`
  - `ugripper-network-monitor.service`
- disable:
  - `ugripper.service`
  - `ugripper-network-monitor.service`
  - `umi-shutdown-trigger.service`
  - `umi-shutdown-trigger.path`

source: `pack_script/prerm`

## 6. 当前主服务依赖与顺序

### `ugripper.service`

- `After=network.target local-fs.target`
- `Restart=always`
- `RestartSec=3`
- `TimeoutStopSec=20s`
- `KillMode=control-group`

source: `pack_script/ugripper.service`

### `ugripper-network-monitor.service`

- `After=systemd-udev-settle.service`
- `Wants=systemd-udev-settle.service`
- `Restart=always`

source: `auto_calibration/ugripper-network-monitor.service`

## 7. 需要后续确认的隐式依赖

以下内容目前已知与服务链路强相关，应在后续 review 中重点确认：

- `/opt/ugripper/run_record.sh`
- `/opt/ugripper/auto_update/trigger_shutdown.sh`
- `/tmp/umi_shutdown_request`
- `/opt/ugripper/auto_calibration/monitor_network.sh`
- `/usr/local/bin/usb_auto_update.sh` 是否由其他包/流程提供
