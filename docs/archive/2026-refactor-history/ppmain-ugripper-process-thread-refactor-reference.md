# PPMain And Ugripper Process/Thread Refactor Reference

本文档用于沉淀以下问题，作为后续完整重构和并入 `pp_main/standalone` 的参考输入：

1. `pp_main` 当前的进程/线程管理模式是什么
2. `ugripper` 当前的进程/线程管理模式是什么
3. 两者的关键差异在哪里
4. `ugripper` 后续应如何拆分和重构
5. 哪些设计可以直接复用 `pp_main`，哪些不能直接照搬

本文档不追求“最终目录树一次定稿”，而是聚焦运行时架构、模块边界和迁移顺序。

相关参考：

- `docs/standalone-merge-plan.md`
- `docs/ugripper-refactor-plan.md`
- `docs/archive/2026-refactor-history/repo-architecture.md`

## 1. 先讲结论

结论可以先压缩成 4 句话：

- `pp_main` 的主流 standalone 模式是“单应用进程 + 进程内明确的线程/定时任务/通信对象管理”。
- `ugripper` 当前主链更接近“supervisor 进程 + 多个 worker 进程 + 各自内部线程模型”。
- `ugripper` 不能靠简单替换几个工具类就变成 `pp_main` 风格，顶层运行模型需要重新收口。
- 最稳的方案不是先把大量模块合成单进程，而是保留少量关键多进程边界，并在每个进程内部学习 `pp_main/standalone` 的生命周期和多线程管理模式。

换句话说：

- `pp_main` 值得复用的是“进程内管理模式”
- `ugripper` 还需要补的是“进程间监督模式”

## 2. `pp_main` 的进程/线程管理模式

本节重点不是业务功能，而是它如何组织一个 standalone 应用。

### 2.1 顶层模式：薄 `main()` + 业务总装对象

典型入口见：

- `pp_main/standalone/Puppeteer/main.cc`
- `pp_main/standalone/Puppetry/main.cc`
- `pp_main/standalone/MessageBridge/main.cc`

它们大体遵循同一模式：

1. 解析命令行参数
2. 获取单实例锁
3. 初始化日志
4. 安装信号处理
5. 初始化业务对象
6. 主线程等待退出信号
7. 调用 Stop 流程并释放后台资源

这意味着：

- `main()` 负责进程外壳生命周期
- 业务对象负责真正的资源组装和退出顺序

这是后续 `ugripper` 并入 standalone 时最应该学习的第一层模式。

### 2.2 单实例保护：`SingleProcessInstance`

参考：

- `pp_main/src/utils/single_process_instance.h`
- `pp_main/src/utils/single_process_instance.cc`

特点：

- Windows 使用命名互斥体
- Linux 使用抽象 Unix Domain Socket `bind`
- 构造成功即获得锁
- 析构时关闭 fd 自动释放

它的定位非常清晰：

- 适合顶层 standalone 进程
- 不适合替代 supervisor 的子进程管理

因此后续若 `ugripper` 拆出：

- `UgripperRuntime`
- `CameraRecorder`
- `SensorRecorder`

则每个最终独立运行的顶层程序都可以单独使用这个机制。

### 2.3 统一周期调度：`TaskScheduler`

参考：

- `pp_main/src/utils/task_scheduler.h`
- `pp_main/src/utils/task_scheduler.cc`
- `pp_main/standalone/Puppeteer/puppeteer.cc`
- `pp_main/standalone/Puppeteer/anomaly_detector.cc`
- `pp_main/standalone/Puppeteer/pp_state_monitor.cc`
- `pp_main/standalone/Puppeteer/chain.cc`
- `pp_main/standalone/Puppeteer/joystick.cc`

`TaskScheduler` 的内部模型是：

- 一个全局单例
- 一个 oneTBB `task_group`
- 一个专门的 timer thread
- 一个按下一次触发时间排序的优先队列

职责分离如下：

- timer thread 负责“什么时候该执行”
- task group 负责“真正执行任务”

这个设计的核心价值是：

- 避免每个模块自己起 `while (running) { ... sleep(...) }` 轮询线程
- 将周期性工作统一收口
- 让周期可以用配置控制
- 让业务对象更像“注册任务”，而不是“自己维护线程”

`pp_main` 中适合交给 scheduler 的工作类型：

- 业务主循环
- 状态发布
- 异常探测
- 周期刷新
- 心跳类任务

不适合强行交给 scheduler 的工作类型：

- 阻塞串口读
- 阻塞设备采集
- 长时间等待外部进程 I/O
- 高耦合的 pipe 读写链路

这一点对 `ugripper` 特别重要：后续能学习 scheduler 模式，但不要把所有阻塞 I/O 都硬改成 timer task。

### 2.4 运行时配置中心：`ConfigManager`

参考：

- `pp_main/src/utils/config_manager.h`
- `pp_main/src/utils/config_manager.cc`

`ConfigManager` 不是单纯 YAML 读取器，而是一个轻量运行时控制面：

- 启动时加载 base YAML
- 扁平化为 `path -> json`
- 可叠加 `overrides.json`
- 提供 `Get/Set`
- 支持按前缀 `Subscribe`
- 可将局部变更持久化到 override 文件

这使得 `pp_main` 的模块可以做到：

- 初始值从配置中读取
- 运行中响应配置变化
- 不把所有参数写死在代码或脚本里

例如 `Puppeteer` 会根据 `/Puppeteer/run/interval_ms` 动态重建主循环任务。

这对 `ugripper` 的启发是：

- runtime 参数不应继续散落在 shell、默认常量和硬编码路径里
- timeout、poll 间隔、feature flag、路径、控制文件位置等都应逐步收口成统一配置

### 2.5 通信对象自己持有后台线程

参考：

- `pp_main/src/communication/publisher.cc`
- `pp_main/src/communication/subscriber.cc`
- `pp_main/src/communication/service_registrar.cc`
- `pp_main/src/communication/service_hub.cc`

`pp_main` 里有一个很值得保留的边界：

- 周期任务交给 `TaskScheduler`
- 消息收发线程归通信对象自己管理

具体来说：

- `Subscriber::Start()` 起一个接收线程，内部 `poll(100ms)`
- `Publisher` 在 buffered 模式下起一个 sender thread
- `ServiceRegistrar` 自己起 heartbeat thread
- `ServiceHub` 自己跑阻塞 loop，并起一个心跳检查线程

好处是：

- 业务层不需要自己再包一层消息线程
- pub/sub/service 生命周期更清晰
- Stop 边界比较明确

### 2.6 业务总装对象负责资源组装和 Stop 顺序

典型参考：

- `pp_main/standalone/Puppeteer/puppeteer.cc`
- `pp_main/standalone/Puppeteer/message_hub.cc`

`Puppeteer` 不是一个“自己做所有事”的巨型 while loop，而是一个总装对象：

- 初始化配置中心
- 初始化 chain / joystick / controller
- 初始化通信
- 注册 scheduler 周期任务
- 启动状态监控器和异常检测器
- Stop 时按顺序回收这些资源

值得学习的不是业务本身，而是这种组织方式：

- 业务对象负责组装子模块
- 子模块自己有明确的 `Init/Start/Stop`
- 退出顺序集中在总装对象里定义

### 2.7 `pp_main` 现有设计的优点

从运行时治理角度看，`pp_main` 的优点主要是：

- 顶层生命周期清晰
- 定时任务有统一入口
- 通信线程边界较清楚
- Stop 流程一般是显式的
- 动态参数能力比硬编码/脚本式控制更强
- `standalone/` 目录本身就是未来 `ugripper` 的目标落点

### 2.8 `pp_main` 不是完美范式，复用时要保留判断

几个需要明确记录的限制：

- `TaskScheduler::CancelTimerTask()` 取消语义比较弱，内部通过重建队列删除，且会 `sleep_for(100ms)`
- `ServiceHub::Init()` 本身阻塞，所以调用方必须额外包线程
- `MessageBridge` 里存在 `detach()` 的 ROS2 executor 线程
- `MessageBridge` 析构里有强制 `release registrar_` 的做法，属于务实规避，不是理想清理方式
- 某些业务模块仍带有较重的 singleton/static 使用痕迹

因此后续应“复用设计方向”，而不是逐行照抄实现。

## 3. `ugripper` 当前的进程/线程管理模式

本节重点是搞清楚它现在到底是什么模型，而不是先评价好坏。

### 3.1 顶层实际链路：`systemd -> shell -> runtime -> workers`

参考：

- `ugripper/pack_script/ugripper.service`
- `ugripper/run_record.sh`
- `ugripper/src/record_runtime/include/record_runtime.h`
- `ugripper/src/record_runtime/src/main.cpp`
- `ugripper/src/record_runtime/src/record_runtime.cpp`

当前链路是：

1. `systemd` 启动 service
2. service 执行 `run_record.sh`
3. shell 负责挂载等待、日志路径、环境读取等前置工作
4. shell 再执行 `record_runtime`
5. `record_runtime` 再启动并监督多个下游进程

这说明：

