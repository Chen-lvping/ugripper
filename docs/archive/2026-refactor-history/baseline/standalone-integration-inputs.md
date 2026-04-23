# Standalone Integration Inputs Baseline

本文档冻结 `Stage B / B1` 的主仓库并仓输入。

目标不是提前改主仓库代码，而是先回答：

- 4 个待迁对象各自依赖什么
- 各自需要带哪些运行时资源和安装物
- 各自由谁启动、与哪些脚本或 service 相连
- 哪些内容不应进入 standalone app，而应继续留在 deploy/script 区域

当前表格面向 4 个对象：

1. `CameraRecorder`
2. `SensorRecorder`
3. `GripperHmiTool`
4. `UgripperRuntime`

## 1. 总体结论

当前最稳的迁移顺序仍然是：

1. `CameraRecorder`
2. `SensorRecorder`
3. `GripperHmiTool`
4. `UgripperRuntime`

原因：

- `CameraRecorder` 和 `SensorRecorder` 已具备相对清晰的 app 壳与纯逻辑边界
- `GripperHmiTool` 目前更接近“库 + helper binary”，归属需要先确认
- `UgripperRuntime` 同时牵动 HMI、audio、stereo、shutdown、service 和脚本，是最高风险入口，必须最后迁

## 2. CameraRecorder

| 字段 | 当前事实 |
| --- | --- |
| 建议 standalone 目录 | `standalone/CameraRecorder/` |
| 建议 target | `CameraRecorder` app；保留 `camera_domain` 纯逻辑库或等价内部 target |
| 当前源码真源 | `src/camera_recorder/include/camera_recorder/*`；`src/camera_recorder/src/main.cpp`；`src/camera_recorder/src/camera_recorder.cpp`；`src/camera_recorder/src/camera_domain.cpp` |
| 当前构建目标 | `camera_domain` 静态/对象库风格目标；`camera_recorder` 可执行 |
| 直接构建依赖 | `utils`、`yaml-cpp`、`libusb-1.0`、`pthread` |
| 关键运行时资源 | `config/camera_recorder.yaml`、`/tmp/umi_stereo_camera_control.json`、`/tmp/umi_stereo_camera_status.json` |
| 关键运行时输入 | `--output-dir`、`--codec`、`--config-yaml`、camera 设备节点、stereo daemon 模式 |
| 被谁拉起 | 当前由 `record_runtime` 拉起；也支持手工 CLI / `--dry-run` |
| 当前安装后位置 | 新优先 `/opt/ugripper/bin/CameraRecorder/CameraRecorder`；兼容入口 `/opt/ugripper/build/src/camera_recorder/camera_recorder` |
| 相关 service | 无直接独立 service；当前属于 `ugripper.service -> run_record.sh -> record_runtime` 间接链路 |
| 建议一起迁入的内容 | app 代码、camera config/schema、stereo control/status file backend 接口边界 |
| 明确不一起迁入的内容 | `run_record.sh`、service 切换、最终安装路径切换、`Updater / Calibration` |
| deploy/script 保留项 | 无必须随 app 迁入的 deploy 脚本；当前先只保留 config 和 runtime file contract |
| 主风险 | 主仓库侧 config 安装路径、camera 设备枚举、`--dry-run` 行为、stereo control/status 文件路径 |
| 建议测试门禁 | 主仓库本地编译 -> host-only 单测 -> `--help` / `--dry-run` -> Docker ARM -> ARM `L2` smoke |

source:

- `src/camera_recorder/CMakeLists.txt`
- `src/camera_recorder/include/camera_recorder/camera_recorder.h`
- `docs/archive/2026-refactor-history/baseline/runtime-entrypoints.md`
- `docs/archive/2026-refactor-history/baseline/runtime-paths.md`
- `docs/archive/2026-refactor-history/baseline/package-contents.md`

## 3. SensorRecorder

