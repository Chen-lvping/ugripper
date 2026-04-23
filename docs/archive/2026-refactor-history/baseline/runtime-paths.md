# Runtime Paths Baseline

本文档冻结 `ugripper` 当前运行时关键路径。
后续路径治理阶段必须以此为对照，先做归类，再做封装替换。

## 0. 仓库内路径真源分类

在开始代码级路径替换前，先冻结“哪些目录是当前真源，哪些不是”的口径。

| 路径 | 身份 | 是否当前真源 | 备注 |
| --- | --- | --- | --- |
| `src/` | 实现真源 | 是 | C++ 主模块和公共库的唯一代码真源 |
| `config/` | 配置真源 | 是 | 当前 YAML / JSON 配置与标定基线真源 |
| `docs/` | 文档真源 | 是 | 计划、baseline、变更口径真源 |
| `test/` | 测试真源 | 是 | `test/src` 为正式 host-only 单测；`test/scripts` 为 field test |
| `audio/`、`audio_en/` | 资源真源 | 是 | 当前运行时音频资源和音频脚本真源，Phase 1 不做物理迁移 |
| `auto_update/`、`auto_calibration/`、`pack_script/` | 交付真源 | 是 | 当前部署、升级、校准、service 与安装脚本真源 |
| `py_script/` | 工具真源 | 是，但非产品主链 | 仓内工具资产真源；默认不作为主链实现真源 |
| `build/` | 构建产物 | 否 | 生成目录；允许作为“当前二进制位置事实”，不允许作为重构改动真源 |
| `tmp/` | 运行/临时产物 | 否 | 临时目录，不作为配置或实现真源 |
| `.local-deps/` | 本地构建辅助目录 | 否 | 本地 Linux 验证所用依赖缓存，不进主链、不进打包 |
| `ugripper/` | 历史副本冻结候选 | 否 | 当前工作树中的嵌套旧仓副本；后续冻结、断引用、再决定归档或删除 |

执行约束：

- 后续重构优先只改真源目录。
- `build/`、`tmp/`、`.local-deps/` 只能作为事实来源或本地验证依赖，不能被当作实现真源。
- `ugripper/` 不再继续叠加主链逻辑。

## 1. 二进制路径

| 路径 | 用途 | source | 备注 |
| --- | --- | --- | --- |
| `./bin/UgripperRuntime/UgripperRuntime` | 主 runtime 二进制入口 | `run_record.sh` | `Stage B` 当前默认运行入口 |
| `./bin/CameraRecorder/CameraRecorder` | camera 录制二进制入口 | `src/record_runtime/include/record_runtime.h` | `Stage B` 当前默认运行入口 |
| `./bin/SensorRecorder/SensorRecorder` | 传感器录制二进制入口 | `src/record_runtime/include/record_runtime.h` | `Stage B` 当前默认运行入口 |
| `./bin/SensorRecorder/zeroing` | 编码器 zeroing 入口 | `auto_calibration/run_calibration.sh` | `Stage B` 当前默认运行入口 |
| `./bin/GripperHmiTool/GripperHmiTool` | HMI helper 入口 | `auto_calibration/run_calibration.sh`, `auto_update/usb_auto_update.sh`, `auto_calibration/import_camera_calibration.sh` | `Stage B` 当前默认运行入口 |
| `./build/src/record_runtime/record_runtime` | 主 runtime 二进制 | `run_record.sh` | 当前可能在本地工作区生成；属于 `build/` 产物，不是真源 |
| `./build/src/camera_recorder/camera_recorder` | camera 录制二进制 | `src/record_runtime/include/record_runtime.h` | 当前可能在本地工作区生成；属于 `build/` 产物，不是真源 |
| `./build/src/sensor_recorder/sensor_recorder` | 传感器录制二进制 | `src/record_runtime/include/record_runtime.h` | 当前可能在本地工作区生成；属于 `build/` 产物，不是真源 |
| `./build/src/sensor_recorder/zeroing` | 编码器 zeroing 二进制 | `auto_calibration/run_calibration.sh` | 当前可能在本地工作区生成；属于 `build/` 产物，不是真源 |
| `./build/src/gripper_hmi/gripper_hmi_test` | HMI helper | `auto_calibration/run_calibration.sh`, `auto_update/usb_auto_update.sh` | 当前可能在本地工作区生成；安装后路径为 `/opt/ugripper/build/src/gripper_hmi/gripper_hmi_test` |