- `ugripper` 目前不是简单 standalone 应用
- shell 参与了运行时编排
- `record_runtime` 不是普通业务进程，而是 supervisor/orchestrator

### 3.2 `record_runtime` 的真实角色

从职责上看，`record_runtime` 当前同时承担了：

- 子进程启动/停止
- episode 创建与收尾
- 录制开始/停止编排
- HMI 按键输入和 LED/蜂鸣器反馈
- 健康检查
- 错误处理与恢复
- 一部分路径和环境治理

也就是说，它本质上已经是“控制面应用”，只是当前实现把大量职责揉在一起了。

### 3.3 `ProcessRunner`：当前已存在 supervisor 雏形

参考：

- `ugripper/src/record_runtime/include/record_runtime.h`
- `ugripper/src/record_runtime/src/record_runtime.cpp`

`ProcessRunner` 目前封装了：

- `fork`
- `execvp`
- `waitpid`
- `sendSignal`
- `stop(timeout)`

它说明了一件很重要的事：

`ugripper` 的顶层需求从来都不是“只有线程管理”，而是明确存在“进程管理”需求。

当前问题不在于“不该有 supervisor”，而在于：

- supervisor 逻辑分层不够
- 进程管理抽象太薄
- 业务编排和执行细节耦合太深

### 3.4 `camera_recorder` 是独立重链路进程

参考：

- `ugripper/src/camera_recorder/include/camera_recorder/camera_recorder.h`
- `ugripper/src/camera_recorder/src/main.cpp`
- `ugripper/src/camera_recorder/src/camera_recorder.cpp`

当前 `camera_recorder` 的内部已经不是一个简单对象，而是一个重运行时：

- 一部分工作通过外部 ffmpeg/gstreamer 子进程完成
- 一部分工作通过 capture thread、writer thread、output reader thread 完成
- 一部分工作还负责 stereo daemon/session 逻辑
- manager 层还会并发启动/停止多路 recorder

这类模块有几个特征：

- 设备依赖重
- 外部进程依赖重
- 故障模式复杂
- 卡顿/退出/管道异常都可能拖垮本体

因此这类模块在重构初期更适合继续保留独立进程边界。

### 3.5 `sensor_recorder` 是阻塞 I/O 型多线程进程

参考：

- `ugripper/src/sensor_recorder/src/main.cpp`
- `ugripper/src/sensor_recorder/src/im648_driver.cpp`
- `ugripper/src/sensor_recorder/src/encoder_driver.cpp`

当前内部线程模型大致包括：

- 左右 writer thread
- 左右 IMU init thread
- 左右 encoder init thread
- 左右 encoder read thread
- 左右 encoder request thread
- 主线程负责 draining / 写入队列

这个模型和 `pp_main` 里的 scheduler 型周期任务不是一类问题。

结论是：

- 可以学习 `pp_main` 的生命周期组织方式
- 不应在早期重构时把这类阻塞 I/O 全部改成 scheduler 驱动

### 3.6 `GripperHmiDriver` 已经是独立 I/O 线程模型

参考：

- `ugripper/src/gripper_hmi/include/gripper_hmi_driver.h`
- `ugripper/src/gripper_hmi/src/gripper_hmi_driver.cpp`

HMI 驱动内部本来就有：

- 独立 IO 线程
- 条件变量
- 独占命令时暂停/恢复后台线程

这也说明：

- 并不是所有模块都该被 scheduler 接管
- `ugripper` 后续需要的是“线程边界更清楚”，不是“一刀切只剩一种线程模式”

### 3.7 `ugripper` 当前模式的优点

虽然粗糙，但当前结构并不是毫无道理。

它的优点包括：

- camera/sensor/audio/stereo 的故障边界天然分开
- recorder 进程崩溃时 supervisor 还有机会接管
- 外部工具链和底层依赖可以按独立可执行体组织
- 现场部署链路已经按 service + runtime + workers 建立

### 3.8 当前模式的主要问题

当前痛点主要在治理层，而不是“多进程本身”：

- shell 参与了过多运行时编排
- `record_runtime` 职责过重
- 进程监督抽象太薄
- runtime 状态机与执行细节混杂
- 路径和配置入口散落
- 周期轮询逻辑没有统一调度面
- Stop 顺序和错误回滚难以复用

## 4. 两种模式的根本差异

这个差异必须先讲清，否则后续重构很容易误判。

### 4.1 `pp_main` 的主要问题域

`pp_main` 大多数 standalone 解决的是：

- 单个应用进程如何组织内部线程
- 周期任务如何统一管理
- 通信和服务发现如何组织
- 进程退出时如何有序清理

### 4.2 `ugripper` 当前的主要问题域

`ugripper` 当前首先解决的是：

- 一个顶层 supervisor 如何监督多个异构 worker 进程
- 开始/停止录制时如何编排多个执行单元
- HMI、音频、相机、传感器、stereo 状态如何协调
- 子进程异常退出时如何决策

因此两者不是“同一类程序写法不同”，而是“问题域层级不同”。

### 4.3 直接套 `pp_main` 设计为什么不够

如果只把 `SingleProcessInstance`、`TaskScheduler`、`ConfigManager` 搬进 `ugripper`，并不能自动解决：

- 子进程生命周期
- 录制编排回滚
- 跨 worker 的错误处理
- service/shell/runtime 的职责重画

所以后续正确做法应是：

- 复用 `pp_main` 的进程内组织思路
- 补出 `ugripper` 缺失的进程间监督抽象

## 5. 后续重构的总原则

### 5.1 不要先追求“大一统单进程”

当前阶段不建议优先做的事：

- 把 camera、sensor、audio、HMI 大量合并到一个大进程
- 一边并仓，一边改所有进程边界
- 一边重写控制面，一边重写采集面

原因：

- 风险叠加太多
- 回归定位困难
- 很容易把进程间复杂度搬成进程内复杂度

### 5.2 推荐方案：少量关键多进程 + 进程内学习 standalone 模式

更稳的方向是：

- 保留少量关键多进程边界
- 在每个进程内部使用 `pp_main/standalone` 风格管理生命周期
- 对顶层 `record_runtime` 做职责拆分和控制面重构

### 5.3 控制面与采集面分离

建议把未来架构明确分为两层：

- 控制面
  - runtime
  - 状态机
  - HMI
  - 健康检查
  - 子进程监督
  - episode 编排

- 采集面
  - camera recorder
  - sensor recorder
  - audio recorder/player
  - stereo worker

控制面学习 `pp_main` 的 standalone 模式；采集面优先维持稳定边界。

## 6. `record_runtime` 应该怎么拆

这里的关键结论是：

后续是否叫 `UgripperRuntime` 并不重要，但“runtime/orchestrator 这个角色”需要保留。

因为只要 `camera_recorder` 和 `sensor_recorder` 还保持独立进程，就必须有一个统一决策者去处理：

- 启动和停止顺序
- 错误回滚
- HMI 触发
- 健康状态汇总
- 录制状态迁移

### 6.1 目标职责切分

建议最终拆成以下模块。

#### `RuntimeApp` 或 `UgripperRuntime`

职责：

- 顶层业务壳
- `Initialize()`
- `Run()`
- `Stop()`
- 组装下列模块
- 定义退出顺序

不应直接承载：

- 具体 `fork/exec`
- 复杂 episode 文件处理
- 大量 HMI 细节

#### `RuntimeConfig`

职责：

- 路径
- timeout
- poll 周期
- feature flags
- codec
- 运行时文件位置

后续建议逐步向 `ConfigManager` 风格对齐。

#### `ProcessSupervisor`

职责：

- 监督和控制：
  - `camera_recorder`
  - `sensor_recorder`
  - 音频相关进程
  - stereo daemon
- 统一：
  - start
  - stop
  - wait
  - signal
  - timeout 升级
  - exit code 查询

这层只做执行，不做业务决策。

#### `RecordingOrchestrator`

职责：

- `StartRecording()`
- `StopRecording()`
- episode 生命周期管理
- 调用 supervisor 按顺序启动/停止 worker
- 失败回滚
- 停止后 finalize / merge / 校验编排

它是“录制业务编排层”，不是“进程执行层”。

#### `HmiController`

职责：

- 管理 `GripperHmiDriver`
- 轮询按钮状态
- 输出按钮事件或状态快照
- 控制 LED / 蜂鸣器
- 管理按钮输入状态机

注意：

- 它不应直接决定是否 kill 子进程
- 它应该输出“输入意图”，再交由上层决策

#### `HealthMonitor`

职责：

- 磁盘健康检查
- HMI 健康检查
- 子进程存活检查
- stereo 状态检查
- fault 汇总

这层是最适合学习 `TaskScheduler` 模式的模块之一。

### 6.2 推荐依赖方向

建议依赖关系保持单向：

- `RuntimeApp`
  - 依赖 `RuntimeConfig`
  - 依赖 `RecordingOrchestrator`
  - 依赖 `HmiController`
  - 依赖 `HealthMonitor`

- `RecordingOrchestrator`
  - 依赖 `ProcessSupervisor`
  - 依赖 `EpisodeManager`
  - 可依赖 `RuntimeConfig`