| 字段 | 当前事实 |
| --- | --- |
| 建议 standalone 目录 | `standalone/SensorRecorder/` |
| 建议 target | `SensorRecorder` app；保留 `sensor_protocol`、`sensor_domain` 可测试库；`zeroing` 视情况保留为同目录 helper app |
| 当前源码真源 | `src/sensor_recorder/include/sensor_recorder/*`；`src/sensor_recorder/src/main.cpp`；`src/sensor_recorder/src/sensor_protocol.cpp`；`src/sensor_recorder/src/sensor_domain.cpp`；`src/sensor_recorder/src/im648_driver.cpp`；`src/sensor_recorder/src/encoder_driver.cpp`；`src/sensor_recorder/src/im648_CMD.cpp`；`src/sensor_recorder/src/zeroing.cpp` |
| 当前构建目标 | `sensor_protocol` 库；`sensor_domain` 库；`sensor_recorder` 可执行；`zeroing` 可执行 |
| 直接构建依赖 | `utils`、`mcap`、`libserialport`、`pthread` |
| 关键运行时资源 | `/etc/udev/rules.d/99-serial.rules`、输出目录、左右 IMU/encoder 串口设备 |
| 关键运行时输入 | 设备串口 `/dev/right_imu`、`/dev/left_imu`、编码器串口；输出路径；`zeroing` 的 side/path 参数 |
| 被谁拉起 | 当前由 `record_runtime` 拉起；`zeroing` 由 `auto_calibration/run_calibration.sh` 拉起 |
| 当前安装后位置 | 新优先 `/opt/ugripper/bin/SensorRecorder/SensorRecorder`、`/opt/ugripper/bin/SensorRecorder/zeroing`；兼容入口 `/opt/ugripper/build/src/sensor_recorder/sensor_recorder`、`/opt/ugripper/build/src/sensor_recorder/zeroing` |
| 相关 service | 无直接独立 service；主链属于 `record_runtime`，校准链属于 `ugripper-calibration.service` |
| 建议一起迁入的内容 | app 代码、`sensor_protocol` / `sensor_domain`、与 `zeroing` 共享的协议层 |
| 明确不一起迁入的内容 | `run_calibration.sh`、安装后校准链路、service 切换 |
| deploy/script 保留项 | `auto_calibration/run_calibration.sh` 继续留在 deploy/script；`zeroing` 先作为配套 helper 保留，不强制立即 app 化 |
| 主风险 | 串口依赖与主仓库现有驱动框架的接线、`99-serial.rules` 安装归属、`zeroing` 与 calibration 脚本的路径关系 |
| 建议测试门禁 | 主仓库本地编译 -> host-only 单测 -> `SensorRecorder` 最小启动 / `zeroing` usage -> Docker ARM -> ARM `L2` smoke |

source:

- `src/sensor_recorder/CMakeLists.txt`
- `src/sensor_recorder/src/main.cpp`
- `docs/archive/2026-refactor-history/baseline/runtime-entrypoints.md`
- `docs/archive/2026-refactor-history/baseline/runtime-paths.md`
- `docs/archive/2026-refactor-history/baseline/package-contents.md`

## 4. GripperHmiTool