## 2. 脚本路径

| 路径 | 用途 | source |
| --- | --- | --- |
| `./run_record.sh` | 主服务脚本入口 | `pack_script/ugripper.service` |
| `./bin/UgripperRuntime/audio/audio_play.py` | 音频播放守护脚本新优先入口 | `src/record_runtime/include/record_runtime.h`, `auto_calibration/run_calibration.sh` |
| `./bin/UgripperRuntime/audio/record_usb_audio.py` | USB 音频录制脚本新优先入口 | `src/record_runtime/include/record_runtime.h` |
| `./auto_update/usb_auto_update.sh` | USB 升级主脚本 | `auto_update/usb-auto-update@.service` |
| `./auto_calibration/run_calibration.sh` | 校准执行脚本 | `auto_calibration/ugripper-calibration.service` |
| `./auto_calibration/import_camera_calibration.sh` | 标定导入脚本 | `auto_update/usb_auto_update.sh` |
| `./auto_update/trigger_shutdown.sh` | 关机触发处理脚本 | `auto_update/umi-shutdown-trigger.service` |
| `./auto_calibration/monitor_network.sh` | 网线监测脚本 | `auto_calibration/ugripper-network-monitor.service` |

## 3. 配置与资源路径

| 路径 | 用途 | source |
| --- | --- | --- |
| `config/camera_recorder.yaml` | camera 配置入口 | `src/camera_recorder/include/camera_recorder/camera_recorder.h` |
| `bin/UgripperRuntime/config/fakeCamCalib.json` | fallback 相机标定 | `src/record_runtime/include/record_runtime.h`, `pack_script/postinst`, `auto_calibration/import_camera_calibration.sh` |
| `config/fakeEncoderCalib.json` | 仓库内标定资源 | 仓库文件存在；当前未发现主链读取点 |
| `config/fakeIMUCalib.json` | 仓库内标定资源 | 仓库文件存在；当前未发现主链读取点 |
| `bin/UgripperRuntime/audio/` | 默认中文音频资源目录 | `audio/audio_play.py`, `src/record_runtime/include/record_runtime.h`, `auto_update/usb_auto_update.sh` |
| `bin/UgripperRuntime/audio_en/` | 英文音频资源目录 | `audio/audio_play.py`, `auto_update/usb_auto_update.sh` |
| `bin/UgripperRuntime/audio/noise.prof` | 音频降噪 profile | `src/record_runtime/include/record_runtime.h` |

## 4. 环境与持久化路径

| 路径 | 用途 | source |
| --- | --- | --- |
| `/etc/environment` | 环境配置入口，保存 `DEVICE_SN` / `UGRIPPER_LANG` / `CAMERA_CODEC` | `run_record.sh`, `src/record_runtime/src/record_runtime.cpp`, `src/camera_recorder/src/camera_recorder.cpp`, `auto_update/usb_auto_update.sh`, `audio/audio_play.py`, `auto_calibration/import_camera_calibration.sh` |
| `/etc/ugripper/config/calibration/calibration.json` | 持久化相机标定 | `src/record_runtime/include/record_runtime.h`, `pack_script/postinst`, `auto_calibration/import_camera_calibration.sh` |
| `/etc/ugripper/config/calibration/imported/<DEVICE_SN>/<STAMP>` | 导入标定留档 | `auto_calibration/import_camera_calibration.sh` |

## 5. 运行时临时路径