- `HealthMonitor`
  - 依赖 `ProcessSupervisor`
  - 可依赖 `HmiController`
  - 可依赖 `RuntimeConfig`

- `HmiController`
  - 依赖 `GripperHmiDriver`

需要避免的依赖：

- `ProcessSupervisor -> RecordingOrchestrator`
- `HmiController -> ProcessSupervisor`
- `EpisodeManager -> HmiController`

否则会再次退化成巨型控制对象。

### 6.3 拆分顺序建议

不建议一次性全拆。建议顺序如下：

1. 先抽 `ProcessSupervisor`
2. 再抽 `HmiController`
3. 再抽 `RecordingOrchestrator`
4. 最后抽 `HealthMonitor` 和 `RuntimeConfig`
5. 最后把 `main + RuntimeApp` 收成 standalone 风格

理由：

- `ProcessSupervisor` 最容易抽，也最能立刻减少 `record_runtime` 的噪音
- `HmiController` 抽出后，顶层逻辑会明显清爽
- `RecordingOrchestrator` 是最重的一层，放在中段做风险更可控

## 7. 哪些边界建议保留多进程

### 7.1 建议保留独立进程的模块

重构初期建议继续保持独立进程：

- `camera_recorder`
- `sensor_recorder`

理由：

- 设备依赖重
- 阻塞 I/O 重
- 线程模型本来就复杂
- 各自故障模式不同
- 相机链路本身还有外部编码/管道子进程

### 7.2 不建议早期强行合并的内容

以下内容在早期不建议合入一个大进程：

- camera 采集与编码执行链路
- sensor 采样与写盘执行链路
- stereo daemon 内部采集链路

原因不是“永远不能合”，而是：

- 初期收益小
- 生命周期复杂度显著增加
- 故障隔离反而变差

### 7.3 更适合收口到单一 runtime 进程的部分

适合收口到新的 `RuntimeApp/UgripperRuntime` 的内容：

- `run_record.sh` 中与业务相关的挂载准备/路径准备逻辑
- HMI 按键、LED、蜂鸣器控制
- 录制状态机
- 健康检查
- 子进程监督
- episode 编排

也就是说：

- 控制面收进一个 standalone
- 采集面先保持少量独立 worker

## 8. 从 `pp_main` 可直接复用什么

### 8.1 可以直接复用的设计思路

- 薄 `main()`
- 统一 `Initialize/Stop`
- 单实例保护
- 统一 scheduler 处理周期任务
- 配置中心化
- 通信对象自己持有线程
- 总装对象负责退出顺序

### 8.2 可以比较直接引入的组件

从公共能力角度，后续可优先复用：

- `SingleProcessInstance`
- `TaskScheduler`
- `ConfigManager`
- logger 体系
- `Publisher/Subscriber`

其中：

- `TaskScheduler` 用于控制面周期任务
- `Publisher/Subscriber` 用于未来如果需要把 runtime 和 worker 控制协议显式化

### 8.3 需要选择性复用的设计

以下设计适合借鉴方向，不适合完全照搬：

- `MessageHub`
- `ServiceHub/ServiceRegistrar`
- `MessageBridge` 的线程处理方式

建议是：

- 保留“通信适配层”的思想
- 不必复制原有 heavy singleton/static 写法

## 9. 不应直接照搬 `pp_main` 的地方

### 9.1 不要把所有线程都改成 scheduler 任务

特别不应直接改造：

- 串口读线程
- encoder 请求/读线程
- 相机采集线程
- pipe/output reader 线程

这些本质是阻塞 I/O 或高耦合执行链，和 scheduler 的使用场景不同。

### 9.2 不要用 `SingleProcessInstance` 代替 supervisor 需求

单实例锁只解决：

- 某个顶层程序只能跑一个

它不解决：

- 子进程管理
- worker 故障恢复
- 跨 worker 编排

### 9.3 不要直接复制不够干净的实现细节

例如：

- `detach()` 的线程清理方式
- 依赖晚期析构顺序的对象回收
- 用全局 singleton 掩盖复杂依赖

这些只能作为现有工程妥协案例，不应作为新 runtime 的默认方案。

## 10. 推荐的目标架构

推荐架构如下：

### 10.1 顶层

- `standalone/UgripperRuntime`
  - 控制面 standalone
  - 负责状态机、HMI、健康检查、录制编排、子进程监督

### 10.2 worker

- `standalone/CameraRecorder`
  - 独立相机录制进程

- `standalone/SensorRecorder`
  - 独立传感器录制进程

### 10.3 公共层

- `src/utils`
  - 配置、日志、路径、时间、命令执行、进程监督 helper

- `src/runtime`
  - `ProcessSupervisor`
  - `RecordingOrchestrator`
  - `HealthMonitor`
  - `HmiController`

若 Phase 1 暂不迁目录，也可先在 `ugripper/src/record_runtime` 内部按模块拆分，再在后续并仓时迁入 `standalone/`。

## 11. 推荐迁移阶段

### Phase 1：仓内重构，不改主进程边界

目标：

- `record_runtime` 拆分职责
- 抽出 `ProcessSupervisor`
- 抽出 `HmiController`
- 抽出 `RecordingOrchestrator`
- 收口配置和路径
- shell 降级为薄启动器

此阶段不要求：

- 立刻并入 `pp_main`
- 立刻改 worker 进程边界

### Phase 2：控制面向 standalone 模式对齐

目标：

- 建立 `RuntimeApp` 风格入口
- 对齐 logger / config / scheduler / Stop 模式
- 将控制面逻辑从 shell 和巨型 `.cpp` 收口到 C++ 应用壳

### Phase 3：逐步并入 `pp_main/standalone`

目标：

- 将稳定后的 `UgripperRuntime`、`CameraRecorder`、`SensorRecorder` 对齐到 `pp_main/standalone` 风格
- 统一公共能力到 `pp_main/src/utils`
- 评估是否需要显式消息控制协议和服务发现

### Phase 4：评估是否继续缩减进程边界

只有在以下条件满足后才评估：

- 控制面稳定
- recorder 内部职责已清楚
- worker 协议明确
- Stop 顺序、故障处理和回归手段已完善

否则不建议继续追求单进程化。

## 12. 重构判断标准

后续做设计决策时，可用下面几个问题快速判断模块该放哪：

### 12.1 这是“控制逻辑”还是“采集执行逻辑”

- 若是控制逻辑，倾向放入 runtime
- 若是采集执行逻辑，倾向保留在 recorder 进程

### 12.2 这是“周期检查”还是“阻塞 I/O”

- 周期检查，倾向 scheduler
- 阻塞 I/O，倾向保留独立线程

### 12.3 这是“决策”还是“执行”

- 决策放 orchestrator / runtime
- 执行放 supervisor / worker

### 12.4 这是“顶层单实例问题”还是“子进程监督问题”

- 单实例问题用 `SingleProcessInstance`
- 子进程监督问题用 `ProcessSupervisor`

## 13. 最终建议

如果只保留一条最重要建议，那就是：

**不要把“学习 `pp_main`”理解成“把 `ugripper` 全部改成单进程”。**

更准确的做法应该是：

- 保留关键多进程边界
- 把 `record_runtime` 重构成清晰的 standalone 控制面应用
- 在每个进程内部采用 `pp_main` 风格的生命周期、配置、周期任务和 Stop 管理方式

这条路线兼顾：

- 风险可控
- 故障隔离
- 未来并仓可行性
- 与 `pp_main/standalone` 的结构收敛

## 14. 关键源码参考

### `pp_main`

- `pp_main/src/utils/task_scheduler.h`
- `pp_main/src/utils/task_scheduler.cc`
- `pp_main/src/utils/single_process_instance.h`
- `pp_main/src/utils/single_process_instance.cc`
- `pp_main/src/utils/config_manager.h`
- `pp_main/src/utils/config_manager.cc`
- `pp_main/src/communication/publisher.cc`
- `pp_main/src/communication/subscriber.cc`
- `pp_main/src/communication/service_hub.cc`
- `pp_main/src/communication/service_registrar.cc`
- `pp_main/standalone/Puppeteer/main.cc`
- `pp_main/standalone/Puppeteer/puppeteer.cc`
- `pp_main/standalone/Puppeteer/message_hub.cc`
- `pp_main/standalone/MessageBridge/main.cc`
- `pp_main/standalone/MessageBridge/message_bridge.cc`

### `ugripper`

- `ugripper/run_record.sh`
- `ugripper/pack_script/ugripper.service`
- `ugripper/src/record_runtime/include/record_runtime.h`
- `ugripper/src/record_runtime/src/main.cpp`
- `ugripper/src/record_runtime/src/record_runtime.cpp`
- `ugripper/src/camera_recorder/include/camera_recorder/camera_recorder.h`
- `ugripper/src/camera_recorder/src/main.cpp`
- `ugripper/src/camera_recorder/src/camera_recorder.cpp`
- `ugripper/src/sensor_recorder/src/main.cpp`
- `ugripper/src/sensor_recorder/src/im648_driver.cpp`
- `ugripper/src/sensor_recorder/src/encoder_driver.cpp`
- `ugripper/src/gripper_hmi/include/gripper_hmi_driver.h`
- `ugripper/src/gripper_hmi/src/gripper_hmi_driver.cpp`

