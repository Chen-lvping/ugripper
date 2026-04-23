# Deploy / Script Boundary Baseline

本文档冻结 `Stage B / B3` 的 deploy/script 归属边界。

目标不是立即改 `pp_main` 代码，而是先回答：

- 哪些内容继续留在 deploy/script 链路，不提前 app 化
- 哪些脚本/服务与 `UgripperRuntime`、`GripperHmiTool`、`zeroing` 等 app 形成调用边界
- 哪些问题必须等 `B4` 的安装路径 / service / 打包切换时再验证

当前结论偏保守：

- `Updater` 先归入未来主仓库的 `deploy/update` 链路
- `Calibration` 先归入未来主仓库的 `deploy/calibration` 链路
- `trigger_shutdown` 与其 `path/service` 先归入未来主仓库的 `deploy/systemd` 链路
- 以上对象当前都**不**纳入首批 standalone app 成功标准

说明：

- 截至当前检查，`pp_main` 里还没有现成的 `deploy/update`、`deploy/calibration` 目录
- 因此 `B3` 的交付物是“归属冻结文档”，不是“主仓库已正式落位”

## 1. 归属总表

| 对象 | 当前 source | 当前触发方式 | 建议主仓库归属 | 当前是否 app 化 | 后续节点 |
| --- | --- | --- | --- | --- | --- |
| `usb_auto_update.sh` | `auto_update/usb_auto_update.sh` | `usb-auto-update@.service` / udev 存储设备事件 | `deploy/update/` | 否 | `B4` |
| `boot_check_install.sh` | `auto_update/boot_check_install.sh` | `ugripper-boot-install.service` | `deploy/update/` | 否 | `B4` |
| `run_calibration.sh` | `auto_calibration/run_calibration.sh` | `ugripper-calibration.service` / 手工触发 | `deploy/calibration/` | 否 | `B4` |
| `import_camera_calibration.sh` | `auto_calibration/import_camera_calibration.sh` | 被 `usb_auto_update.sh` 调用 | `deploy/calibration/` | 否 | `B4` |
| `generate_gripper_calibration_bin.py` | `auto_calibration/generate_gripper_calibration_bin.py` | 被 `import_camera_calibration.sh` 调用 | `deploy/calibration/` | 否 | `B4` |
| `monitor_network.sh` | `auto_calibration/monitor_network.sh` | `ugripper-network-monitor.service` | `deploy/calibration/` | 否 | `B4` |
| `network_iface_lib.sh` | `auto_calibration/network_iface_lib.sh` | 被 `monitor_network.sh` 调用 | `deploy/calibration/` | 否 | `B4` |
| `trigger_shutdown.sh` | `auto_update/trigger_shutdown.sh` | `umi-shutdown-trigger.service` | `deploy/systemd/` | 否 | `B4` |
| `umi-shutdown-trigger.path/service` | `auto_update/umi-shutdown-trigger.*` | 安装后 `systemd` 启用 | `deploy/systemd/` | 否 | `B4` |

## 2. 与 app 的真实边界

### 2.1 `UgripperRuntime`

当前 app 侧负责：

- supervisor 主控与子进程启停
- `AudioCommandPort`、`StereoSessionPort`、`ShutdownRequestPort` 默认 file backend
- 对 `camera_recorder`、`sensor_recorder`、audio 脚本的运行时协调

当前 deploy/script 侧负责：

- `run_record.sh` 作为 service 入口壳
- `umi-shutdown-trigger.path/service` 的安装后启用
- `trigger_shutdown.sh` 的 system shutdown 执行

边界要求：

- `UgripperRuntime` 不在 `B3` 直接接管 systemd unit
- `trigger_shutdown.sh` 不在 `B3` 被吸进 standalone app
- `run_record.sh` 的真实安装路径切换后移到 `B4`

### 2.2 `GripperHmiTool`

当前 app / helper 侧负责：

- 夹爪串口命令、LED 状态、SN 读取等 helper 能力

当前 deploy/script 侧负责：

- `usb_auto_update.sh` 中的升级提示灯效
- `run_calibration.sh` 中的校准提示灯效
- `import_camera_calibration.sh` 中的 SN 读取与写标定结果

