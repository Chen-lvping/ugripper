# Runtime Entrypoints Baseline

本文档冻结 `ugripper` 当前运行入口事实。
目标是回答两件事：

- 当前主链和辅助链路分别从哪里进入
- 后续重构时，哪些入口必须保行为等价

## 当前本地状态

- 当前工作区已完成一次本地 Linux 编译，已生成：
  - `build/src/record_runtime/record_runtime`
  - `build/src/camera_recorder/camera_recorder`
  - `build/src/sensor_recorder/sensor_recorder`
  - `build/src/sensor_recorder/zeroing`
- 本地环境未安装系统级 `libserialport-dev` / `nlohmann-json3-dev`
- 当前构建依赖工作区内的本地解包依赖目录：
  - `.local-deps/libserialport`
  - `.local-deps/nlohmann`
- 因此：
  - `camera_recorder` 可直接本地运行最小命令
  - `record_runtime`、`sensor_recorder`、`zeroing` 本地运行时需补 `LD_LIBRARY_PATH=$PWD/.local-deps/libserialport/root/usr/lib/x86_64-linux-gnu`

## 主链启动链

1. `systemd` 启动 `ugripper.service`
2. `ugripper.service` 执行 `/opt/ugripper/run_record.sh`
3. `run_record.sh` 执行 `./bin/UgripperRuntime/UgripperRuntime`
4. `record_runtime` 再拉起：
   - `./bin/CameraRecorder/CameraRecorder`
   - `./bin/SensorRecorder/SensorRecorder`
   - `./bin/UgripperRuntime/audio/audio_play.py`
   - `./bin/UgripperRuntime/audio/record_usb_audio.py`

## 入口清单

| 入口 | 当前命令/路径 | 触发方式 | source | 当前本地状态 |
| --- | --- | --- | --- | --- |
| 主服务入口 | `systemctl start ugripper.service` | 安装后 `postinst` 启动并 enable | `pack_script/ugripper.service`, `pack_script/postinst` | 本地未安装到 systemd，不执行 |
| 主运行脚本 | `./run_record.sh [record_runtime args...]` | `ugripper.service` / 手工执行 | `run_record.sh` | 已实际执行；会先等待 `/mnt/data_disk` 可写，再进入 runtime |
| runtime 主程序 | `./bin/UgripperRuntime/UgripperRuntime --help` | `run_record.sh` 或手工执行 | `src/record_runtime/src/main.cpp`, `src/record_runtime/include/record_runtime.h` | 安装态使用 `bin/` 入口；开发态仍可手工执行 `build/src` 二进制做局部验证 |
| camera 录制程序 | `./bin/CameraRecorder/CameraRecorder --output-dir DIR --dry-run [--config-yaml PATH]` | `record_runtime` 或手工执行 | `src/camera_recorder/src/main.cpp`, `src/camera_recorder/include/camera_recorder/camera_recorder.h` | 安装态使用 `bin/` 入口；开发态仍可手工执行 `build/src` 二进制做局部验证 |
| sensor 录制程序 | `./bin/SensorRecorder/SensorRecorder [OUTPUT_DIR]` | `record_runtime` 拉起 | `src/record_runtime/include/record_runtime.h`, `src/sensor_recorder/CMakeLists.txt`, `src/sensor_recorder/src/main.cpp` | 安装态使用 `bin/` 入口；开发态仍可手工执行 `build/src` 二进制做局部验证 |
| 编码器校准程序 | `./bin/SensorRecorder/zeroing <left\|right\|/dev/encoder_path>` | `run_calibration.sh` | `src/sensor_recorder/src/zeroing.cpp`, `auto_calibration/run_calibration.sh` | 安装态使用 `bin/` 入口；开发态仍可手工执行 `build/src` 二进制做局部验证 |
| 校准脚本 | `/opt/ugripper/auto_calibration/run_calibration.sh` | `ugripper-calibration.service` 或手工 root 执行 | `auto_calibration/ugripper-calibration.service`, `auto_calibration/run_calibration.sh` | 未执行；已做语法检查 |
| 标定导入脚本 | `/opt/ugripper/auto_calibration/import_camera_calibration.sh <USB_ROOT>` | `usb_auto_update.sh` 内部调用或手工执行 | `auto_calibration/import_camera_calibration.sh`, `auto_update/usb_auto_update.sh` | 未执行；已做语法检查 |
| USB 升级脚本 | `/usr/local/bin/usb_auto_update.sh /dev/%I` | `usb-auto-update@.service` | `auto_update/usb-auto-update@.service`, `auto_update/usb_auto_update.sh`, `usb_updater_build.sh` | 由可选独立包 `ugripper-usb-updater` 提供，不属于主 `ugripper` 包安装主线 |
| 关机触发脚本 | `/opt/ugripper/auto_update/trigger_shutdown.sh` | `umi-shutdown-trigger.path` 监听 `/tmp/umi_shutdown_request` | `auto_update/umi-shutdown-trigger.service`, `auto_update/umi-shutdown-trigger.path`, `auto_update/trigger_shutdown.sh` | 未执行 |
| 音频播放守护脚本 | `python3 bin/UgripperRuntime/audio/audio_play.py` | `record_runtime` / `run_calibration.sh` 依赖其 FIFO 协议 | `audio/audio_play.py`, `src/record_runtime/include/record_runtime.h`, `auto_calibration/run_calibration.sh` | 安装态使用 `bin/` 入口 |
| USB 音频录制脚本 | `python3 bin/UgripperRuntime/audio/record_usb_audio.py --output /tmp/test.wav --seconds 0.5` | `record_runtime` | `audio/record_usb_audio.py`, `src/record_runtime/include/record_runtime.h` | 安装态使用 `bin/` 入口 |

