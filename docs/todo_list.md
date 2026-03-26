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
status = "todo"
enabled = true
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