边界要求：

- `gripper_hmi_test` / `GripperHmiTool` 继续作为被脚本调用的 helper
- `B3` 不改脚本对 helper 的调用形态
- helper 最终命名兼容策略可在 `B4` 再决定

### 2.3 `SensorRecorder / zeroing`

当前 app 侧负责：

- 录制期的 `SensorRecorder`
- 校准期的 `zeroing` helper

当前 deploy/script 侧负责：

- `run_calibration.sh` 对 `zeroing` 的超时、日志、root/user 切换和 service 停启

边界要求：

- `zeroing` 继续作为 calibration 链路中的 helper 可执行
- `B3` 不把校准流程重写成 standalone app 生命周期

## 3. 脚本链路细化

### 3.1 Update 链路

| 对象 | 当前安装/触发事实 | 关键依赖 | 风险点 |
| --- | --- | --- | --- |
| `usb_auto_update.sh` | 当前仓库内脚本；`usb-auto-update@.service` 指向 `/usr/local/bin/usb_auto_update.sh /dev/%I`；目标归属已明确为可选独立包 `ugripper-usb-updater` | `gripper_hmi_test`、`import_camera_calibration.sh`、`/mnt/data_disk`、`ugripper-network-monitor.service` | 板端已验证“挂载由主包负责、service 触发由 updater 包负责”可稳定协作；剩余风险集中在真实导包/导配置/导标定的完整 U 盘交付内容 |
| `boot_check_install.sh` | `ugripper-boot-install.service` 依赖 `/usr/local/bin/boot_check_install.sh`；目标归属已明确为可选独立包 `ugripper-usb-updater` | 安装环境、系统启动时机 | 作为选配功能，不应要求主 `ugripper` 包安装时默认携带 |
| `99-usb-auto-update.rules` | 当前由 updater 包提供真实触发规则；只负责对允许的 USB 分区添加 `SYSTEMD_WANTS+=usb-auto-update@%k.service` | udev block 设备事件 | 必须与主包 `config/99-fixed-usb-map.rules` 对允许物理 USB 口的口径保持一致 |

### 3.2 Calibration 链路

| 对象 | 当前安装/触发事实 | 关键依赖 | 风险点 |
| --- | --- | --- | --- |
| `run_calibration.sh` | `ugripper-calibration.service` 指向 `/opt/ugripper/auto_calibration/run_calibration.sh` | `zeroing`、`gripper_hmi_test`、`audio/audio_play.py`、`/tmp/umi_audio_pipe`、`/mnt/data_disk` | 同时牵涉 service 停启、用户切换、FIFO、helper 进程清理 |
| `import_camera_calibration.sh` | 被 `usb_auto_update.sh` 调用 | `gripper_hmi_test`、`generate_gripper_calibration_bin.py`、`/etc/environment` | 依赖安装后路径、SN 读取输出格式和现场 U 盘目录结构 |
| `monitor_network.sh` | `ugripper-network-monitor.service` 指向 `/opt/ugripper/auto_calibration/monitor_network.sh` | `network_iface_lib.sh`、`ugripper.service` | 与升级保护锁、网线插拔重启策略耦合，不适合提前 app 化 |

### 3.3 Shutdown 链路

| 对象 | 当前安装/触发事实 | 关键依赖 | 风险点 |
| --- | --- | --- | --- |
| `umi-shutdown-trigger.path` | 监听 `/tmp/umi_shutdown_request` | `ShutdownRequestPort` 默认 file backend | 必须与运行时写文件语义一致 |
| `umi-shutdown-trigger.service` | 调用 `trigger_shutdown.sh` | 安装后脚本路径 | service unit 与脚本路径必须一起切换 |
| `trigger_shutdown.sh` | 删除请求文件后执行 `systemctl poweroff` | `/tmp/umi_shutdown_request` | 这条链只能在真实安装环境里做最终验证 |

## 4. `B3` 明确不做的事情

