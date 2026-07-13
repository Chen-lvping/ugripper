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
- `test/src/gripper_disconnect_repro_sop/`
  - 夹爪掉线复现与日志采集包，可单独打包给测试同学使用。
  - 主机侧从该文件夹根目录执行 `./scripts/prepare_240_repro.sh`，脚本会通过 SSH 下发板端 worker、连续触发软件录制，并在本地实时刷新板端 `hws` 状态；报错时自动抓取现场快照并暂停等待恢复。
  - 系统日志和 `ugripper.service` 实时日志由测试同学按 README 指令另开终端手动观察。
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
- `test/scripts/board_gripper_hmi_link_stress.sh`
  - 停止 `ugripper.service` 后反复独占连接左右 HMI，执行 RGB、蜂鸣器开关和状态回读，统计合法回复、蜂鸣器状态确认、最大回复年龄与失败轮次。
  - 默认在整个压力窗口并行运行 `SensorRecorder`，持续记录左右 encoder，以区分 HMI 单 UART/控制板异常和整颗 CH9344 或 USB 上行链路异常。
  - 使用 `trap` 在正常结束、失败或信号退出时停止传感器探针并恢复原本运行中的服务；结果默认写入 `/dev/shm`，适合加密环境下中转下发和回收。
- `test/scripts/board_ch9344_interference_matrix.sh`
  - 针对 CH9344 多 UART 相互干扰做分组归因：依次执行 encoder 空载基线、HMI UART 原始 open/close、termios 重配置、`TCIOFLUSH`、`TIOCEXCL`、原始状态/RGB/蜂鸣器帧、完整 HMI 反复重连和单次长连接。
  - 每个阶段独立记录左右 encoder MCAP 和 gap 事件，将同侧 encoder 与异侧 encoder 对照，用于判断问题由端口打开、串口配置、flush、协议流量还是整颗 CH9344/USB 链路触发。
- `test/scripts/board_encoder_to_hmi_interference_matrix.sh`
  - 反向验证 encoder UART 是否会干扰同颗 CH9344 的 HMI：左右 HMI 保持长连接和周期状态查询，同时对指定 encoder UART 高频执行 open/close、termios、flush 和真实 1Mbps 位置请求。
  - 统计左右 HMI 最大合法回复年龄、inactive 样本和 I/O failure，以异侧 HMI 为对照判断 encoder 操作是否能够触发同侧 HMI 无响应。
- `test/scripts/board_hmi_self_timeout_soak.sh`
  - 左右 HMI 各只打开一次并在整个测试窗口保持连接，持续执行动态灯效、蜂鸣器开关确认和状态查询，同时由 `SensorRecorder` 连续记录左右 encoder。
  - 自动统计 HMI 最大合法回复年龄、超过 runtime `5500ms` 门限的 inactive 样本、I/O failure 和收发计数，并统计 encoder 的 `>2/5/10/20/100/1000ms` gap 数量；用于确认 HMI 是否会被自身指令流卡死，且避免反复 tty open/close 污染结论。
- `test/scripts/board_hmi_high_rate_soak.sh`
  - 两侧 HMI 全程各只打开一次，将状态查询从 `1Hz` 阶梯提升到 `50/100/250/500/800/1200Hz`，并叠加最高 `250Hz` RGB 与 `50Hz` 蜂鸣器脉冲；可通过 `--saturation-repeat` 延长接近串口带宽上限的最高档，最后回到 `1Hz` 观察是否恢复。
  - 原始串口监视会分别统计 RX 字节、合法 XOR 帧、非法 XOR、丢弃字节、残留半帧、最大合法回复年龄与 `5500ms` timeout 事件；双 encoder 同期持续录制，用于提高偶发 HMI 自卡死的触发概率并区分协议过载、解析异常和物理无回包。
- `test/scripts/board_hmi_strict_reply_soak.sh`
  - 不发送 RGB 或蜂鸣器控制，只执行 HMI 状态查询；每侧严格限制为单请求在途，收到对应的下一条状态帧或单轮超时后才允许继续发送。
  - 单轮超时后进入静默隔离窗口，将迟到帧单独计数，避免高频积压回复被错误匹配到下一轮；同步记录原始字节、合法帧、非法 XOR、单轮延迟分位数和双 encoder gap，用于验证 HMI 是否会偶发吞掉状态查询。
- `test/scripts/board_hmi_record_start_matrix.sh`
  - 按真实故障前左右灯效还原录制启动边界：左 HMI 为 `Ready`、右 HMI 为触觉告警，启动时两侧切换到 `Recording`；HMI 端口全程只打开一次，状态查询保持真实 `1Hz`，LED保持颜色变化或 `250ms` 重发策略，不发送蜂鸣器命令。
  - `combo` 在灯效切换边界同步启停 `SensorRecorder`；`timing` 扫描 encoder open/config 相对灯效切换的时间偏移；`led-only` 让 encoder 全程常开、仅重复灯效切换，以依次隔离组合竞争、CH9344并发时序和Recording灯效自身影响。

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
bash test/scripts/board_gripper_hmi_link_stress.sh --cycles 20 --side both
bash test/scripts/board_ch9344_interference_matrix.sh --side left --quick-repeats 1000 --tool-cycles 40
bash test/scripts/board_encoder_to_hmi_interference_matrix.sh --side left --quick-repeats 2000 --query-cycles 100
bash test/scripts/board_hmi_self_timeout_soak.sh --duration-sec 600
bash test/scripts/board_hmi_high_rate_soak.sh --stage-sec 60 --saturation-repeat 10
bash test/scripts/board_hmi_strict_reply_soak.sh --baseline-sec 60 --stress-sec 300
bash test/scripts/board_hmi_record_start_matrix.sh --mode combo --cycles 50
```

HMI 链路压力测试会直接占用夹爪和 encoder 串口，必须以 root 权限执行。现场推荐先将仓库脚本和 `board_test_common.sh` 复制到 `/dev/shm/ugripper_hmi_test/`，校验 SHA-256 后执行：

```bash
sudo bash /dev/shm/ugripper_hmi_test/board_gripper_hmi_link_stress.sh \
  --cycles 20 \
  --side both \
  --output-dir /dev/shm/ugripper_hmi_test/result
```

判断口径：

- HMI 轮次失败、但左右 encoder 探针持续有样本，优先判断 HMI UART、线缆、协议或夹爪控制板异常。
- HMI 与同侧 encoder 同时停止产出，并伴随 CH9344/USB 内核错误，优先判断 CH9344、Hub、供电或 USB 上行链路异常。
- encoder MCAP 默认要求最大相邻时间戳 gap 不超过 `100ms`；即使总样本数和平均频率看似正常，单次秒级数据洞也会使测试失败，可通过 `--encoder-max-gap-ms` 调整门限。
- 蜂鸣器测试要求同时观察到非零 duty 的开启回读与 `duty=0` 的关闭回读；部分固件关闭后会保留最后一次 frequency 字段，因此关闭判据只看 duty。仅串口写成功不算通过。

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
