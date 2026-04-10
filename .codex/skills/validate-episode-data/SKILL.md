---
name: validate-episode-data
description: 为 ugripper 项目执行 episode 数据深度校验。用于用户在录完一条新数据后，指定“校验 episode / 检查这条录制数据 / 验证视频和 sensor 是否同步 / 看有没有 gap 或异常 / 校验最新一条数据”等请求时：先阅读 docs/agent/overview.md 理解当前 episode 结构与时间语义，再对指定 episode 目录执行只读分析；若用户说“校验最新一条数据”，默认对用户给出的数据父目录执行 `--latest` 选择最新 `episode_*`；重点检查视频时间对齐、逐帧 gap、容器时间戳异常、首帧同步误差、左右 sensor 同步、视频与 sensor 对齐、sensor topic gap、尾部缺失与其他可疑异常，并给出结论、证据和建议。
---

# validate-episode-data

## 固定执行顺序

1. 先阅读 `docs/agent/overview.md`，确认当前 episode 产物、`info.json` 时间字段语义、`stereo_session` 语义，以及默认视频 / sensor 输出结构。
2. 找到用户指定的 episode 目录；如果用户只给了父目录，可在其中定位最近一条 `episode_*`。
   - 若用户直接说“校验最新一条数据”，默认将这句话解释为“对指定数据父目录下最新的 `episode_*` 做校验”。
3. 无需等待确认，直接执行只读分析脚本：
   - `uv run python .codex/skills/validate-episode-data/scripts/validate_episode.py <episode_dir>`
   - 若只给数据父目录，优先使用：`uv run python .codex/skills/validate-episode-data/scripts/validate_episode.py --latest <parent_dir>`
   - 默认认为当前仓库 `uv` / `.venv` 环境可直接提供 `mcap` 依赖。
4. 若用户只关注某一类问题，可按需加参数缩小范围：
   - `--latest`：当传入的是父目录时，自动选择最新的 `episode_*`。
   - `--skip-video-packets`：跳过视频逐包扫描，只保留容器级检查。
   - `--json`：输出结构化结果，便于后续保存或机器消费。
   - `--top N`：调整输出的最大 gap 样本数。
5. 输出时必须区分：
   - 总体结论：`PASS / WARN / FAIL`
   - 具体异常项
   - 支撑证据
   - 建议下一步

## 默认检查范围

默认至少覆盖以下项目：

- episode 关键文件是否存在、非空、可读。
- `info.json` / `metadata.json` 结构与关键时间字段是否完整。
- 八路视频容器是否可读，是否存在明显时间戳异常。
- 八路视频的起止系统时间是否对齐，是否有异常长尾 / 短尾。
- 各视频流是否存在明显逐帧 / 逐包 gap、重复时间戳、回退时间戳。
- 首帧同步误差是否超阈值。
- `stereo_session` 中的首帧 / 结束时间与顶层 offset 语义是否自洽。
- 左右 `sensor_data_*.mcap` 是否可读，topic 是否齐全。
- IMU / encoder topic 是否存在 gap、长时间中断、样本数异常。
- 左右 sensor 同类型 topic 是否明显不同步。
- 每侧 sensor 的覆盖窗口与该侧视频窗口是否明显错位。
- encoder / imu 尾部是否明显早停。

## 建议重点补充的异常类型

除用户已提出的项目外，额外重点关注：

- 文件存在但样本数极少，说明链路可能起了但几乎没录到。
- 单路视频能播但时间轴回退，说明 mux 或时间戳可能异常。
- 双目 `stereo_session` 与实际文件首尾不一致，说明 warmup/session 边界可能失配。
- 左右同类 sensor 都正常，但视频与 sensor 整体错位，说明跨进程起录边界可能异常。
- 只有某一侧 encoder 或 imu 尾部缺失，说明串口或写线程可能中途掉线。
- 触觉相机与主相机 / stereo 普遍错开，说明该类设备独立启动或 stop 边界异常。
- 所有流都存在相近时刻的大 gap，说明可能是系统调度 / 写盘抖动，而不是单设备掉流。

## 输出要求

优先用简洁、可执行的结论，不要只堆原始统计：

1. 总结该条 episode 是否可用。
2. 列出最严重的异常，按严重度排序。
3. 每个异常都给出证据：
   - 文件或 topic
   - 时间点
   - gap / 偏差 / 时长差
4. 若能判断模式，明确写出高概率归因：
   - 单路掉流
   - 多路同时卡顿
   - 首帧未对齐
   - sensor 早停
   - 视频与 sensor 起止窗口错位
5. 最后给出下一步建议：
   - 是否建议重录
   - 是否需要看运行日志
   - 是否需要继续排查 camera_recorder / sensor_recorder / record_runtime

## 资源使用

- 阈值口径与检查项说明：读取 `references/checklist.md`
- 默认入口脚本：执行 `scripts/validate_episode.py`
- 默认运行方式：优先使用 `uv run python`
- 只在用户明确要求修复代码时，才切换到修改流程；本 skill 默认保持只读分析