## 15. 设计补丁：把本文档从“方向文档”补到“落地设计输入”

本节用于补齐执行阶段最容易被 reviewer 追问的内容：

- `record_runtime` 现状代码如何映射到目标模块
- Phase 1 结束后入口、线程、进程、配置和测试分别长什么样
- `ProcessSupervisor` 和 `RecordingOrchestrator` 的合同是什么
- shell、systemd、runtime 的边界如何重画

### 15.1 为什么 Phase 1 不直接并入 `pp_main`

这个 tradeoff 需要显式记录：

- 当前最高风险不在目录位置，而在 `record_runtime` 职责过重、进程监督抽象不足、shell 参与过多运行时编排
- 如果同时做“仓内拆分 + 并入主仓 + 进程模型调整”，会把架构风险、构建风险、部署风险和回归风险叠加
- Phase 1 先在 `ugripper` 仓内把控制面拆清楚，可以把后续并仓工作变成“结构迁移 + 规范对齐”，而不是“在主仓里边猜边改”

因此推荐顺序必须是：

1. 先仓内拆分 `record_runtime`
2. 再对齐 `pp_main/standalone` 模式
3. 最后并入主仓

### 15.2 Phase 1 结束后的目标形态

Phase 1 不追求最终并仓，但应达到下面的运行时结构。

#### 15.2.1 入口

- 保留一个顶层 `record_runtime` 可执行体
- `main()` 只做：
  - 参数解析
  - logger 初始化
  - 信号处理
  - 创建 `RuntimeApp`
  - 调用 `Initialize()/Run()/Stop()`

#### 15.2.2 进程

- 保留独立 worker：
  - `camera_recorder`
  - `sensor_recorder`
  - 音频播放/录音脚本
  - stereo daemon

- 顶层仍是一个 runtime 监督这些 worker

#### 15.2.3 线程

`record_runtime` 进程内部建议变成：

- 主线程
  - 负责生命周期、状态转移和事件分发
- `TaskScheduler` timer thread
  - 负责周期健康检查、worker 存活检查、音频/daemon 维护检查
- `TaskScheduler` worker threads
  - 负责轻量非阻塞周期任务
- HMI 驱动内部 I/O 线程
  - 保持在 `GripperHmiDriver` 内部

Phase 1 不新增更多业务自管轮询线程，除非该逻辑天然属于阻塞 I/O。

#### 15.2.4 配置

Phase 1 不要求立刻上 `ConfigManager`，但至少要形成统一配置对象，按类别拆清：

- 运行模式类
- 路径类
- 设备类
- timeout / polling 类
- feature flag 类
- 调试/诊断类

#### 15.2.5 测试

Phase 1 至少要具备：

- 无硬件最小启动测试
- start/stop happy path 冒烟测试
- worker 启停和异常退出测试
- 状态机非法跳转测试
- shell/runtime 兼容入口测试

## 16. 现状到目标的迁移映射表

本节是执行阶段最重要的表。它不是穷举所有 helper，而是覆盖高风险主链逻辑。

### 16.1 `record_runtime` 迁移映射

| 现有位置 | 当前职责 | 目标模块 | Phase | 是否保留兼容层 | 风险 |
| --- | --- | --- | --- | --- | --- |
| `RecordRuntime::initialize()` | 读环境、默认 gripper port、创建 episode manager、连接 panel、启动 LED、音频播放器、stereo daemon | `RuntimeApp` 组装流程；其中环境与默认值进入 `RuntimeConfig`，音频/stereo 启动委托给 `AudioCoordinator` / `StereoSessionClient` | P1 | 否 | 中 |
| `RecordRuntime::run()` | 主监督循环；维护音频、维护 stereo、健康检查、轮询按钮、检测 worker 退出、按 poll 周期 sleep | `RuntimeApp` 主循环 + `TaskScheduler` 周期任务 + `HmiController` 事件输入 | P1 | 否 | 高 |
| `RecordRuntime::requestStop()` | 顶层 stop 标志 | `RuntimeApp` | P1 | 否 | 低 |
| `RecordRuntime::startRecording()` | episode 创建、prepare、检查二进制、并发拉起 camera/sensor、开始 stereo session、设置录制状态 | `RecordingOrchestrator`；worker 拉起委托 `ProcessSupervisor`；stereo 指令委托 `StereoSessionClient` | P1 | 否 | 高 |
| `RecordRuntime::stopRecording()` | stop stereo、stop camera/sensor、flush、stereo finalize、merge、validate、LED/audio 反馈 | `RecordingOrchestrator`；worker stop 委托 `ProcessSupervisor`；finalize/merge/validate 委托 `EpisodeManager` + `StereoSessionClient` | P1 | 否 | 高 |
| `RecordRuntime::handleShortUpAction()` / `handleShortDownAction()` | 录制开始/停止/重录决策 | `RuntimeApp` 事件分发到 `RecordingOrchestrator` | P1 | 否 | 中 |
| `RecordRuntime::handleLongUpAction()` / `handleLongDownAction()` | pre/post audio 业务动作 | `RuntimeApp` 分发到 `AudioCoordinator` | P1 | 否 | 中 |
| `RecordRuntime::handleDualShutdownAction()` | 停录、写 shutdown request file、请求退出 | `RuntimeApp` + `RecordingOrchestrator` + 部署兼容层 | P1 | 是 | 中 |
| `RecordRuntime::handleButtons()` | 按钮去抖、长按判定、双键判定、事件触发 | `HmiController` | P1 | 否 | 高 |
| `RecordRuntime::monitorHardwareHealth()` / `evaluateHardwareHealth()` | 磁盘、关键设备、stereo、HMI 健康检查 | `HealthMonitor` | P1 | 否 | 高 |
| `RecordRuntime::checkRecorderProcesses()` | camera/sensor worker 存活检查 | `HealthMonitor` 或 `ProcessSupervisor` 状态查询接口 | P1 | 否 | 中 |
| `RecordRuntime::startAudioPlayer()` / `stopAudioPlayer()` / `maintainAudioPlayer()` | 音频播放器进程管理与 ready 检查 | `AudioCoordinator` + `ProcessSupervisor` | P1 | 否 | 中 |
| `RecordRuntime::sendAudioCommand()` / `setAudioRecoveryCommand()` / `recordAudioClip()` / `attachPendingPreAudio()` | 音频提示、录音、pre/post audio 文件处理 | `AudioCoordinator` | P1 | 否 | 中 |
| `RecordRuntime::startStereoDaemon()` / `stopStereoDaemon()` / `maintainStereoDaemon()` / `writeStereoControl()` / `waitForStereoFinalize()` | stereo daemon 生命周期、控制文件、状态文件、finalize 等待 | `StereoSessionClient` + `ProcessSupervisor` | P1 | 否 | 高 |
| `RecordRuntime::mergeEpisodeInfo()` | 将 stereo 信息合入 episode 输出 | `EpisodeFinalizer` 或 `RecordingOrchestrator` 内部 finalize 子组件 | P1 | 否 | 中 |
| `RecordRuntime::syncRuntimeLogToDisk()` | 运行日志同步到磁盘 | `RuntimeApp` 部署兼容层；P1 保留，P2 评估是否移除 | P1 | 是 | 低 |
| `RecordRuntime::ProcessRunner` | `fork/exec/signal/wait/stop` | `ProcessSupervisor` 内部基础执行类 `SubprocessHandle` | P1 | 否 | 高 |
| `RecordRuntime::GripperPanelManager` | 多 HMI 设备连接、状态轮询、LED 分发、重连 | `HmiController` 内部设备层 | P1 | 否 | 中 |
| `RecordRuntime::HmiLedController` | LED 状态控制 | `HmiController` 内部 | P1 | 否 | 低 |
| `RecordRuntime::EpisodeManager` | episode 创建、输出准备、metadata、校验 | 先保留 `EpisodeManager`，只调整其调用边界 | P1 | 否 | 中 |
| `runCommandSync()` / `runCommandCapture()` / `fileExistsAndNotEmpty()` | 工具型 helper | `src/utils` 或 `runtime/common` | P1 | 否 | 低 |

### 16.2 当前暂时不动的部分

为控制 Phase 1 风险，下面内容原则上不做深度改写：

| 现有位置 | 暂不大动的原因 | 目标策略 |
| --- | --- | --- |
| `src/camera_recorder/src/camera_recorder.cpp` 内部录制线程/子进程细节 | 设备依赖重、外部进程重、故障模式复杂 | 仅保持 worker 边界稳定，后续再单独重构 |
| `src/sensor_recorder/src/main.cpp` 内部 writer / imu / encoder 线程模型 | 阻塞 I/O 与写盘耦合重 | 不强行改成 scheduler 模式 |
| `src/gripper_hmi/src/gripper_hmi_driver.cpp` 内部 I/O 线程与独占命令细节 | 已形成相对完整的驱动边界 | 只在控制层重新封装，不重写驱动 |
| `EpisodeManager` 内部大量文件准备与校验细节 | 已承担明确职责，修改成本高 | Phase 1 保持职责稳定，只抽调用边界 |

