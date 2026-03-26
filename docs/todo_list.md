# TODO List

这个文件由你和 `todo-hub` 主控 agent 共同维护。你可以直接手改；skill 会按约定解析真实任务。

## 规则

- 默认不会启动全部任务；只会启动你明确点名、`focus = true` 或筛选命中的任务。
- 只有带 `<!-- TODO_HUB_TASK -->` 标记的 TOML 代码块才会被解析成真实任务。
- `slug` 使用英文短横线，用于分支名与 worktree 目录。
- 分支格式固定为：
  - `feature/zhouwu/<slug>`
  - `test/zhouwu/<slug>`
  - `fix/zhouwu/<slug>`

## 新任务模板（复制后记得补 `<!-- TODO_HUB_TASK -->` 标记）

```toml
id = "TASK-001"
title = "示例：补充 USB 音频回归测试"
type = "test"
slug = "usb-audio-regression"
status = "todo"
enabled = false
focus = false
depends_on = []
summary = ""
acceptance = [
  "写第一条验收条件",
  "写第二条验收条件",
]
notes = """
这里写上下文、限制、关注文件、排除范围等。
"""
```

## Active Tasks

<!-- TODO_HUB_TASK -->
```toml
id = "TASK-001"
title = "补充蜂鸣器独立测试链路"
type = "test"
slug = "buzzer-smoke-test"
status = "done"
enabled = false
focus = false
depends_on = []
summary = "补一条可独立执行的蜂鸣器测试路径，先验证硬件、驱动方式和回归入口。"
acceptance = [
  "明确蜂鸣器当前控制入口、依赖设备和执行命令",
  "提供可单独运行的测试脚本或测试步骤，能稳定触发蜂鸣器",
  "记录测试日志与失败时的排查口径，方便后续联调",
]
notes = """
目标是先把蜂鸣器单独测通，不和“运动过快提醒”逻辑绑死。
优先参考现有 py_script/usb_audio_play_test.py 这种独立测试脚本风格，输出明确的测试说明。
若当前仓库还没有蜂鸣器控制实现，需要先完成现状摸底，确认最终控制接口应放在哪一层。
"""
```

<!-- TODO_HUB_TASK -->
```toml
id = "TASK-002"
title = "新增夹爪运动过快告警与阈值调节接口"
type = "feature"
slug = "motion-threshold-alert"
status = "todo"
enabled = true
focus = true
depends_on = []
summary = "基于夹爪 IMU 加速度与角速度做过快判定，先输出独立告警信号并预留蜂鸣器控制接口。"
acceptance = [
  "在运行时增加加速度和角速度阈值判定，超过阈值时能触发独立告警事件",
  "在未接入真实蜂鸣器前，告警逻辑可通过日志、状态或 mock 控制接口单独运行验证",
  "支持通过键盘按键或命令行输入调节阈值，并能看到当前阈值和告警状态",
  "蜂鸣器控制接口与业务判定解耦，后续蜂鸣器完成后只需接线，不需重写判定核心",
]
notes = """
建议将“阈值判定 / 去抖 / 冷却时间 / 告警状态机”放在 record_runtime 层，
将实时 IMU 数据输出改成可订阅或共享的轻量接口，避免直接把蜂鸣器逻辑塞进 sensor_recorder。
键盘交互更适合做成开发态/单独运行模式，例如增加 test/demo 程序或 record_runtime 的调试输入开关，
先满足“单独运行可调阈值”，不强行耦合到正式录制主循环的人机输入。
TASK-001 完成后，再把预留接口接到真实蜂鸣器控制实现上做联调。
"""
```

<!-- TODO_HUB_TASK -->
```toml
id = "TASK-003"
title = "录制数据分段写内存并后台落盘"
type = "feature"
slug = "buffered-segmented-writeback"
status = "todo"
enabled = true
focus = true
depends_on = []
summary = "改造视频与传感器录制写盘路径，分段写入内存缓冲后再后台落盘，降低录制阻塞与停录耗时。"
acceptance = [
  "为视频与传感器录制链路统一设计分段缓冲与后台落盘方案，避免采集线程或录制子进程直接长期阻塞在目标盘写入上",
  "停录时能够有序收尾，确保视频文件、MCAP 文件和元数据都完整刷入且不破坏现有校验流程",
  "增加关键性能日志，能区分采集阶段、后台写盘阶段和 stop/flush 阶段耗时",
  "明确内存上限、背压策略、磁盘异常处理和降级策略，避免长时录制导致内存失控或数据损坏",
]
notes = """
这项任务不是 sensor_recorder 单点优化，而是录制链路整体改造。
当前 sensor_recorder 在主循环内直接调用 McapWriter.write；当前 camera_recorder 则通过 ffmpeg / gstreamer 子进程直接写 mkv 文件，
两边都可能被目标盘写入时延放大，只是形态不同。
实现时需要先拆成两部分方案：1）传感器链路的段缓冲 + 后台写线程；2）视频链路的本地缓冲/中转落地/异步搬运方案，
明确最终是“进程内内存段 + writer 线程”、还是“先写本地高速介质再后台迁移到数据盘”、还是“ffmpeg/gstreamer 管道前增加缓冲层”。
同时要结合 record_runtime 停录阶段的 camera stop、sensor stop、逐文件 fsync 和 validate 耗时一起评估，避免只优化一段却整体收益不明显。
建议把 segment 大小、最大缓存段数、后台 flush 周期、磁盘切换策略参数化，便于现场调优。
"""
```

