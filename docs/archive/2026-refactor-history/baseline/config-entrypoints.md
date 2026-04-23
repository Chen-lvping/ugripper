# Config Entrypoints Baseline

本文档冻结当前配置入口，包括：

- 环境变量入口
- 仓库内配置文件入口
- 持久化配置入口
- USB 外部导入配置入口

## 1. 环境变量入口：`/etc/environment`

| 键 | 当前用途 | 读取方 | 写入方 | source |
| --- | --- | --- | --- | --- |
| `DEVICE_SN` | 设备 SN；用于日志命名、标定导入目录选择、episode 元数据 | `run_record.sh`, `record_runtime`, `import_camera_calibration.sh` | 当前仓库内未见写入逻辑 | `run_record.sh`, `src/record_runtime/src/record_runtime.cpp`, `auto_calibration/import_camera_calibration.sh` |
| `UGRIPPER_LANG` | 音频语言选择 | `record_runtime`, `audio/audio_play.py`, `usb_auto_update.sh` | `usb_auto_update.sh` | `src/record_runtime/src/record_runtime.cpp`, `audio/audio_play.py`, `auto_update/usb_auto_update.sh` |
| `CAMERA_CODEC` | camera 编码器口径（`h264` / `h265`） | `record_runtime`, `camera_recorder`, `usb_auto_update.sh` | `usb_auto_update.sh` | `src/record_runtime/src/record_runtime.cpp`, `src/camera_recorder/src/camera_recorder.cpp`, `auto_update/usb_auto_update.sh` |

## 2. 仓库内配置文件入口

| 路径 | 当前用途 | 读取方 | source |
| --- | --- | --- | --- |
| `config/camera_recorder.yaml` | camera 列表、模式、设备名、输出文件、编码参数 | `camera_recorder` | `src/camera_recorder/include/camera_recorder/camera_recorder.h`, `src/camera_recorder/src/main.cpp` |
| `config/fakeCamCalib.json` | fallback 相机标定模板 | `record_runtime`, `postinst`, `import_camera_calibration.sh` | `src/record_runtime/include/record_runtime.h`, `pack_script/postinst`, `auto_calibration/import_camera_calibration.sh` |
| `config/99-fixed-usb-map.rules` | udev 固定 USB 规则 | 打包阶段复制到 `/etc/udev/rules.d/` | `build_deb.sh` |

## 3. 持久化配置入口

| 路径 | 当前用途 | 读取方/写入方 | source |
| --- | --- | --- | --- |
| `/etc/ugripper/config/calibration/calibration.json` | 当前持久化标定文件 | `record_runtime` 读取；`postinst` 初始化；`import_camera_calibration.sh` 更新 | `src/record_runtime/include/record_runtime.h`, `pack_script/postinst`, `auto_calibration/import_camera_calibration.sh` |
| `/etc/ugripper/config/calibration/imported/<DEVICE_SN>/<STAMP>` | 导入标定留档 | `import_camera_calibration.sh` 写入 | `auto_calibration/import_camera_calibration.sh` |

## 4. USB 导入侧配置入口

### 4.1 `config.txt`

`usb_auto_update.sh` 会从 USB 挂载根目录读取 `config.txt` 中的以下键：

| 支持键 | 作用 | 解析结果 | source |
| --- | --- | --- | --- |
| `LANGUAGE`, `VOICE_LANG`, `VOICE_LANGUAGE`, `LANG` | 语言配置 | 归一化为 `zh` / `en`，写入 `UGRIPPER_LANG` | `auto_update/usb_auto_update.sh` |
| `CAMERA_CODEC`, `VIDEO_CODEC`, `TRIPLE_CAMERA_CODEC`, `CODEC` | 编码器配置 | 归一化为 `h264` / `h265`，写入 `CAMERA_CODEC` | `auto_update/usb_auto_update.sh` |

### 4.2 `calibration.txt`

| 文件 | 用途 | 读取方 | source |
| --- | --- | --- | --- |
| `/mnt/data_disk/calibration.txt` | 触发校准脚本执行 | `auto_calibration/run_calibration.sh` | `auto_calibration/run_calibration.sh` |

### 4.3 标定导入目录

`import_camera_calibration.sh` 会基于以下信息定位导入源：

- USB 根目录参数 `<USB_ROOT>`
- `DEVICE_SN`
- 夹爪 SN
- camchain / calibration 相关文件

source: `auto_calibration/import_camera_calibration.sh`

## 5. 运行时控制文件入口

| 路径 | 用途 | 读取方/写入方 | source |
| --- | --- | --- | --- |
| `/tmp/umi_stereo_camera_control.json` | stereo 控制文件 | `record_runtime` / `camera_recorder` | `src/record_runtime/include/record_runtime.h`, `src/camera_recorder/include/camera_recorder/camera_recorder.h` |
| `/tmp/umi_stereo_camera_status.json` | stereo 状态文件 | `record_runtime` / `camera_recorder` | `src/record_runtime/include/record_runtime.h`, `src/camera_recorder/include/camera_recorder/camera_recorder.h` |
| `/tmp/umi_shutdown_request` | 关机触发文件 | `record_runtime` 写；`umi-shutdown-trigger.path` 监听 | `src/record_runtime/include/record_runtime.h`, `auto_update/umi-shutdown-trigger.path` |

## 6. 当前高风险配置入口

以下入口后续修改时必须优先做等价性核对：

- `/etc/environment`
- `config/camera_recorder.yaml`
- `/etc/ugripper/config/calibration/calibration.json`
- USB 侧 `config.txt`
- USB 侧 `calibration.txt`