| 字段 | 当前事实 |
| --- | --- |
| 建议 standalone 目录 | `standalone/GripperHmiTool/` |
| 建议 target | `GripperHmiTool` helper app + `gripper_hmi` 库；纯逻辑可继续拆为 `gripper_hmi_protocol` / `gripper_hmi_led_effects` |
| 当前源码真源 | `src/gripper_hmi/include/*`；`src/gripper_hmi/src/gripper_hmi_protocol.cpp`；`src/gripper_hmi/src/gripper_hmi_driver.cpp`；`src/gripper_hmi/src/gripper_hmi_led_effects.cpp`；`src/gripper_hmi/test/gripper_hmi_test.cpp` |
| 当前构建目标 | `gripper_hmi` 库；`gripper_hmi_test` helper binary |
| 直接构建依赖 | `utils`、`libserialport`；helper binary 额外依赖 `pthread` |
| 关键运行时资源 | gripper 串口设备 `/dev/right_gripper`、`/dev/left_gripper` |
| 关键运行时输入 | `--port`、`--state`、`--led-only`、`--duration`、校准读写参数 |
| 被谁拉起 | 当前不由主 service 直接拉起；由 `usb_auto_update.sh`、`run_calibration.sh`、`import_camera_calibration.sh` 作为 helper 调用 |
| 当前安装后位置 | 新优先 `/opt/ugripper/bin/GripperHmiTool/GripperHmiTool`；兼容入口 `/opt/ugripper/build/src/gripper_hmi/gripper_hmi_test` |
| 相关 service | 无直接 service；被 update/calibration 脚本间接调用 |
| 建议一起迁入的内容 | `gripper_hmi` 库与 helper binary 的最小闭环 |
| 明确不一起迁入的内容 | update/calibration 脚本本体、service 切换 |
| deploy/script 保留项 | `auto_update/usb_auto_update.sh`、`auto_calibration/run_calibration.sh`、`auto_calibration/import_camera_calibration.sh` 继续留在 deploy/script，并通过 helper 路径调用 |
| 主风险 | 当前 helper 二进制名称带 `test`，但已被现场脚本当成正式工具使用；并仓时需要先决定是保留兼容名，还是加适配层 |
| 建议测试门禁 | 主仓库本地编译 -> `gripper_hmi` host-only 单测 -> helper `--help` / `--state READY` 最小验证 -> Docker ARM -> ARM `L1-L2` |

当前额外确认项：

- 当前已确认按“`standalone` 目录下的 helper tool + `gripper_hmi` 运行时库”迁移
- 当前仍待确认现场脚本切换时是否保留兼容名 `gripper_hmi_test`

source:

- `src/gripper_hmi/CMakeLists.txt`
- `src/gripper_hmi/test/gripper_hmi_test.cpp`
- `auto_update/usb_auto_update.sh`
- `auto_calibration/run_calibration.sh`
- `auto_calibration/import_camera_calibration.sh`
- `docs/archive/2026-refactor-history/baseline/runtime-entrypoints.md`
- `docs/archive/2026-refactor-history/baseline/runtime-paths.md`
- `docs/archive/2026-refactor-history/baseline/package-contents.md`

## 5. UgripperRuntime