<!-- TODO_HUB_TASK -->
```toml
id = "TASK-004"
title = "双目相机热启动与热插拔录制预热"
type = "feature"
slug = "stereo-camera-hot-start-hotplug"
status = "todo"
enabled = true
focus = true
depends_on = []
summary = "在开始录制前就维持双目相机拉流预热，开始录制后只切换到保存态，同时支持热插拔恢复且保证录制时间戳正确。"
acceptance = [
  "录制开始前双目相机采集链路已经处于拉流状态，但不会把预热阶段的帧写入本次录制产物",
  "开始录制时不通过重启相机管线切换到保存态，写入帧的时间戳连续、单调，并与录制会话时间基准对齐",
  "相机在录制前或录制中热插拔后能够自动恢复到预热或录制状态，并明确暴露 ready/not-ready 状态与错误日志",
  "明确预热缓冲边界、丢帧策略和恢复后的首帧对齐规则，避免把旧帧、重复帧或错误时间戳写进结果文件",
]
notes = """
核心目标是把“相机采集管线存活状态”和“是否将当前帧持久化到录制结果”拆开，避免 start record 时才冷启动双目相机。
需要重点梳理 camera_recorder / record_runtime / 录制文件命名与时间戳生成链路，确认时间戳以采集时刻、单调时钟还是 session 基准时间为准。
热插拔不仅是设备重新枚举成功，还要覆盖恢复后如何重新进入预热态、录制中恢复后如何安全续写，以及如何对外报告该段数据缺口。
实现时优先保证“录制文件中的时间戳正确且可解释”，不要为了保活预拉流把预热阶段缓存的旧帧直接灌入录制结果。
"""
```

<!-- TODO_HUB_TASK -->
```toml
id = "TASK-005"
title = "按 v1 口径回填触觉相机序列号到 episode 标定"
type = "feature"
slug = "episode-tactile-serial-injection"
status = "done"
enabled = false
focus = false
depends_on = []
summary = "已在起录前按现场设备回填 4 路 tactile 相机 serial，并把四路独立标定项写入 episode calibration.json。"
acceptance = [
  "参考 origin/feature/zhouwu/v1 的运行时注入逻辑，在每次开录前或生成 episode calibration.json 时读取当前 /dev/left_tcam_* 与 /dev/right_tcam_* 对应的真实 USB serial",
  "episode/calibration.json 中不再保留 {{TACTILE_LEFT_SERIAL}} / {{TACTILE_RIGHT_SERIAL}} 占位符，而是写入本次录制现场真实触觉相机 serial，并补齐 left_tcam_l/left_tcam_r/right_tcam_l/right_tcam_r 四路独立 tactile 标定项",
  "当 calibration 持久化文件中的 tactile serial 与现场硬件不一致时，至少在 episode 侧修正并输出明确日志；需要时说明是否回写 persist calibration",
  "补充定向验证步骤，能在当前设备上直接检查 episode/calibration.json 中的 tactile serial 是否与 udevadm 读取结果一致",
]
notes = """
明确参考 v1：origin/feature/zhouwu/v1 的 run_record.sh 在每次开录前会读取持久化 calibration.json，
然后用 udevadm 从 tactile 设备节点向上取 USB ATTRS{serial}，将 {{TACTILE_LEFT_SERIAL}} / {{TACTILE_RIGHT_SERIAL}} 占位符替换到当前 episode calibration.json。
当前 v2 的 RecordRuntime::EpisodeManager::writeFilteredCalibration 只是把 /etc/ugripper/config/calibration/calibration.json 过滤后落到 episode，
不会读取现场触觉 serial，因此这次任务需要把“运行时按设备注入 tactile serial”的能力补回到 v2。
当前已完成主仓回流：episode 侧仅输出四路独立 tactile 标定项；仍不回写 /etc/ugripper/config/calibration/calibration.json。
"""
```