## 17. Phase 1 建议的模块清单

相比前文“方向性模块”，这里给出更适合直接落代码的粒度。

### 17.1 顶层模块

- `RuntimeApp`
- `RuntimeConfig`
- `RecordingOrchestrator`
- `ProcessSupervisor`
- `HmiController`
- `HealthMonitor`
- `AudioCoordinator`
- `StereoSessionClient`
- `EpisodeManager`

### 17.2 为什么新增 `AudioCoordinator` 和 `StereoSessionClient`

若没有这两个模块，`RecordingOrchestrator` 会再次膨胀成巨型控制对象。

建议职责切分为：

- `RecordingOrchestrator`
  - 决定何时开始/停止录制
  - 组织 worker 启停顺序
  - 组织 finalize / validate 顺序

- `AudioCoordinator`
  - 音频播放器生命周期
  - pre/post audio 录音
  - 音频提示命令和 recovery 状态

- `StereoSessionClient`
  - stereo daemon 生命周期
  - 控制文件与状态文件写读
  - stop-session / finalize 等待

## 18. `ProcessSupervisor` 合同

本节定义 `ProcessSupervisor` 不是“ProcessRunner 的加强版”，而是一个有明确语义的控制接口。

### 18.1 管理对象

`ProcessSupervisor` 管理的是“命名的 worker 进程实例”，不是随手运行的一次性 shell 命令。

建议管理对象至少包括：

- `audio_player`
- `audio_recorder`
- `stereo_daemon`
- `camera_recorder`
- `sensor_recorder`

建议用 `ProcessSpec` + `ProcessHandle` 分离：

- `ProcessSpec`
  - 名字
  - argv 构造规则
  - 默认 stop 超时
  - readiness 检查方式
  - restart policy

- `ProcessHandle`
  - pid
  - 启动时间
  - 最近退出码
  - 最近停止原因
  - 当前状态

### 18.2 建议状态

建议内部状态至少包括：

- `Stopped`
- `Starting`
- `Running`
- `Stopping`
- `ExitedExpected`
- `ExitedUnexpected`
- `FailedToStart`

### 18.3 `start()` 语义

`start(name, spec)` 必须满足：

- 幂等性：
  - 若已在 `Running/Starting`，返回“已运行”或显式错误，不重复 fork
- 成功条件：
  - 不是单纯 `fork` 成功
  - 必须满足该 worker 的 readiness 条件
- readiness 判定方式：
  - Phase 1 允许按 worker 区分
  - 例如：
    - `audio_player`：ready file 出现
    - `stereo_daemon`：status file 可读且 `ready=true`
    - `camera_recorder` / `sensor_recorder`：进程在最短观察窗内未提前退出，且返回启动确认日志或约定文件

### 18.4 `stop()` 语义

`stop(name, reason)` 必须满足：

- 幂等
- 可重复调用
- 若已停止，返回 success
- 默认升级链路：
  - 先发送优雅停止信号
  - 超时后升级到强制终止

Phase 1 的默认升级链路建议：

- worker 默认：`SIGTERM -> SIGKILL`
- 若某个 worker 当前约定必须先 `SIGINT`，则由 `ProcessSpec` 显式配置

不要把升级策略散落在各业务调用点。

### 18.5 expected exit 与 unexpected exit

必须显式区分：

- expected exit
  - 由 runtime 主动 stop
  - 或启动后按预期完成的一次性进程

- unexpected exit
  - 未经 runtime 允许的中途退出
  - 启动观察窗内提前退出
  - readiness 未通过就退出

`HealthMonitor` 和 `RecordingOrchestrator` 只消费这个语义结果，不再自己猜测。

### 18.6 restart policy

建议 Phase 1 只支持两种：

- `Never`
- `OnUnexpectedExit`

默认建议：

- `camera_recorder`：`Never`
- `sensor_recorder`：`Never`
- `audio_player`：`OnUnexpectedExit`
- `stereo_daemon`：`OnUnexpectedExit`

理由：

- camera/sensor 在录制过程中的意外退出更应上报 fault，而不是后台悄悄重启
- 音频提示和 warmup daemon 更适合尝试恢复

### 18.7 stdout / stderr

Phase 1 至少要明确归属：

- 默认仍并入 runtime 日志体系
- 若暂时无法统一接 logger，至少不能静默丢弃

建议：

- Phase 1 支持：
  - 继承父进程 stdout/stderr
  - 或重定向到 runtime 管理的日志文件

不建议 Phase 1 直接做复杂日志多路复用，但要在接口上预留。

### 18.8 liveness / readiness

需要区分：

- liveness：进程还活着吗
- readiness：进程已经准备好可被业务使用吗

这两个概念不能再混成“pid 还在就算成功”。

## 19. `RecordingOrchestrator` 状态机

这是高风险核心。若不显式定义，很容易重新长成另一个巨型控制对象。

### 19.1 状态列表

建议状态至少包括：

- `Idle`
- `Preparing`
- `StartingWorkers`
- `Recording`
- `Stopping`
- `Finalizing`
- `Faulted`

### 19.2 输入事件列表

建议按来源分类。

来自 HMI：

- `StartRequested`
- `ResetRecordingRequested`
- `StopRequested`
- `PreAudioRequested`
- `PostAudioRequested`
- `ShutdownRequested`

来自 worker / supervisor：

- `WorkerStarted(name)`
- `WorkerStartFailed(name)`
- `WorkerExitedUnexpected(name, exit_code)`
- `WorkerStopped(name)`

来自健康检查：

- `HealthFaultRaised(key)`
- `HealthRecovered`

来自内部流程：

- `PrepareSucceeded`
- `PrepareFailed`
- `FinalizeSucceeded`
- `FinalizeFailed`
- `ValidationSucceeded`
- `ValidationFailed`

### 19.3 状态转移表

| 当前状态 | 输入事件 | 下一状态 | 动作 |
| --- | --- | --- | --- |
| `Idle` | `StartRequested` | `Preparing` | 创建 episode、准备输出、附加 pre-audio |
| `Idle` | `ResetRecordingRequested` | `Preparing` | 检查 `lastEpisodeDir`，准备 reset 录制 |
| `Idle` | `PreAudioRequested` | `Idle` | 调 `AudioCoordinator::RecordPreAudio()` |
| `Idle` | `PostAudioRequested` | `Idle` | 调 `AudioCoordinator::RecordPostAudio()` |
| `Idle` | `ShutdownRequested` | `Idle` 或退出中 | 创建 shutdown request，通知 `RuntimeApp` 退出 |
| `Preparing` | `PrepareSucceeded` | `StartingWorkers` | 并发启动 camera/sensor，启动 stereo session |
| `Preparing` | `PrepareFailed` | `Faulted` | 设置错误反馈，保留诊断信息 |
| `StartingWorkers` | 全部 worker ready | `Recording` | 清理恢复状态，播放 start 提示 |
| `StartingWorkers` | 任一 `WorkerStartFailed` | `Faulted` | 回滚已启动 worker，记录失败原因 |
| `Recording` | `StopRequested` | `Stopping` | 发送停止序列 |
| `Recording` | `WorkerExitedUnexpected` | `Faulted` | 进入 fault-stop 流程 |
| `Recording` | `HealthFaultRaised` | `Faulted` | 进入 fault-stop 流程 |
| `Stopping` | 关键 worker stopped | `Finalizing` | 等待 stereo finalize，merge，flush |
| `Finalizing` | `FinalizeSucceeded + ValidationSucceeded` | `Idle` | 更新 `lastEpisodeDir`，恢复 Ready 提示 |
| `Finalizing` | `FinalizeFailed` 或 `ValidationFailed` | `Faulted` | 写诊断、输出错误提示 |
| `Faulted` | `StopRequested` 或内部 fault-stop 完成 | `Idle` 或保留 `Faulted` | 取决于是否需要人工确认；Phase 1 建议回到 `Idle` 但保留错误状态字段 |

### 19.4 幂等约束

必须显式保证：

- 在 `Idle` 时再次 `StopRequested` 不报错
- 在 `Recording` 期间再次 `StartRequested` 直接拒绝
- 在 `Stopping/Finalizing` 期间新的 start 请求直接拒绝
- fault-stop 过程中重复收到 worker 异常事件不重复触发 stop

### 19.5 回滚规则

建议回滚规则：

- `Preparing` 失败：
  - 不启动 worker
  - 不创建 recording 状态
- `StartingWorkers` 中途失败：
  - 停掉已经启动成功的 worker
  - 停止 stereo session
  - 保留 episode 目录用于诊断
- `Recording` 中途 fault：
  - 进入 `Stopping`
  - 仍尽量执行 finalize / validate
  - 最终回到 `Idle`，但保留 `last_fault`

