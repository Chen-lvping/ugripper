# Equivalence Checklist Baseline

本文档定义 `A1` 阶段冻结下来的行为等价检查清单。
它分两层：

- `目标检查项`：后续重构必须保住的行为
- `当前本地证据`：在当前工作区实际执行或静态核对得到的结果

## 1. 当前本地环境说明

- 当前工作区已完成一次本地 Linux 编译
- 由于系统未安装 `libserialport-dev` / `nlohmann-json3-dev`，本地编译使用了工作区内解包依赖：
  - `.local-deps/libserialport`
  - `.local-deps/nlohmann`
- 因此涉及 `libserialport` 的二进制本地运行时需补：
  - `LD_LIBRARY_PATH=$PWD/.local-deps/libserialport/root/usr/lib/x86_64-linux-gnu`
- 当前 `A1` 已能完成：
  - 入口事实冻结
  - shell 语法检查
  - Python 语法检查
  - 本地二进制最小 CLI / dry-run / timeout 探测

## 2. 行为等价检查清单

| 检查项 | 目标命令/证据 | source | 当前本地状态 |
| --- | --- | --- | --- |
| 正常启动主服务 | `systemctl start ugripper.service` 最终应拉起 `/opt/ugripper/run_record.sh` | `pack_script/ugripper.service`, `pack_script/postinst` | 未执行；当前仅完成静态核对 |
| 正常启动 runtime | `LD_LIBRARY_PATH=$PWD/.local-deps/libserialport/root/usr/lib/x86_64-linux-gnu ./build/src/record_runtime/record_runtime --help` 或通过 `run_record.sh` 间接拉起 | `run_record.sh`, `src/record_runtime/src/main.cpp` | `pass`：usage 正常 |
| 正常开始录制 | runtime 能拉起 `camera_recorder`、`sensor_recorder`、音频脚本 | `src/record_runtime/include/record_runtime.h` | `not fully verified`：只完成最小 CLI 探测，未做真实录制 |
| 正常停止录制 | runtime 停止 camera / sensor / audio 子流程 | `src/record_runtime/src/record_runtime.cpp` | `not yet verified`：未做真实录制会话 |
| 音频播放正常 | `audio/audio_play.py` 能启动并处理 FIFO 指令 | `audio/audio_play.py` | `static-only`：语法已通过，未做运行验证 |
| 相机录制正常 | `camera_recorder --output-dir DIR --dry-run` 至少可完成无硬件命令生成 | `src/camera_recorder/src/main.cpp`, `src/camera_recorder/src/camera_recorder.cpp` | `partial-pass`：CLI 正常；当前因缺少 `/dev/*camera*` 设备返回 missing devices |
| 传感器录制正常 | `sensor_recorder` 能被 runtime 拉起并写出数据 | `src/sensor_recorder/CMakeLists.txt`, `src/record_runtime/include/record_runtime.h` | `partial-pass`：最小启动可运行；当前因缺少 `/dev/right_imu`、`/dev/left_imu` 失败 |
| 升级脚本能执行 | `usb_auto_update.sh /dev/<node>` 语法正确，配置导入逻辑可静态核对 | `auto_update/usb_auto_update.sh` | `pass`：`bash -n` 已通过；未做真实升级 |
| 校准脚本能执行 | `run_calibration.sh` 语法正确，zeroing / 音频 / service 链路可静态核对 | `auto_calibration/run_calibration.sh` | `pass`：`bash -n` 已通过；未做真实校准 |
| 标定导入脚本能执行 | `import_camera_calibration.sh <USB_ROOT>` 语法正确，导入逻辑可静态核对 | `auto_calibration/import_camera_calibration.sh` | `pass`：`bash -n` 已通过；未做真实导入 |

## 3. 当前已执行的本地验证

### 3.1 Shell 语法检查

已通过：

- `run_record.sh`
- `auto_update/usb_auto_update.sh`
- `auto_calibration/run_calibration.sh`
- `auto_calibration/import_camera_calibration.sh`
- `pack_script/postinst`
- `pack_script/prerm`
- `pack_script/postrm`

### 3.2 Python 语法检查

已通过：

- `audio/*.py`
- `auto_calibration/generate_gripper_calibration_bin.py`

### 3.3 最小命令探测

| 命令 | 结果 |
| --- | --- |
| `./run_record.sh --help` | 失败，报错 `record_runtime binary not found: ./build/src/record_runtime/record_runtime` |
| `python3 audio/record_usb_audio.py --help` | 成功，返回 argparse usage |
| `LD_LIBRARY_PATH=$PWD/.local-deps/libserialport/root/usr/lib/x86_64-linux-gnu ./build/src/record_runtime/record_runtime --help` | 成功，返回 usage |
| `./build/src/camera_recorder/camera_recorder --help` | 成功，返回 usage |
| `./build/src/camera_recorder/camera_recorder --output-dir /tmp/ugripper_cam_dryrun --dry-run` | 执行成功到设备探测阶段，返回 missing devices |
| `LD_LIBRARY_PATH=$PWD/.local-deps/libserialport/root/usr/lib/x86_64-linux-gnu timeout 2s ./build/src/sensor_recorder/sensor_recorder` | 启动成功到设备打开阶段，报 `/dev/right_imu`、`/dev/left_imu` 缺失 |
| `LD_LIBRARY_PATH=$PWD/.local-deps/libserialport/root/usr/lib/x86_64-linux-gnu ./build/src/sensor_recorder/zeroing` | 成功，返回 usage |

## 4. 当前不能在本地 Linux 完成的验证

以下检查仍后移到后续阶段：

- 主仓库本地编译验证
- Docker ARM 交叉编译
- ARM 板 smoke
- 安装后真实路径验证
- `systemd` 安装后真实启动验证
- 真正的录制、升级、校准、音频硬件验证

## 5. 后续使用方式

后续每次重构完成后，至少回答以下问题：

1. 本次改动影响了上表中的哪些检查项
2. 哪些检查项已在本地 Linux 重新验证
3. 哪些检查项仍需后移到主仓库 / Docker / ARM 阶段
