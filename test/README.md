# Test Utilities

`test/scripts/` 用于收纳仓库侧的现场验证脚本，方便在开发机或问题机器上直接做定向检查。

这些脚本当前不属于 `ugripper.service` 主链路，也没有进入默认 `deb` 打包清单；它们更适合作为临时验证、回归或故障抓取工具使用。

## 当前脚本
- `test/scripts/camera_test.sh`
  - 直接拉起多路 `ffmpeg`，验证非主相机录制稳定性，并收集 `vmstat`、`top`、`ps` 等运行态信息。
- `test/scripts/camera_crash_capture.sh`
  - 在运行 `camera_test.sh` 前后补充系统快照，并持续抓取 `dmesg`、`journalctl -k`、中断、内存和进程信息，便于定位相机崩溃或卡死。
- `test/scripts/testVideoPipe.sh`
  - 枚举指定 `/dev/video*` 节点的驱动、格式、分辨率和帧率能力，用于快速核对视频输入能力。

## 建议用法
在仓库根目录执行：

```bash
bash test/scripts/camera_test.sh
bash test/scripts/camera_crash_capture.sh
bash test/scripts/testVideoPipe.sh
```

可按需通过环境变量覆盖输出目录和测试参数，例如：

```bash
BASE_DIR=/tmp/ugripper_cam_check DURATION=30 CAMERA_SET=non_main_merge_tactile \
  bash test/scripts/camera_crash_capture.sh
```

## 注意事项
- 两个相机脚本默认把结果写到当前工作目录下新建的时间戳目录；若不希望在仓库根目录落盘，请显式设置 `BASE_DIR`。
- `camera_crash_capture.sh` 在开启内核日志抓取时依赖 `sudo -n dmesg` 与 `sudo -n journalctl -k`；无免密 sudo 时，对应日志会失败或为空。
- `testVideoPipe.sh` 依赖 `v4l2-ctl`。