## 20. shell / systemd / runtime 边界

### 20.1 推荐边界原则

- systemd：只负责进程保活、基础环境和 stop 超时
- shell：只保留薄兼容层
- runtime：接管绝大多数业务判断和运行时编排

### 20.2 现状逻辑如何重画

| 现有位置 | 当前逻辑 | Phase 1 目标归属 |
| --- | --- | --- |
| `ugripper.service` | `ExecStart`、`Restart=always`、`TimeoutStopSec=20s`、`KillMode=control-group` | 继续保留在 systemd |
| `run_record.sh` | `findmnt` 等待 `/mnt/data_disk` 可写 | Phase 1 可保留 shell；Phase 2 移入 `RuntimeApp` 启动前检查 |
| `run_record.sh` | 读取 `/etc/environment` 的 `DEVICE_SN` | 移入 `RuntimeConfig` |
| `run_record.sh` | 本地日志文件名拼装和同步到磁盘 | Phase 1 可保留薄兼容；Phase 2 与 logger/部署策略统一 |
| `run_record.sh` | 查找并执行 `record_runtime` 二进制 | 长期应由 systemd 直接启动 runtime；Phase 1 可保留兼容入口 |
| `record_runtime` | 业务状态机、worker 编排、错误回滚 | 必须保留在 runtime |

### 20.3 对 `run_record.sh` 的建议

建议分阶段处理：

- Phase 1：
  - 保留 `run_record.sh` 作为兼容入口
  - 但明确它不再新增业务逻辑
  - 只允许保留：
    - 二进制路径适配
    - 存储等待兼容
    - 日志同步兼容

- Phase 2：
  - 评估由 systemd 直接起 `record_runtime`
  - shell 降为可选运维兼容脚本

### 20.4 为什么不建议一开始彻底删 shell

因为 shell 当前还承担：

- 存储可写等待
- 本地/磁盘日志双写兼容
- 部署路径适配

这些逻辑在还没稳定迁入 `RuntimeApp` 之前，直接删 shell 会增加部署风险。

## 21. 配置收口方案

### 21.1 配置类别

建议至少分成六类。

#### 运行模式类

- 是否启用音频提示
- 是否启用 stereo daemon
- 是否允许 reset recording

#### 路径类

- camera/sensor 二进制路径
- 音频脚本路径
- `diskRoot`
- `envFile`
- stereo control/status file
- shutdown request file

#### 设备类

- gripper ports
- 关键设备节点清单
- 默认 camera stream 选择

#### timeout / polling 类

- `pollMs`
- worker stop timeout
- startup readiness timeout
- stereo finalize timeout
- HMI active timeout

#### feature flag 类

- 是否启用日志同步到磁盘
- 是否启用自动重启某些 worker
- 是否启用严格 validation

#### 调试/诊断类

- dry-run / mock worker 开关
- 更高日志等级
- 额外保留中间产物

### 21.2 变更时机策略

建议明确：

| 配置类别 | 启动前确定 | 运行时可变 | 影响 worker 重启 |
| --- | --- | --- | --- |
| 路径类 | 是 | 否 | 是 |
| 设备类 | 是 | 原则上否 | 是 |
| timeout / polling 类 | 否 | 可以部分支持 | 视具体项而定 |
| 运行模式类 | 大部分是 | 可部分支持 | 可能 |
| feature flag 类 | 视项而定 | 可部分支持 | 可能 |
| 调试/诊断类 | 大部分是 | 少量可支持 | 可能 |

### 21.3 与 `ConfigManager` 的对齐策略

建议分两步：

- Phase 1：
  - 先统一成 `RuntimeConfig`
  - 明确类别和默认值来源

- Phase 2：
  - 再评估是否正式接入 `pp_main` 的 `ConfigManager`
  - 对运行时可变项提供订阅和 override 能力

## 22. 新 runtime 和 worker 的通信策略

这是必须明确的边界问题。

### 22.1 Phase 1 通信建议

Phase 1 不强推新 IPC 协议，建议保留当前可工作的控制方式，并把语义写清楚：

- 启动：
  - 命令行参数
- 停止：
  - signal
- stereo 协调：
  - control/status file
- 成功/失败：
  - readiness 条件 + 退出码 + 约定文件

### 22.2 Phase 2 再评估显式 IPC

等控制面稳定后，再评估是否引入：

- `Publisher/Subscriber`
- service discovery
- 更明确的 runtime <-> worker 命令协议

不建议在 Phase 1 同时改控制面和 IPC 协议。

## 23. 测试与验收策略

这是本次文档必须补齐的核心部分。

### 23.1 Phase 1 最小验收集

至少应覆盖以下 case：

- runtime 最小启动成功
- start recording 成功路径
- stop recording 成功路径
- camera worker 启动失败触发回滚
- sensor worker 启动失败触发回滚
- recording 中 camera worker 异常退出触发 fault-stop
- recording 中 sensor worker 异常退出触发 fault-stop
- HMI 短按触发 start/stop
- HMI 长按触发 pre/post audio
- 双键长按触发 shutdown request
- 磁盘不可写触发硬件 fault
- 关键设备缺失触发硬件 fault
- stop 超时后升级 kill
- finalize 失败时写出诊断
- validation 失败时写出诊断
- 退出后无僵尸子进程残留

### 23.2 推荐测试分层

#### 单元测试

适合覆盖：

- `RecordingOrchestrator` 状态转移
- `HmiController` 的按钮去抖和长按判定
- 配置解析与分类
- `ProcessSupervisor` 的 stop 升级链

#### 组件测试

适合覆盖：

- mock `ProcessSupervisor` 下的录制编排
- mock HMI 事件输入
- mock stereo status/control 文件交互

#### 无硬件冒烟测试

建议通过：

- stub worker
- fake ready/status file
- 临时目录 episode 输出

来覆盖 start/stop/fault 路径。

### 23.3 验收标准

Phase 1 可视为完成的标准建议为：

- `record_runtime` 入口已从巨型控制对象变成 `RuntimeApp` 风格
- `ProcessSupervisor`、`RecordingOrchestrator`、`HmiController`、`HealthMonitor` 已独立
- `run_record.sh` 不再新增业务逻辑
- worker 异常退出和 stop 超时路径有自动化回归
- 仍可保持现有部署入口工作

## 24. 并入 `pp_main` 前的规范对齐清单

这是“主仓库视角”的补充清单。

### 24.1 命名和目录

- 顶层 standalone 目录名使用 `UpperCamelCase`
- 文件名逐步对齐 `lower_snake_case`
- 入口文件统一 `main.cc`

### 24.2 logger

- C++ 主链统一接 `DM_LOG_*`
- 子进程输出至少要能归属到 runtime 日志

### 24.3 配置

- 启动参数和默认值来源显式化
- 不再长期依赖相对路径硬编码

### 24.4 `utils` 依赖方向

- `utils` 只提供公共能力
- 不允许反向依赖 runtime 或 recorder 业务模块

### 24.5 shell 依赖

- 并仓后仍允许保留极薄 shell 兼容层
- 但不允许 shell 成为业务状态机承载层

### 24.6 standalone target 约定

每个 standalone target 至少应满足：

- 有自己的 `main.cc`
- 有清晰的 `Initialize/Stop`
- 可单独运行
- 可单独加单实例保护

### 24.7 头文件和实现边界

- 控制面接口放 `.h`
- 复杂流程实现放 `.cc`
- 避免继续把多类嵌套实现长期堆在单个巨型文件中

### 24.8 singleton / static

- 不强制全部消灭
- 但新 runtime 模块默认不引入不必要的 singleton/static
- 全局状态必须可解释其生命周期

### 24.9 错误码与日志字段

建议逐步统一：

- worker name
- pid
- episode dir
- state
- reason
- exit code

这样后续并仓和运维排障才有一致语义。

## 25. 函数级迁移表

本节把 `record_runtime.cpp` 的关键函数进一步下钻到“函数 -> 目标类/文件”的粒度，便于直接拆 PR。

### 25.1 顶层生命周期与主循环

| 当前函数 | 现状职责 | 目标类 | 目标文件建议 | 说明 |
| --- | --- | --- | --- | --- |
| `RecordRuntime::initialize()` | 组装顶层依赖，读环境、初始化 episode、panel、LED、音频、stereo | `RuntimeApp::Initialize()` | `runtime_app.cc` | 该函数应保留总装职责，但内部细节下沉到各组件 |
| `RecordRuntime::run()` | 监督主循环，轮询执行维护逻辑和按钮处理 | `RuntimeApp::Run()` | `runtime_app.cc` | Phase 1 仍可保留主循环，但周期任务应逐步迁入 scheduler |
| `RecordRuntime::requestStop()` | 设置 stop 请求 | `RuntimeApp::RequestStop()` | `runtime_app.cc` | 保持简单 |

### 25.2 录制编排