| 路径 | 用途 | source |
| --- | --- | --- |
| `/tmp/umi_audio_pipe` | 音频命令 FIFO | `src/record_runtime/include/record_runtime.h`, `audio/audio_play.py`, `auto_calibration/run_calibration.sh`, `pack_script/postinst`, `pack_script/prerm` |
| `/tmp/umi_audio_ready` | 音频后端 ready 标记 | `src/record_runtime/include/record_runtime.h`, `audio/audio_play.py` |
| `/tmp/umi_audio` | 音频临时目录 | `src/record_runtime/include/record_runtime.h` |
| `/tmp/umi_shutdown_request` | 关机请求文件 | `src/record_runtime/include/record_runtime.h`, `auto_update/umi-shutdown-trigger.path`, `pack_script/postinst` |
| `/tmp/umi_recording.lock` | 录制状态锁文件 | `pack_script/postinst`, `pack_script/prerm`, `auto_update/usb_auto_update.sh` |
| `/tmp/umi_stereo_camera_control.json` | stereo 控制文件 | `src/record_runtime/include/record_runtime.h`, `src/camera_recorder/include/camera_recorder/camera_recorder.h` |
| `/tmp/umi_stereo_camera_status.json` | stereo 状态文件 | `src/record_runtime/include/record_runtime.h`, `src/camera_recorder/include/camera_recorder/camera_recorder.h` |
| `/tmp/umi_sys_<device_sn>_<date>.log` | 本地 runtime 日志 | `run_record.sh` |
| `/tmp/umi_sys_<device_sn>_<date>.pos` | 日志增量同步位置 | `run_record.sh` |
| `/tmp/umi_sys_<device_sn>_<date>.lock` | 日志同步锁 | `run_record.sh` |

## 6. 数据盘与日志路径

| 路径 | 用途 | source |
| --- | --- | --- |
| `/mnt/data_disk` | 主数据盘挂载点 | `run_record.sh`, `src/record_runtime/include/record_runtime.h`, `pack_script/postinst`, `auto_update/usb_auto_update.sh`, `auto_calibration/run_calibration.sh` |
| `/mnt/data_disk/logs` | 同步后的 runtime 日志目录 | `run_record.sh` |
| `/var/log/ugripper/usb_auto_update.log` | USB 升级日志 | `auto_update/usb_auto_update.sh` |

## 7. 设备节点路径

| 路径 | 用途 | source |
| --- | --- | --- |
| `/dev/right_gripper` | 右夹爪/HMI 端口 | `src/record_runtime/src/main.cpp`, `auto_calibration/run_calibration.sh`, `auto_update/usb_auto_update.sh` |
| `/dev/left_gripper` | 左夹爪/HMI 端口 | `src/record_runtime/src/main.cpp`, `auto_calibration/run_calibration.sh`, `auto_update/usb_auto_update.sh` |
| `/dev/left_cam_main` 等 camera 设备 | camera 采集设备 | `config/camera_recorder.yaml` |
| `/dev/input/ugripper_usb_audio_keys` | USB 音频按键输入 | `audio/audio_play.py` |

## 8. 安装后路径口径

当前打包脚本以 `/opt/ugripper` 为安装根目录。

| 安装后路径 | 当前来源 | source |
| --- | --- | --- |
| `/opt/ugripper/run_record.sh` | 仓库根目录 `run_record.sh` | `pack_script/ugripper.service`, `build_deb.sh` |
| `/opt/ugripper/bin/UgripperRuntime/*` | standalone 安装树 / package staging | `pp_main/package_ugripper_stage.sh` |
| `/opt/ugripper/bin/CameraRecorder/*` | standalone 安装树 / package staging | `pp_main/package_ugripper_stage.sh` |
| `/opt/ugripper/bin/SensorRecorder/*` | standalone 安装树 / package staging | `pp_main/package_ugripper_stage.sh` |
| `/opt/ugripper/bin/GripperHmiTool/*` | standalone 安装树 / package staging | `pp_main/package_ugripper_stage.sh` |
| `/opt/ugripper/auto_calibration/*` | 项目树 rsync | `build_deb.sh` |
| `/opt/ugripper/auto_update/*` | 项目树 rsync | `build_deb.sh` |

## 备注

- 当前 `Stage B` 默认运行布局已经固定为 `bin/...` 新路径。
- `build/src/...` 与顶层 `audio...` / `config/...` 仍可能作为仓库真源或显式兼容打包开关输入存在，但不再属于默认运行入口。
- `/etc/environment`、`/etc/ugripper/config/calibration/calibration.json`、`/mnt/data_disk` 属于高风险路径，后续不能按普通字符串替换处理