## 当前最小可运行基线

以下命令在后续每次重构后都可作为最小对照物：

| 命令 | 目的 | source | 当前结果 |
| --- | --- | --- | --- |
| `./run_record.sh --help` | 验证主运行脚本入口是否仍指向 `record_runtime` | `run_record.sh` | 已执行；当前行为是先等待 `/mnt/data_disk` 可写，不直接透传 `--help` |
| `python3 audio/record_usb_audio.py --help` | 验证音频录制脚本 CLI 仍可工作 | `audio/record_usb_audio.py` | 已执行；返回 usage |
| `LD_LIBRARY_PATH=$PWD/.local-deps/libserialport/root/usr/lib/x86_64-linux-gnu ./build/src/record_runtime/record_runtime --help` | 验证 runtime CLI | `src/record_runtime/src/main.cpp` | 已执行；返回 usage |
| `./build/src/camera_recorder/camera_recorder --help` | 验证 camera CLI | `src/camera_recorder/src/main.cpp` | 已执行；返回 usage |
| `./build/src/camera_recorder/camera_recorder --output-dir /tmp/ugripper_cam_dryrun --dry-run` | 验证 camera 无硬件 dry-run 路径 | `src/camera_recorder/src/main.cpp` | 已执行；打印 missing devices 列表并退出 |
| `LD_LIBRARY_PATH=$PWD/.local-deps/libserialport/root/usr/lib/x86_64-linux-gnu timeout 2s ./build/src/sensor_recorder/sensor_recorder` | 验证 sensor 最小启动路径 | `src/sensor_recorder/src/main.cpp` | 已执行；打印输出文件路径并报 `/dev/right_imu`、`/dev/left_imu` 缺失 |
| `LD_LIBRARY_PATH=$PWD/.local-deps/libserialport/root/usr/lib/x86_64-linux-gnu ./build/src/sensor_recorder/zeroing` | 验证 zeroing usage | `src/sensor_recorder/src/zeroing.cpp` | 已执行；返回 usage |
| `bash -n run_record.sh auto_update/usb_auto_update.sh auto_calibration/run_calibration.sh auto_calibration/import_camera_calibration.sh` | 验证关键 shell 入口语法 | 各脚本文件 | 已通过 |
| `python3 -m py_compile audio/*.py auto_calibration/generate_gripper_calibration_bin.py` | 验证关键 Python 文件语法 | 各 Python 文件 | 已通过 |

## 备注

- `record_runtime` 当前默认子进程与音频资源路径已固定为 `bin/...` 新布局。
- `sensor_recorder` 当前仍未提供 `--help`，后续若重构其入口，建议补明确 CLI usage
- `camera_recorder` 的 `--dry-run` 已是当前较适合的无硬件基线命令，应保留