| 当前函数 | 现状职责 | 目标类 | 目标文件建议 | 说明 |
| --- | --- | --- | --- | --- |
| `RecordRuntime::startRecording(bool)` | 创建 episode、准备输出、并发拉起 worker、开始 stereo session、设置录制状态 | `RecordingOrchestrator::StartRecording()` | `recording_orchestrator.cc` | 当前是最核心的高风险函数之一 |
| `RecordRuntime::stopRecording(bool, const std::string&)` | stop stereo、stop worker、flush/finalize/merge/validate、设置反馈状态 | `RecordingOrchestrator::StopRecording()` | `recording_orchestrator.cc` | 建议拆成 stop、finalize、validate 3 个内部子阶段函数 |
| `RecordRuntime::mergeEpisodeInfo(...)` | stereo 信息与 episode 信息合并 | `EpisodeFinalizer::MergeEpisodeInfo()` 或 `RecordingOrchestrator` 私有辅助 | `episode_finalizer.cc` 或 `recording_orchestrator.cc` | 若 Phase 1 不想新增类，可先收在 orchestrator 私有方法中 |

### 25.3 按钮与 HMI

| 当前函数 | 现状职责 | 目标类 | 目标文件建议 | 说明 |
| --- | --- | --- | --- | --- |
| `RecordRuntime::handleButtons(const ButtonSnapshot&)` | 去抖、长按、双键、事件触发 | `HmiController::HandleButtons()` | `hmi_controller.cc` | 应改成“产生事件”，而不是直接触发业务函数 |
| `RecordRuntime::handleShortUpAction()` | 开始/停止录制 | `RuntimeApp::OnHmiEvent()` -> `RecordingOrchestrator` | `runtime_app.cc` | HMI 不直接做业务决策 |
| `RecordRuntime::handleShortDownAction()` | reset 录制或停止录制 | `RuntimeApp::OnHmiEvent()` -> `RecordingOrchestrator` | `runtime_app.cc` | 同上 |
| `RecordRuntime::handleLongUpAction()` | 触发 pre-audio | `RuntimeApp::OnHmiEvent()` -> `AudioCoordinator` | `runtime_app.cc` | 同上 |
| `RecordRuntime::handleLongDownAction()` | 触发 post-audio | `RuntimeApp::OnHmiEvent()` -> `AudioCoordinator` | `runtime_app.cc` | 同上 |
| `RecordRuntime::handleDualShutdownAction()` | 停录、写 shutdown request、请求退出 | `RuntimeApp::HandleShutdownRequest()` | `runtime_app.cc` | 仍允许保留 shutdown request file 兼容 |
| `RecordRuntime::setLedState(...)` | LED 状态下发 | `HmiController::SetLedState()` | `hmi_controller.cc` | 顶层只表达状态，不操作具体 LED 设备 |

### 25.4 音频

| 当前函数 | 现状职责 | 目标类 | 目标文件建议 | 说明 |
| --- | --- | --- | --- | --- |
| `RecordRuntime::startAudioPlayer()` | 启动音频播放器并等待 ready | `AudioCoordinator::StartAudioPlayer()` | `audio_coordinator.cc` | 内部通过 `ProcessSupervisor` 启动进程 |
| `RecordRuntime::stopAudioPlayer()` | 停止音频播放器 | `AudioCoordinator::StopAudioPlayer()` | `audio_coordinator.cc` | 同上 |
| `RecordRuntime::maintainAudioPlayer()` | 音频播放器保活与重启 | `AudioCoordinator::MaintainAudioPlayer()` | `audio_coordinator.cc` | 后续适合转成 scheduler 周期任务 |
| `RecordRuntime::sendAudioCommand(...)` | 发送音频控制命令 | `AudioCoordinator::SendCommand()` | `audio_coordinator.cc` | 统一收口音频控制协议 |
| `RecordRuntime::setAudioRecoveryCommand(...)` | 设置恢复命令语义 | `AudioCoordinator::SetRecoveryCommand()` | `audio_coordinator.cc` | Phase 1 可先保留现有语义 |
| `RecordRuntime::recordAudioClip(...)` | pre/post audio 录音和落盘 | `AudioCoordinator::RecordClip()` | `audio_coordinator.cc` | 注意不要让 orchestrator 再直接操作文件 |
| `RecordRuntime::attachPendingPreAudio(...)` | 将 pre-audio 绑定到 episode | `AudioCoordinator::AttachPendingPreAudio()` | `audio_coordinator.cc` | 保持音频职责闭合 |

### 25.5 stereo 会话

| 当前函数 | 现状职责 | 目标类 | 目标文件建议 | 说明 |
| --- | --- | --- | --- | --- |
| `RecordRuntime::startStereoDaemon()` | 启动 stereo daemon 并准备 control file | `StereoSessionClient::StartDaemon()` | `stereo_session_client.cc` | 内部通过 `ProcessSupervisor` 启动 |
| `RecordRuntime::stopStereoDaemon()` | 停止 daemon | `StereoSessionClient::StopDaemon()` | `stereo_session_client.cc` | 同上 |
| `RecordRuntime::maintainStereoDaemon()` | daemon 保活与恢复 | `StereoSessionClient::MaintainDaemon()` | `stereo_session_client.cc` | 适合 scheduler 周期任务 |
| `RecordRuntime::writeStereoControl(...)` | 写控制文件 | `StereoSessionClient::WriteControl()` | `stereo_session_client.cc` | 保持 Phase 1 控制协议不变 |
| `RecordRuntime::waitForStereoFinalize(...)` | 等待 stereo finalize 完成 | `StereoSessionClient::WaitForFinalize()` | `stereo_session_client.cc` | orchestrator 调用，但不直接实现 |

### 25.6 健康检查

| 当前函数 | 现状职责 | 目标类 | 目标文件建议 | 说明 |
| --- | --- | --- | --- | --- |
| `RecordRuntime::evaluateHardwareHealth()` | 生成 fault 结果 | `HealthMonitor::EvaluateHealth()` | `health_monitor.cc` | 纯检查逻辑应尽量无副作用 |
| `RecordRuntime::monitorHardwareHealth()` | 周期健康检查和状态反馈 | `HealthMonitor::Poll()` | `health_monitor.cc` | 输出健康事件或 fault 对象，不直接写业务状态机 |
| `RecordRuntime::checkRecorderProcesses()` | camera/sensor 存活检查 | `HealthMonitor::CheckWorkers()` 或 `ProcessSupervisor::QueryWorkers()` | `health_monitor.cc` / `process_supervisor.cc` | 倾向由 supervisor 提供状态，monitor 只消费 |

### 25.7 进程监督

| 当前函数 | 现状职责 | 目标类 | 目标文件建议 | 说明 |
| --- | --- | --- | --- | --- |
| `RecordRuntime::ProcessRunner::start(...)` | fork/exec 启动 | `SubprocessHandle::Start()` | `subprocess_handle.cc` | 作为 `ProcessSupervisor` 的内部基础类 |
| `RecordRuntime::ProcessRunner::isRunning()` | 查询是否仍在运行 | `SubprocessHandle::IsRunning()` | `subprocess_handle.cc` | 同上 |
| `RecordRuntime::ProcessRunner::wait(int)` | 等待退出 | `SubprocessHandle::Wait()` | `subprocess_handle.cc` | 同上 |
| `RecordRuntime::ProcessRunner::sendSignal(int)` | 发信号 | `SubprocessHandle::SendSignal()` | `subprocess_handle.cc` | 同上 |
| `RecordRuntime::ProcessRunner::stop(int)` | stop + timeout 升级 | `SubprocessHandle::Stop()` | `subprocess_handle.cc` | 升级策略不应散落业务层 |
| `RecordRuntime::ProcessRunner::pollExit(bool)` | 收割退出状态 | `SubprocessHandle::PollExit()` | `subprocess_handle.cc` | 同上 |

### 25.8 Episode 与文件治理

| 当前函数 | 现状职责 | 目标类 | 目标文件建议 | 说明 |
| --- | --- | --- | --- | --- |
| `EpisodeManager::initialize()` | 初始化 episode 根目录 | `EpisodeManager` 保持 | `episode_manager.cc` | Phase 1 不重写 |
| `EpisodeManager::createNextEpisodeDir()` | 创建 episode 目录 | `EpisodeManager` 保持 | `episode_manager.cc` | 由 orchestrator 调用 |
| `EpisodeManager::prepareEpisode(...)` | 准备 metadata 与输出目录 | `EpisodeManager` 保持 | `episode_manager.cc` | 由 orchestrator 调用 |
| `EpisodeManager::validateEpisode(...)` | 录制后校验 | `EpisodeManager` 保持 | `episode_manager.cc` | 由 orchestrator 调用 |
| `EpisodeManager::writeMetadata(...)` | metadata 写入 | `EpisodeManager` 保持 | `episode_manager.cc` | 暂不拆 |
| `EpisodeManager::writeFilteredCalibration(...)` | calibration 输出 | `EpisodeManager` 保持 | `episode_manager.cc` | 暂不拆 |
| `EpisodeManager::prepareEpisodeOutputs(...)` | 输出目录和文件准备 | `EpisodeManager` 保持 | `episode_manager.cc` | 暂不拆 |

### 25.9 通用 helper

