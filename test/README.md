# Test Layout

`ugripper` 当前测试入口分成 3 层：

- `test/src/*`
  - 正式 host-only 单元测试目录。
  - 默认使用 `GoogleTest`。
  - 只放无硬件依赖、可稳定回归的模块测试。
  - 当前已覆盖 `utils`、`gripper_hmi`、`sensor`、`camera`、`record_runtime` 五类模块。
  - `record_runtime` 当前已覆盖 process boundary、button state machine、health monitor 和 recording orchestrator 这四类控制面逻辑。
- `src/utils/src/utils_smoke_test.cc`
  - 仍保留为最小 smoke 程序。
  - 它用于快速验证 `utils` 基础接线，不替代正式单元测试。
- `test/src/gripper_hmi/`
  - 当前更接近串口调试/工具程序。
  - 它不属于正式 host-only 单元测试集合。
- `test/scripts/*`
  - 现场验证或 field test 脚本。
  - 这些脚本不进入正式 host-only 单测集合。
- `run_record.sh` / `auto_update/usb_auto_update.sh` / `auto_calibration/run_calibration.sh` / `audio/*.py`
  - 这些属于主链 shell/python 编排层，不属于 `test/src/*` 正式单元测试集合。
  - 当前默认门禁是语法检查、定向 `--help` / `py_compile` 和文档化入口，不在仓库阶段要求硬件级回归。

## Field Scripts

`test/scripts/` 用于收纳仓库侧的现场验证脚本，方便在开发机或问题机器上直接做定向检查。

这些脚本当前不属于 `ugripper.service` 主链路，也没有进入默认 `deb` 打包清单；它们更适合作为临时验证、回归或故障抓取工具使用。

## 当前脚本
- `test/scripts/camera_test.sh`
  - 直接拉起多路 `ffmpeg`，验证非主相机录制稳定性，并收集 `vmstat`、`top`、`ps` 等运行态信息。
- `test/scripts/camera_crash_capture.sh`
  - 在运行 `camera_test.sh` 前后补充系统快照，并持续抓取 `dmesg`、`journalctl -k`、中断、内存和进程信息，便于定位相机崩溃或卡死。
- `test/scripts/testVideoPipe.sh`
  - 枚举指定 `/dev/video*` 节点的驱动、格式、分辨率和帧率能力，用于快速核对视频输入能力。
- `test/scripts/scan_main_camera_mkv_issues.py`
  - 递归扫描一个或多个目录中的 `cam_left.mkv` / `cam_right.mkv`，并发检查包时间戳异常、显著时间洞和可疑解码报错。
  - 默认先做快速 `ffprobe` 包级扫描，只对可疑文件追加 `ffmpeg` 解码扫描；适合批量数据排查。
  - 会按同目录左右主摄对齐“大时间洞”事件，便于判断是否存在左右同时异常。

## 建议用法
在仓库根目录执行：

```bash
bash test/scripts/camera_test.sh
bash test/scripts/camera_crash_capture.sh
bash test/scripts/testVideoPipe.sh
bash -n run_record.sh auto_update/usb_auto_update.sh auto_calibration/run_calibration.sh
python3 -m py_compile audio/*.py
python3 audio/record_usb_audio.py --help
```

可按需通过环境变量覆盖输出目录和测试参数，例如：

```bash
BASE_DIR=/tmp/ugripper_cam_check DURATION=30 CAMERA_SET=non_main_merge_tactile \
  bash test/scripts/camera_crash_capture.sh
```

批量扫描主摄 `mkv` 时，推荐直接给目录：

```bash
python3 test/scripts/scan_main_camera_mkv_issues.py /mnt/data_disk
python3 test/scripts/scan_main_camera_mkv_issues.py --root /mnt/data_disk --root ./tmp --jobs 12
python3 test/scripts/scan_main_camera_mkv_issues.py /mnt/data_disk --decode-mode full --json
```

常用参数：

- `--jobs N`
  - 控制并发度；数据量大时可按机器核数调高。
- `--decode-mode auto|full|off`
  - `auto` 先快扫再只深扫可疑文件；`full` 对全部文件做完整解码扫描；`off` 只做包级时间戳扫描。
- `--align-window-sec SEC`
  - 左右主摄异常时间点的对齐容差，默认 `0.25s`。
- `--fail-on-issue`
  - 发现可疑文件时返回非零退出码，方便接入批处理脚本。
- `--json`
  - 输出结构化 JSON 结果，便于后续汇总。

## 注意事项
- 两个相机脚本默认把结果写到当前工作目录下新建的时间戳目录；若不希望在仓库根目录落盘，请显式设置 `BASE_DIR`。
- `camera_crash_capture.sh` 在开启内核日志抓取时依赖 `sudo -n dmesg` 与 `sudo -n journalctl -k`；无免密 sudo 时，对应日志会失败或为空。
- `testVideoPipe.sh` 依赖 `v4l2-ctl`。
- `scan_main_camera_mkv_issues.py` 依赖 `ffprobe`；当 `--decode-mode` 不是 `off` 时还依赖 `ffmpeg`。