- 不把 `Updater` 做成 standalone app
- 不把 `Calibration` 做成 standalone app
- 不切换 `run_record.sh`、service、安装路径、打包路径
- 不改 `usb_auto_update.sh` / `run_calibration.sh` 的主流程语义
- 不把 `ShutdownRequestPort` 从 file backend 改写成 ZMQ 或其他机制

## 5. 进入 `B4` 前必须继续验证的事项

### 主仓库文档/归属层

- `pp_main` 中 update / calibration / systemd 脚本的真实落位目录
- `build/package/install` 对这些脚本与 unit 的收包方式
- helper binary 与脚本的安装后相对路径
- 主包 `99-fixed-usb-map.rules` 与 updater 包自动升级触发规则的一致性维护方式

### 必须等安装环境验证的事项

- 可选包 `ugripper-usb-updater` 安装后：
  - `/usr/local/bin/usb_auto_update.sh` 与 `usb-auto-update@.service` 是否真正对齐
    - 已在板端验证通过
    - 剩余建议先走模拟 smoke：
      - 手动 `systemctl start usb-auto-update@<dev>.service`
      - 第一轮仅放 `config.txt`
        - 已验证通过：可更新 `/etc/environment` 中的 `UGRIPPER_LANG` / `CAMERA_CODEC`，并单次重启 `ugripper.service`
      - 第二轮再放 `ugripper_calib/`
      - 暂不把根目录 `calibration.txt` 作为当前门禁，因为它会触发 `ugripper-calibration.service`
  - `/usr/local/bin/boot_check_install.sh` 与 `ugripper-boot-install.service` 是否真正对齐
    - 已验证安装、enable 与手动 `systemctl start` no-op smoke
    - 已验证 `/opt/backup` 存在同版本包时可正确识别“无需升级”
    - 已验证 `ugripper` 处于 `deinstall ok config-files` 状态时，可从 `/opt/backup/ugripper_1.2.8_arm64.deb` 自动恢复安装并回到 `install ok installed`
    - 后续可继续用手动 `systemctl start ugripper-boot-install.service` 模拟：
      - 更高版本 backup 包升级
      - 包缺失或状态异常时的恢复安装
    - 但这仍不能替代真实 reboot 时序验证
    - 尚未做真实 reboot 触发的恢复 smoke
  - 主包挂载规则与 updater 包触发规则是否可在同一次插盘流程里稳定协作
    - 已在板端验证通过
- 未安装 `ugripper-usb-updater` 时：
  - 主包数据盘挂载规则是否仍可独立工作
  - 系统是否不会再尝试拉起不存在的 `usb-auto-update@.service`
    - 已在板端验证通过
- `ugripper-usb-updater` 卸载 / 重装时：
  - `usb-auto-update@.service`、`ugripper-boot-install.service` 与 `99-usb-auto-update.rules` 是否随包生命周期正确移除和恢复
    - 已在板端验证通过
- `/opt/.../run_calibration.sh`、`monitor_network.sh`、`trigger_shutdown.sh` 的真实安装后路径
- `umi-shutdown-trigger.path/service` 与 `ShutdownRequestPort` 的实际联动
- `run_calibration.sh` / `usb_auto_update.sh` 的 `L3` 交付 smoke

## 6. Source

- `auto_update/usb_auto_update.sh`
- `auto_update/boot_check_install.sh`
- `auto_update/trigger_shutdown.sh`
- `auto_update/usb-auto-update@.service`
- `auto_update/ugripper-boot-install.service`
- `auto_update/umi-shutdown-trigger.path`
- `auto_update/umi-shutdown-trigger.service`
- `auto_update/99-usb-auto-update.rules`
- `auto_calibration/run_calibration.sh`
- `auto_calibration/import_camera_calibration.sh`
- `auto_calibration/generate_gripper_calibration_bin.py`
- `auto_calibration/monitor_network.sh`
- `auto_calibration/network_iface_lib.sh`
- `auto_calibration/ugripper-calibration.service`
- `auto_calibration/ugripper-network-monitor.service`
- `docs/archive/2026-refactor-history/baseline/service-map.md`
- `docs/archive/2026-refactor-history/baseline/package-contents.md`