| 当前函数 | 现状职责 | 目标归属 | 目标文件建议 | 说明 |
| --- | --- | --- | --- | --- |
| `readEnvValue(...)` | 从 env file 读键值 | `runtime/common` 或 `src/utils` | `env_utils.cc` | 若主仓已有等价能力，后续再并入主仓公共 utils |
| `fileExistsAndNotEmpty(...)` | 文件存在性检查 | `src/utils` | `file_utils.cc` | 若已有等价 helper 可直接复用 |
| `runCommandSync(...)` / `runCommandCapture(...)` | 一次性命令执行 | `src/utils` | `process_utils.cc` | 要和 `SubprocessHandle` 区分：一个是 helper，一个是长期监督对象 |

## 26. Phase 1 目标目录树

这一版不是最终并仓目录，而是 Phase 1 在 `ugripper` 仓内更可执行的落地形态。

```text
ugripper/
  src/
    record_runtime/
      include/
        runtime_app.h
        runtime_config.h
        recording_orchestrator.h
        process_supervisor.h
        subprocess_handle.h
        hmi_controller.h
        health_monitor.h
        audio_coordinator.h
        stereo_session_client.h
        episode_manager.h
        runtime_types.h
      src/
        main.cpp
        runtime_app.cpp
        runtime_config.cpp
        recording_orchestrator.cpp
        process_supervisor.cpp
        subprocess_handle.cpp
        hmi_controller.cpp
        health_monitor.cpp
        audio_coordinator.cpp
        stereo_session_client.cpp
        episode_manager.cpp
        runtime_types.cpp
    camera_recorder/
    sensor_recorder/
    gripper_hmi/
  docs/
  run_record.sh
  pack_script/
```

说明：

- Phase 1 不强制把 `.cpp` 改成 `.cc`
- Phase 1 不强制物理迁入 `standalone/`
- 但建议在 `record_runtime` 内部先把职责拆到多个文件

## 27. Phase 1 头文件草案

本节只给“最小接口草案”，用于指导拆分边界，不要求一次完全定型。

### 27.1 `runtime_types.h`

建议收口公共状态和事件类型：

```cpp
enum class RuntimeState {
  kIdle,
  kPreparing,
  kStartingWorkers,
  kRecording,
  kStopping,
  kFinalizing,
  kFaulted,
};

enum class WorkerName {
  kAudioPlayer,
  kAudioRecorder,
  kStereoDaemon,
  kCameraRecorder,
  kSensorRecorder,
};

struct HmiEvent {
  enum class Type {
    kStartRequested,
    kResetRecordingRequested,
    kStopRequested,
    kPreAudioRequested,
    kPostAudioRequested,
    kShutdownRequested,
  };
  Type type;
};

struct HealthFault {
  std::string key;
  std::string detail;
};
```

### 27.2 `runtime_config.h`

```cpp
class RuntimeConfig {
 public:
  bool LoadFromEnvFile(const std::string& env_file);
  bool ApplyDefaults();
  bool Validate(std::string* error);

  std::string camera_recorder_bin;
  std::string sensor_recorder_bin;
  std::string audio_play_script;
  std::string audio_record_script;
  std::string disk_root;
  std::string env_file;
  std::string stereo_control_file;
  std::string stereo_status_file;
  std::string shutdown_request_file;
  std::vector<std::string> gripper_ports;
  std::string camera_codec;
  int poll_ms = 20;
  int worker_stop_timeout_ms = 5000;
  int stereo_finalize_timeout_ms = 10000;
};
```

### 27.3 `subprocess_handle.h`

```cpp
class SubprocessHandle {
 public:
  bool Start(const std::vector<std::string>& argv);
  bool IsRunning();
  bool Wait(int timeout_ms);
  bool SendSignal(int signal_number);
  bool Stop(int timeout_ms);
  int LastExitCode() const;
  int Pid() const;

 private:
  bool PollExit(bool blocking);
};
```

### 27.4 `process_supervisor.h`

```cpp
struct ProcessSpec {
  std::string name;
  std::vector<std::string> argv;
  int stop_timeout_ms = 5000;
  enum class StopMode { kSigTermThenKill, kSigIntThenTermThenKill };
  StopMode stop_mode = StopMode::kSigTermThenKill;
  enum class RestartPolicy { kNever, kOnUnexpectedExit };
  RestartPolicy restart_policy = RestartPolicy::kNever;
};

struct ProcessStatus {
  bool running = false;
  bool ready = false;
  bool expected_exit = false;
  int pid = -1;
  int last_exit_code = 0;
};

class ProcessSupervisor {
 public:
  bool Start(WorkerName worker, const ProcessSpec& spec, std::string* error);
  bool Stop(WorkerName worker, const std::string& reason, std::string* error);
  ProcessStatus GetStatus(WorkerName worker) const;
  void PollUnexpectedExit();
};
```

### 27.5 `recording_orchestrator.h`

```cpp
class RecordingOrchestrator {
 public:
  bool StartRecording(bool reset_recording, std::string* error);
  bool StopRecording(bool due_to_error, const std::string& reason, std::string* error);
  RuntimeState state() const;
  std::string current_episode_dir() const;
  std::string last_episode_dir() const;

 private:
  bool PrepareEpisode(bool reset_recording, std::string* error);
  bool StartWorkers(std::string* error);
  bool StopWorkers(std::string* error);
  bool FinalizeEpisode(std::string* error);
  bool ValidateEpisode(std::string* error);
};
```

### 27.6 `hmi_controller.h`

```cpp
class HmiController {
 public:
  bool Initialize(const std::vector<std::string>& gripper_ports, std::string* error);
  void Shutdown();
  std::optional<HmiEvent> PollEvent(int timeout_ms);
  void SetLedState(int led_state, double progress = 0.0);
};
```

说明：

- Phase 1 可以先继续沿用 `LedState` 枚举
- 这里的 `int led_state` 只是示意，实际应使用显式类型

### 27.7 `health_monitor.h`

```cpp
class HealthMonitor {
 public:
  std::optional<HealthFault> Poll();
};
```

### 27.8 `audio_coordinator.h`

```cpp
class AudioCoordinator {
 public:
  bool StartAudioPlayer(std::string* error);
  void StopAudioPlayer();
  void MaintainAudioPlayer();
  void SendCommand(const std::string& command);
  void SetRecoveryCommand(const std::string& command);
  bool RecordClip(const std::string& audio_type, bool monitor_up_button, std::string* error);
  bool AttachPendingPreAudio(const std::string& episode_dir, std::string* error);
};
```

### 27.9 `stereo_session_client.h`

```cpp
class StereoSessionClient {
 public:
  bool StartDaemon(std::string* error);
  void StopDaemon();
  void MaintainDaemon();
  bool StartSession(const std::string& episode_dir, int64_t start_system_time_us, std::string* error);
  bool StopSession(const std::string& episode_dir, int64_t stop_system_time_us, std::string* error);
  bool WaitForFinalize(const std::string& episode_dir, int timeout_ms, std::string* error);
};
```

### 27.10 `runtime_app.h`

```cpp
class RuntimeApp {
 public:
  bool Initialize();
  int Run();
  void Stop();
  void RequestStop();

 private:
  void OnHmiEvent(const HmiEvent& event);
  void OnHealthFault(const HealthFault& fault);
  void PollWorkers();
};
```

## 28. 建议的拆分 PR 顺序

为了避免单个 PR 混入太多语义变化，建议按以下顺序拆。

### PR 1：抽 `SubprocessHandle` 和 `ProcessSupervisor`

目标：

- 从 `RecordRuntime::ProcessRunner` 中抽出基础子进程控制类
- 不改变业务行为
- 保持旧 `RecordRuntime` 仍可调用新 supervisor

验收：

- 现有 worker 启停行为不变
- stop 超时路径行为等价

### PR 2：抽 `AudioCoordinator` 和 `StereoSessionClient`

目标：

- 把音频和 stereo 相关流程从 `RecordRuntime` 中移走
- 保持控制协议不变

验收：

- 音频播放器 ready 检查不变
- stereo daemon 与 control/status file 交互不变

### PR 3：抽 `HmiController`

目标：

- 将按钮去抖、长按、双键逻辑与 LED 控制独立
- `RecordRuntime` 只消费 HMI 事件

验收：

- 所有按钮语义保持等价
- LED 行为保持等价

### PR 4：抽 `HealthMonitor`

目标：

- 将健康检查逻辑变为独立组件
- fault 由 monitor 产出，runtime 只消费

验收：

- 关键 fault 条件不变
- fault 触发下的 stop/提示路径不变

### PR 5：抽 `RecordingOrchestrator`

目标：

- 将 start/stop recording、finalize、validate 收口
- runtime 只负责事件分发

验收：

- happy path 与 worker fault path 全量回归

### PR 6：收成 `RuntimeApp`

目标：

- 将 `main + RecordRuntime` 整理成 standalone 风格
- 为后续并入 `pp_main/standalone` 做目录与生命周期准备

验收：

- 入口、退出、signal、stop 顺序可读且可测