| 字段 | 当前事实 |
| --- | --- |
| 建议 standalone 目录 | `standalone/UgripperRuntime/` |
| 建议 target | `UgripperRuntime` app；保留 `runtime_process_boundary`、`runtime_control_plane` 或等价内部 target |
| 当前源码真源 | `src/record_runtime/include/record_runtime/*`；`src/record_runtime/include/record_runtime.h`；`src/record_runtime/src/main.cpp`；`src/record_runtime/src/record_runtime.cpp`；`src/record_runtime/src/runtime_process.cpp`；`src/record_runtime/src/runtime_domain.cpp` |
| 当前构建目标 | `runtime_process_boundary` 库；`runtime_control_plane` 库；`record_runtime` 可执行 |
| 直接构建依赖 | `utils`、`gripper_hmi`、`mcap`、`pthread`；本地测试期还依赖 `nlohmann` / `libserialport` 头库路径 |
| 关键运行时资源 | `audio/audio_play.py`、`audio/record_usb_audio.py`、`audio/noise.prof`、`config/fakeCamCalib.json`、`/etc/environment`、`/etc/ugripper/config/calibration/calibration.json`、`/mnt/data_disk` |
| 关键运行时通道 | `/tmp/umi_audio_pipe`、`/tmp/umi_audio_ready`、`/tmp/umi_stereo_camera_control.json`、`/tmp/umi_stereo_camera_status.json`、`/tmp/umi_shutdown_request` |
| 关键子进程依赖 | `camera_recorder`、`sensor_recorder`、`audio/audio_play.py`、`audio/record_usb_audio.py` |
| 被谁拉起 | 当前由 `run_record.sh` 拉起；安装后由 `ugripper.service` 触发 |
| 当前安装后位置 | 新优先 `/opt/ugripper/bin/UgripperRuntime/UgripperRuntime`；兼容入口 `/opt/ugripper/build/src/record_runtime/record_runtime` |
| 相关 service | `ugripper.service`；间接关联 `umi-shutdown-trigger.path/service`；与 calibration/update 脚本共享 HMI/audio/lock 路径约束 |
| 建议一起迁入的内容 | app 代码、runtime process/domain 边界、`AudioCommandPort` / `StereoSessionPort` / `ShutdownRequestPort` 接口与默认 file backend |
| 明确不一起迁入的内容 | `run_record.sh` 路径切换、service 切换、打包/安装切换、`Updater / Calibration` app 化 |
| deploy/script 保留项 | `run_record.sh`、`auto_update/trigger_shutdown.sh`、`umi-shutdown-trigger.path/service`、`usb_auto_update.sh`、`run_calibration.sh` |
| 主风险 | 它是 supervisor 主控，迁移时同时牵动 HMI、audio、stereo、shutdown、脚本路径和 service 链路；任何安装路径错误都会放大成主链故障 |
| 建议测试门禁 | 主仓库本地编译 -> runtime host-only 单测 -> `--help` / 最小无硬件启动 smoke -> Docker ARM -> ARM `L2`，涉及 service/安装/打包时直接 `L3` |

必须在主仓库阶段继续验证的内容：

- `ugripper.service -> run_record.sh -> UgripperRuntime` 的真实启动链
- `umi-shutdown-trigger.path/service` 与 `ShutdownRequestPort` 的安装后联动
- `audio/audio_play.py` 与 `AudioCommandPort` 的安装后路径
- `StereoSessionPort` 与 `camera_recorder --stereo-daemon` 的真实路径

source:

- `src/record_runtime/CMakeLists.txt`
- `src/record_runtime/include/record_runtime.h`
- `docs/archive/2026-refactor-history/baseline/runtime-entrypoints.md`
- `docs/archive/2026-refactor-history/baseline/runtime-paths.md`
- `docs/archive/2026-refactor-history/baseline/service-map.md`
- `docs/archive/2026-refactor-history/baseline/package-contents.md`

## 6. 暂不进入 standalone app 的内容

这部分仍然视为 `B3` 范围，不纳入第一批 app 成功标准：

| 对象 | 当前建议归属 | 原因 |
| --- | --- | --- |
| `auto_update/usb_auto_update.sh` | `deploy/update` | 强绑定 USB 升级、安装环境和现场 SOP |
| `auto_calibration/run_calibration.sh` | `deploy/calibration` | 强绑定 zeroing、HMI helper、audio helper 和 root 运行链路 |
| `auto_calibration/import_camera_calibration.sh` | `deploy/calibration` | 强绑定持久化标定导入与设备环境 |
| `auto_update/trigger_shutdown.sh` | `deploy/systemd` | 直接服务于 `umi-shutdown-trigger.service` |
| `umi-shutdown-trigger.path/service` | `deploy/systemd` | 当前仍属于安装后 systemd 交付物 |

## 7. B1 完成口径

当前 `B1` 完成，至少应满足：

- 4 个对象的并仓输入已成文
- 已明确哪些内容属于 app，哪些属于 deploy/script
- 已明确每个模块的主仓库本地编译、Docker ARM、ARM 板验证门禁
- 已明确 `GripperHmiTool` 需要在真正迁移前再确认“独立 app 还是库 + helper”

后续可直接进入：

- `B2.1 CameraRecorder`
