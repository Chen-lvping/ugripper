# PPMain And Ugripper Test-Oriented Refactor Reference

本文档用于回答 3 个问题，并作为后续完整重构 `ugripper`、补齐测试、再逐步并入 `pp_main` 的参考输入。

本文默认以当前工作树里的 `ugripper` 代码为基线，包括已经开始但尚未提交的重构内容。这里讨论的不是“理想状态下从零开始怎么做”，而是“基于当前这版代码继续往测试友好方向推进”。

核心问题是：

1. `pp_main/test` 的设计里，哪些模式值得复用
2. `ugripper` 当前为什么很难做出稳定单元测试和回归测试
3. 应该如何一边重构代码边建立测试体系，而不是最后再补测试

相关参考：

- `pp_main/test/CMakeLists.txt`
- `pp_main/test/src/*`
- `ugripper/src/*`
- `ugripper/test/README.md`
- `docs/ppmain-ugripper-camera-refactor-reference.md`
- `docs/ppmain-ugripper-process-thread-refactor-reference.md`
- `docs/ugripper-refactor-plan.md`
- `docs/standalone-merge-plan.md`

## 1. 先讲结论

结论可以先压缩成 9 句话：

- `pp_main` 最值得学的不是“用了 GTest”本身，而是“先有可链接的模块库，再有测试目标”。
- `ugripper` 当前最主要的问题不是“测试文件太少”，而是“很多逻辑被埋在可执行程序和硬件 I/O 流程里，没法稳定测试”。
- 后续重构必须把 `main()`、流程编排、纯业务逻辑、系统适配层拆开。
- `pp_main/test` 的镜像式目录结构值得复用：测试目录按模块对齐源码目录，而不是随意堆在一个目录里。
- `ugripper` 现有 `utils_smoke_test`、`gripper_hmi_test`、`test/scripts/*.sh` 更接近 smoke / 现场验证工具，不是完整测试体系。
- `ugripper` 不应直接照搬 `pp_main` 的 Conan、安装路径或全部构建细节，但应复用它的测试组织原则。
- 最稳的推进方式不是一次性“大重写”，而是按“先抽纯逻辑，再加单测，再收口应用壳层”的顺序分阶段推进。
- 当前代码并不是完全没有可利用的收口：`utils` 已经是库目标，`gripper_hmi` 已经是库加工具，`sensor_recorder` 也已经有 `SENSOR_COMMON_SOURCES` 这样的拆分痕迹，应在这些基础上继续推进。
- 并入 `pp_main` 之前，至少要把 `ugripper` 的测试体系收敛到“无硬件单测可跑、有限组件测试可跑、现场脚本保留但降级为 field test”。

换句话说：

- `pp_main` 提供的是“测试友好的模块化组织样板”
- `ugripper` 当前需要的是“先把代码改造成可测试的形态”

### 1.1 当前基线约定

后续执行时，本文统一采用下面的基线约定：

- 以当前 `ugripper` 工作树代码为准，而不是要求先回到干净提交状态。
- 已经开始的重构视为有效输入，不把“有未提交改动”当成阻塞条件。
- 方案优先利用现有收口成果，再继续往“库目标 + 测试目标”方向演进。
- 只有在发现同一模块存在明显互相冲突的中间态时，才需要单独停下来校准。

这意味着本文不是要求“先冻结，再开始”，而是要求“从当前版本继续有序收口”。

## 2. `pp_main/test` 里值得复用的设计

参考：

- `pp_main/test/CMakeLists.txt`
- `pp_main/test/src/CMakeLists.txt`
- `pp_main/test/src/utils/CMakeLists.txt`
- `pp_main/test/src/mathutils/CMakeLists.txt`
- `pp_main/test/src/communication/CMakeLists.txt`
- `pp_main/test/src/robotics/CMakeLists.txt`

这里最值得复用的是下面 6 点。

### 2.1 单独的测试工程入口

`pp_main` 把测试作为独立 CMake 入口维护：

- 顶层 `test/CMakeLists.txt`
- 显式 `enable_testing()`
- 统一依赖查找
- 再分发到 `test/src/*`

这带来的价值是：

- 测试不是某个模块顺手带的附属物
- 测试依赖可以单独管理
- 可以按模块增量扩展

对 `ugripper` 的启发是：

- 后续必须有独立的 `test/CMakeLists.txt`
- 顶层 CMake 需要提供 `BUILD_TESTING` 开关
- 测试构建和产品构建要显式关联，而不是靠手工可执行程序凑

### 2.2 测试目录镜像源码模块

`pp_main/test/src` 下面按模块分目录：

- `utils`
- `mathutils`
- `communication`
- `robotics`

这比“一个 tests 目录里堆所有测试”更好，因为：

- review 时更容易定位职责
- 模块拆分后测试也跟着移动
- 测试拥有清晰归属

对 `ugripper` 的启发是：

- `test/src/utils`
- `test/src/camera`
- `test/src/record_runtime`
- `test/src/sensor`
- `test/src/gripper_hmi`

应直接和重构后的模块边界对齐。

### 2.3 测试目标链接库，不链接应用壳

`pp_main` 的测试目标大多直接链接：

- `PP::utils`
- `PP::mathutils`
- `PP::communication`
- `PP::robotics`

而不是去拉起某个完整 standalone 程序。

这背后的关键不是“库化很漂亮”，而是：

- 测试对象边界稳定
- 依赖可控
- 硬件和进程副作用少

这正是 `ugripper` 当前最缺的能力。

### 2.4 测试资产和测试代码一起维护

`pp_main` 里有典型样板：

- `test/src/utils/test_yaml_parser.cc`
- `test/src/utils/test_yaml_parser/*.yaml`
- `test/src/robotics/test_robot_asset.cc`
- `test/src/robotics/test_robot_asset/*.urdf`

这说明它不是把配置样例、输入样例、资源文件散在仓库别处，而是把测试输入当作测试资产维护。

对 `ugripper` 的启发是：

- YAML 示例
- JSON 控制文件示例
- calibration 示例
- MCAP/二进制协议样例
- ffmpeg/gstreamer 日志样例

都应该按测试归属放在对应目录下，而不是直接复用运行时真配置。

### 2.5 每个测试目标都很小

`pp_main` 当前测试目标基本是：

- 一类模块
- 一个二进制
- 少量源文件

这意味着：

- 失败定位直接
- 构建代价较小
- 不会形成“一个超级测试程序”

这对 `ugripper` 同样重要。不要把所有测试都塞进一个 `ugripper_all_tests`。

### 2.6 复用原则，不照抄构建细节

`pp_main/test` 里也有一些不应机械照抄的部分，例如：

- Conan/toolchain 强绑定
- 当前 install 路径约定
- 某些测试还带明显的 sleep 和时序等待

因此 `ugripper` 应复用的是：

- 模块镜像
- 测试资产同目录
- 库目标可单测
- 测试工程独立入口

不必原样复制：

- 构建工具链配置
- 安装路径
- 当前 target 命名细节

## 3. `ugripper` 当前阻碍测试的核心问题

这里只讲会直接影响测试体系建设的问题。

### 3.1 当前基本没有正式测试拓扑

现状是：

- 顶层 `CMakeLists.txt` 没有 `enable_testing()`
- 没有独立 `test/CMakeLists.txt`
- 没有 `add_test(...)`
- 没有 GTest 目标

这意味着当前仓库还没有真正的“自动化测试入口”。

### 3.2 现有测试更像 smoke / tool

现有内容主要是：

- `src/utils/src/utils_smoke_test.cc`
- `src/gripper_hmi/test/gripper_hmi_test.cpp`
- `test/scripts/camera_test.sh`
- `test/scripts/camera_crash_capture.sh`
- `test/scripts/testVideoPipe.sh`

这些文件有价值，但定位不同：

- `utils_smoke_test` 是最小可运行校验
- `gripper_hmi_test` 更像串口交互工具
- `test/scripts/*.sh` 是现场验证脚本

问题不是它们“不好”，而是它们不能替代：

- 单元测试
- 组件测试
- 可重复回归测试

### 3.3 可执行程序承担了过多逻辑

当前主链里仍然以“可执行程序优先”为主，但已经出现了不均匀的局部收口：

- `src/camera_recorder`
- `src/record_runtime`
- `src/sensor_recorder`

同时，当前已有两处比较明确的正向信号：

- `src/utils` 已经是独立库目标
- `src/gripper_hmi` 已经是独立库目标，`gripper_hmi_test` 更像调试/工具程序

共同特征是：

- 大量逻辑直接存在于应用模块中
- 纯逻辑、状态机、配置解析、命令拼装、硬件访问没有明确分层
- 测试时很难只测其中一小块

因此当前的真实问题不是“完全没拆过”，而是“拆分还没有进入可测试体系”。结果就是：

- 想测配置解析，会顺手依赖进程模型
- 想测命令拼装，会顺手依赖设备配置和输出路径
- 想测 runtime 状态机，会顺手依赖音频、相机、传感器、文件系统

### 3.4 缺少明确的系统适配层

当前很多逻辑直接调用：

- 串口
- `/dev/*`
- 文件系统
- shell command
- `ffmpeg`
- `gst-launch-1.0`
- 时间函数
- 线程和 sleep

但没有统一的 adapter/interface 层。

这会导致：

- 单测无法 fake
- 组件测试只能真跑
- 小改动也容易引起全链副作用

### 3.5 配置语义分散，导致测试断点不稳定

当前和相机/episode 强相关的语义分散在：

- `config/camera_recorder.yaml`
- `src/camera_recorder`
- `src/record_runtime`
- 部分 shell / 音频 / calibration 逻辑

这意味着很多逻辑测试不是“给定输入 -> 断言输出”，而是“必须知道别处还有哪些常量”。

### 3.6 硬件协议代码和业务代码耦合太深

典型例子包括：

- `sensor_recorder` 中的 CRC、协议帧、时间平滑、MCAP 写入、串口重连
- `gripper_hmi` 中的协议编解码、LED effect、串口读写
- `camera_recorder` 中的 mode 选择、滤镜拼装、命令拼装、stereo 会话协调

这些逻辑中有相当一部分本来可以做纯逻辑测试，但目前仍和 I/O 紧耦合。

## 4. 重构目标不是“把测试补上”，而是“把代码改成可测试”

这部分是本文的核心。

建议把 `ugripper` 的后续重构目标写成下面四层。

### 4.1 App Shell 层

职责：

- 解析 CLI
- 初始化日志
- 组装依赖
- 安装 signal handler
- 启动和停止顶层对象

约束：

- 尽量薄
- 不承载业务规则
- 不承载协议细节

典型落点：

- `main.cpp`
- app 级 `Runner`

### 4.2 Orchestration 层

职责：

- 录制流程编排
- episode 生命周期
- supervisor / worker 关系
- 状态切换
- 故障聚合

约束：

- 只依赖抽象接口
- 不直接碰 `/dev/*`
- 不直接写死 shell 细节

### 4.3 Domain Logic 层

职责：

- 配置模型
- 协议模型
- 命令生成
- 状态机
- 路径/命名规则
- 校验规则
- 时间对齐/补偿算法

约束：

- 尽量纯逻辑
- 以值对象、纯函数、小类为主
- 这是单元测试的主战场

### 4.4 Adapter 层

职责：

- 串口访问
- 文件系统访问
- 子进程拉起
- 时间源
- ffmpeg/gstreamer 调用
- V4L2/UVC 设备访问

约束：

- 封装系统依赖
- 暴露最小接口
- 允许 fake/stub

## 5. 建议的模块重构方向

这里不要求第一步就改目录树，但要求先把逻辑边界拆出来。

### 5.1 `src/utils`

目标：

- 保留为基础公共库
- 只放通用轻量能力

当前现状：

- 已经有 `utils` 库目标
- 已有 `utils_smoke_test`

因此这里不建议先做大拆分，建议先把它变成第一批正式单测落点。

建议拆分为：

- `env_utils`
- `time_utils`
- `file_utils`
- 后续可补 `process_utils`、`temp_dir`

测试重点：

- 纯函数
- 边界条件
- 临时文件行为

### 5.2 `src/gripper_hmi`

建议从当前库中拆成 3 块逻辑：

- `hmi_protocol`
- `hmi_led_effects`
- `hmi_transport` 或 `hmi_driver`

当前现状：

- `gripper_hmi_protocol.h` 已经把协议类型和接口单独暴露出来
- `gripper_hmi_led_effects.h` 已经把 LED effect renderer 单独暴露出来
- `gripper_hmi` 已经是库目标

因此这里的第一步也不应该是“重写库结构”，而应该是：

- 先给 `protocol` 和 `led_effects` 加单测
- 再把 `driver` 中仍然带串口副作用的部分往 transport 层收口

测试重点：

- 协议编解码
- LED 状态映射
- 按键状态解释

说明：

- 当前的 `gripper_hmi_test.cpp` 建议降级为 `tool/smoke`，不再作为“主测试”承担责任。

### 5.3 `src/sensor_recorder`

建议逻辑拆成：

- `sensor_protocol`
  - CRC
  - 编码器/IMU 协议帧
- `sensor_domain`
  - 时间平滑
  - 样本结构
  - 队列/回压规则
- `sensor_io`
  - 串口驱动
  - MCAP writer adapter
- `sensor_app`
  - recorder / zeroing app 壳层

当前现状：

- `bsp_crc.cpp`
- `im648_CMD.cpp`
- `im648_driver.cpp`
- `encoder_driver.cpp`

已经通过 `SENSOR_COMMON_SOURCES` 被复用到 `sensor_recorder` 和 `zeroing`，说明这里已经具备继续抽库的现实基础。

因此更贴近现状的推进方式是：

- 先把 `bsp_crc` 做成第一批正式单测对象
- 再把 `SENSOR_COMMON_SOURCES` 收敛成 `sensor_core` 一类的库目标
- 最后再把 recorder/zeroing 两个 app 壳层进一步变薄

测试重点：

- CRC 正确性
- 编码器状态解析
- IMU 样本解析
- timestamp smoothing
- backlog / reconnect 策略

### 5.4 `src/camera_recorder`

这是后续最需要拆的模块。

建议至少拆成：

- `camera_types`
  - 模式枚举
  - 配置对象
  - 错误类型
- `camera_config`
  - YAML 解析
  - 默认值
  - 校验规则
- `camera_command_builder`
  - ffmpeg/gstreamer 参数拼装
  - filter 构建
  - 输出文件命名
- `camera_runtime`
  - recorder 生命周期
  - 监控状态
- `stereo_session`
  - warmup / session 协调
  - control/status JSON
- `camera_device_adapter`
  - UVC/V4L2/libusb 访问

当前现状：

- `main.cpp` 已经比较薄
- `camera_recorder.cpp` 仍然承载了配置解析、命令构造、录制生命周期和 stereo 协调

因此这里最合适的切入点不是先碰设备采集实现，而是先抽：

- `camera_types`
- `camera_config`
- `camera_command_builder`

先把最稳定、最容易测的部分从巨型实现里拿出来。

测试重点：

- mode 解析
- YAML 合法/非法输入
- 命令拼装
- filter 构造
- stereo control/status 文件读写
- session 状态转换

说明：

- 这里的测试核心不是“真的录一段视频”，而是先把 80% 的纯逻辑从硬件行为里分离出来。

### 5.5 `src/record_runtime`

建议从当前顶层流程中拆出：

- `runtime_types`
  - fault / health / state / action
- `runtime_button_logic`
  - 短按/长按/双键逻辑
- `runtime_episode`
  - 目录生成
  - metadata/calibration 写入规则
- `runtime_health`
  - 故障聚合
  - fault 优先级
- `runtime_process`
  - 子进程拉起和停止策略
- `runtime_audio`
  - 音频播放/录制流程编排
- `runtime_app`
  - 顶层 `RecordRuntime`

当前现状：

- `main.cpp` 已经是薄入口
- `record_runtime.h` 中已经显式暴露了 `RecordRuntime` 和若干嵌套类型
- 真正的问题集中在 `record_runtime.cpp` 里仍然把很多流程逻辑堆在一起

因此这里更贴近现状的拆分方式是：

- 先把 header/cpp 里的嵌套状态机和纯逻辑类往独立文件移动
- 不先动最外层 app 接线方式
- 先把 button / episode / health / process policy 抽出来上测试

测试重点：

- 按键状态机
- 健康检查到 LED state 的映射
- episode 命名和 metadata 生成
- calibration 过滤逻辑
- 子进程参数拼装
- stop 顺序和故障传播

## 6. 建议建立的测试目录结构

参考 `pp_main/test/src` 的组织方式，`ugripper` 建议逐步收敛到：

```text
test/
  CMakeLists.txt
  src/
    CMakeLists.txt
    utils/
      CMakeLists.txt
      test_env_utils.cc
      test_file_utils.cc
      test_time_utils.cc
      env_samples/
        basic.env
        malformed.env
    gripper_hmi/
      CMakeLists.txt
      test_hmi_protocol.cc
      test_hmi_led_effects.cc
      protocol_samples/
        frame_ok.bin
        frame_bad_crc.bin
    sensor/
      CMakeLists.txt
      test_bsp_crc.cc
      test_encoder_protocol.cc
      test_imu_batch_timestamp.cc
      sensor_samples/
        encoder_frames.bin
        imu_packets.bin
    camera/
      CMakeLists.txt
      test_camera_config.cc
      test_camera_command_builder.cc
      test_stereo_control_json.cc
      test_camera_session_policy.cc
      config_samples/
        valid_camera.yaml
        invalid_missing_device.yaml
      logs/
        ffmpeg_debug_ts.log
        gst_identity.log
    record_runtime/
      CMakeLists.txt
      test_button_logic.cc
      test_episode_manager.cc
      test_runtime_health.cc
      test_process_policy.cc
      calibration_samples/
        fake_cam_calib.json
        expected_episode_metadata.json
  support/
    temp_dir.h
    fake_clock.h
    fake_process_runner.h
    fake_file_system.h
  field/
    camera_test.sh
    camera_crash_capture.sh
    test_video_pipe.sh
```

这里要强调两个边界：

- `test/src/*` 是自动化测试
- `test/field/*` 是现场验证脚本

不要再把两者混成“都叫 test，所以都算测试体系”。

### 6.1 `test/support` 的角色

`test/support/*` 建议明确为共享测试基础设施层。

定位：

- 只服务测试
- 不进入产品库
- 不作为运行时依赖

建议优先放在这里的能力：

- `fake_clock.h`
- `fake_process_runner.h`
- `fake_file_system.h`
- `temp_dir.h`
- 后续可补 `fake_command_runner.h`
- 后续可补 `fake_device_registry.h`

优先依赖这些 fake 的模块：

- `record_runtime` 组件测试
- `camera_recorder` 组件测试
- 部分 `sensor_recorder` 无硬件测试

约束：

- 一个 fake 只解决一类系统依赖，不要在模块内部各自再造一套假的 clock/process/file system
- 产品代码如果需要配合测试，应通过接口注入，而不是让测试直接改生产逻辑分支
- `test/support` 可以被多个测试 target 复用，但不应反向进入 `src/*`

演进方式建议：

- Phase A1/A2 先以轻量头文件工具或少量小型实现为主
- 若复用明显增多，再收敛为单独的 `test_support` target
- 是否做成 header-only 或独立 target，不必前置写死，按复用规模决定

## 7. 推荐的测试分层

后续建议把测试明确分成 4 层。

### 7.1 Unit Test

目标：

- 无硬件
- 无真实子进程
- 无真实 `/dev/*`

典型对象：

- 配置解析
- 状态机
- 协议编解码
- 命令构建
- 路径/命名
- 校验逻辑

这是最先要补齐的一层。

### 7.2 Component Test

目标：

- 允许临时目录
- 允许 fake process runner
- 允许 fake clock
- 不依赖真实硬件

边界约束：

- 默认允许临时目录、fake adapter、短时同步等待
- 默认不允许真实 shell command
- 默认不允许真实 `/dev/*`
- 默认不允许真实网络
- 默认不允许真实长期 sleep 或依赖秒级等待稳定

典型对象：

- `RecordRuntime` 的 stop/start 顺序
- `camera_recorder` 的 control/status 文件交互
- episode 目录输出物校验
- 多模块协作但仍在单机可重复范围内

### 7.3 Hardware Test

目标：

- 依赖真实设备
- 只在指定环境运行

典型对象：

- 真实串口收发
- 真实 `/dev/video*`
- 真实 UVC 控制

说明：

- 这类测试不能作为默认 CI 门禁
- 应单独打标签，例如 `hardware`

### 7.4 Field Test

目标：

- 面向现场验证和问题抓取

现有脚本就属于这一层：

- `camera_test.sh`
- `camera_crash_capture.sh`
- `testVideoPipe.sh`

说明：

- 保留，但从“主测试体系”中降级
- 作为 release 前、板端排障、售后分析工具存在
- 不进入默认 CTest 主测试集
- 不进入默认 CI 门禁
- 建议单独保留 README 或执行入口说明

### 7.5 本地与 CI 运行矩阵

建议至少固定下面这个执行矩阵：

| 测试层级 | 本地默认 | CI 默认 | 专用环境 | 门禁级别 |
| --- | --- | --- | --- | --- |
| Unit | 是 | 是 | 否 | 必须 |
| Component | 是 | 是/部分 | 否 | 稳定后纳入 |
| Hardware | 否 | 否 | 是 | 非默认 |
| Field | 否 | 否 | 是 | 人工触发 |

标签建议：

- `unit`
- `component`
- `hardware`
- `field`

执行建议：

- 本地开发默认至少跑 `unit + component`
- 默认 CI 门禁至少跑 `unit`，条件允许时再纳入稳定的 `component`
- `hardware` 和 `field` 只在专用 runner、板端或人工触发场景执行
- `field` 脚本应有单独入口，不混入 `ctest --output-on-failure` 主路径

### 7.6 组件测试里的 fake 组合示例

下面这些例子用于说明组件测试应长什么样：

- `test_process_policy`
  - `fake_process_runner` + `fake_clock`
- `test_camera_session_policy`
  - `fake_file_system` + `fake_process_runner`
- `test_episode_manager`
  - `temp_dir` + `fake_file_system`
- `test_stereo_control_json`
  - `temp_dir` + `fake_file_system`

这些例子的目的不是固定唯一实现，而是强调：

- 组件测试依赖 fake adapter 组合
- 不直接碰真实进程、真实设备和真实外部环境

## 8. 按模块给出优先级测试清单

下面这份清单可以直接作为补测 backlog。

### 8.1 P0：第一批必须补

`utils`

- `ReadEnvValue` 的正常路径、引号、空值、重复 key、缺失 key
- `FileExistsAndNotEmpty` 的空文件和不存在文件
- 时间工具返回单调非零值

`camera`

- camera YAML 解析
- `mode` 字符串到枚举映射
- 默认值和非法配置校验
- ffmpeg/gstreamer 命令拼装
- 输出文件列表和会话命名规则

`record_runtime`

- 按键短按/长按/双键逻辑
- episode 目录和 metadata 生成
- health fault 到 LED state 的映射
- 进程参数拼装和 stop 顺序

`sensor`

- CRC
- 编码器协议帧解析
- IMU 批量时间平滑

`gripper_hmi`

- 协议编码/解码
- LED effect 生成逻辑

### 8.2 P1：第二批建议补

`camera`

- stereo control/status JSON 兼容性
- session 状态转换
- timing 元信息解析

`record_runtime`

- calibration 过滤和占位补齐
- 故障优先级排序
- reset recording 和普通 recording 路径差异

`sensor`

- reconnect/backoff 策略
- backlog 警戒行为
- writer queue 边界条件

### 8.3 P2：后续再补

- 硬件相关集成测试
- 真实设备长时稳定性回归
- 板端录像、音频、传感器联合回归

### 8.4 现有文件到测试目标映射表

下面这张表更适合作为排 PR 和拆实施任务时的直接输入。

| 现有文件/类 | 当前问题 | 先抽出的测试对象 | 对应测试目标 | Phase |
| --- | --- | --- | --- | --- |
| `src/utils/src/env_utils.cc` | 已有库目标，但只有 smoke 校验 | `ReadEnvValue` 及 env 解析边界 | `test_env_utils` | A1 |
| `src/utils/src/time_utils.cc` | 已有库目标，但没有正式断言 | 时间工具纯函数与非零/单调边界 | `test_time_utils` | A1 |
| `src/utils/include/utils/file_utils.hpp` | 文件工具复用价值高，但无正式回归 | 文件存在性、空文件、临时目录行为 | `test_file_utils` | A1 |
| `src/gripper_hmi/include/gripper_hmi_protocol.h` + `src/gripper_hmi/src/gripper_hmi_protocol.cpp` | 协议逻辑已独立，但仍未进入正式测试 | 协议编解码、XOR、状态码描述 | `test_hmi_protocol` | A1 |
| `src/gripper_hmi/include/gripper_hmi_led_effects.h` + `src/gripper_hmi/src/gripper_hmi_led_effects.cpp` | 纯逻辑接口清晰，但仍无自动化回归 | 状态解析、颜色渲染、边界态 | `test_hmi_led_effects` | A1 |
| `src/gripper_hmi/test/gripper_hmi_test.cpp` | 更像串口交互工具，不适合作为主测试 | 保留为 smoke/tool，不再承载正式回归 | `gripper_hmi_test` | A1 |
| `src/sensor_recorder/src/bsp_crc.cpp` | 纯算法逻辑，最容易先落测试 | CRC8/CRC16 校验 | `test_bsp_crc` | A2 |
| `src/sensor_recorder/src/encoder_driver.cpp` | 协议、串口、副作用混合 | 编码器协议解析和状态更新逻辑 | `test_encoder_protocol` | A2 |
| `src/sensor_recorder/src/im648_CMD.cpp` + `src/sensor_recorder/src/im648_driver.cpp` | 解析、驱动、副作用混合 | IMU 包解析和时间平滑输入 | `test_imu_batch_timestamp` / `test_imu_protocol` | A2 |
| `src/sensor_recorder/src/main.cpp` | app 壳层和写入流程仍耦合 | 先抽 writer queue / batch smoothing / process policy，再建测试 | `test_sensor_write_queue` / `test_imu_batch_timestamp` | A2 |
| `src/camera_recorder/src/camera_recorder.cpp` | 巨型实现同时承载配置、命令、状态、stereo 协调 | `camera_types` | `test_camera_types` | A3 |
| `src/camera_recorder/src/camera_recorder.cpp` | YAML 解析和默认值规则埋在实现里 | `camera_config` | `test_camera_config` | A3 |
| `src/camera_recorder/src/camera_recorder.cpp` | ffmpeg/gstreamer 命令构造难回归 | `camera_command_builder` | `test_camera_command_builder` | A3 |
| `src/camera_recorder/src/camera_recorder.cpp` | stereo control/status 规则和会话策略混在 runtime 内 | `stereo_control_json` / `camera_session_policy` | `test_stereo_control_json` / `test_camera_session_policy` | A3 |
| `src/camera_recorder/src/main.cpp` | 已经是薄入口 | 保持 app shell，不作为优先单测对象 | 不单独建 | A3 |
| `src/record_runtime/include/record_runtime.h` + `src/record_runtime/src/record_runtime.cpp` | button 规则和流程耦合在 runtime 内 | `runtime_button_logic` | `test_button_logic` | A4 |
| `src/record_runtime/include/record_runtime.h` + `src/record_runtime/src/record_runtime.cpp` | episode 目录和 metadata 规则容易回归破坏 | `runtime_episode` | `test_episode_manager` | A4 |
| `src/record_runtime/include/record_runtime.h` + `src/record_runtime/src/record_runtime.cpp` | health/fault/LED 映射耦合在顶层流程里 | `runtime_health` | `test_runtime_health` | A4 |
| `src/record_runtime/include/record_runtime.h` + `src/record_runtime/src/record_runtime.cpp` | 子进程拉起与 stop 顺序需要稳定回归 | `runtime_process_policy` | `test_process_policy` | A4 |
| `test/scripts/camera_test.sh` | 现场脚本，不适合主测试体系 | 保留为 field test | 不进默认 CTest | 持续保留 |
| `test/scripts/camera_crash_capture.sh` | 故障抓取脚本，不适合主测试体系 | 保留为 field test | 不进默认 CTest | 持续保留 |
| `test/scripts/testVideoPipe.sh` | 能力探测脚本，不适合主测试体系 | 保留为 field test | 不进默认 CTest | 持续保留 |

## 9. 建议的 CMake 改造策略

这里同样建议“参考 `pp_main`，但不照抄”。

### 9.1 顶层增加测试开关

建议：

- 顶层 `option(BUILD_TESTING "Build tests" ON)`
- `include(CTest)`
- `if(BUILD_TESTING) add_subdirectory(test) endif()`

关于 GTest 的接入方式，本文不强制写死：

- 可以是系统依赖
- 可以是 vendor
- 也可以接入现有工具链

这里真正要求先固定的是：

- 测试目标如何组织
- 模块边界如何收口
- 测试链接哪些库目标

### 9.2 先把应用依赖改成库 + 壳层

在 `ugripper` 里，后续不应继续只有：

- `camera_recorder` executable
- `sensor_recorder` executable
- `record_runtime` executable

而应逐步收敛为：

- `ugripper_camera_core`
- `ugripper_sensor_core`
- `ugripper_runtime_core`
- 对应薄 `main` 可执行程序

测试链接核心库，不链接薄壳。

但执行顺序应按当前代码现状来：

- `utils`、`gripper_hmi` 直接开始补正式测试
- `sensor_recorder` 先从公共源文件收敛出库目标
- `camera_recorder`、`record_runtime` 先抽纯逻辑子模块，再形成核心库

### 9.2.1 目标归属示意

建议逐步形成下面这种 target 归属关系：

| 产品 target | 角色 | 对应测试 target |
| --- | --- | --- |
| `ugripper_utils` | 公共轻量工具库 | `test_env_utils`, `test_file_utils`, `test_time_utils` |
| `ugripper_hmi_protocol` | HMI 协议和纯逻辑库 | `test_hmi_protocol` |
| `ugripper_hmi_effects` | HMI LED effect 纯逻辑库 | `test_hmi_led_effects` |
| `ugripper_hmi` | HMI 聚合库/运行时库 | 组件测试可按需链接，不作为第一批主测入口 |
| `ugripper_sensor_protocol` | CRC、协议帧、纯解析逻辑 | `test_bsp_crc`, `test_encoder_protocol`, `test_imu_protocol` |
| `ugripper_sensor_core` | sensor 领域逻辑与无硬件核心库 | `test_imu_batch_timestamp`, `test_sensor_write_queue` |
| `ugripper_camera_types` | camera 类型与枚举库 | `test_camera_types` |
| `ugripper_camera_config` | camera YAML 解析和校验库 | `test_camera_config` |
| `ugripper_camera_command_builder` | camera 命令拼装库 | `test_camera_command_builder` |
| `ugripper_camera_runtime` | camera 运行时核心库 | `test_stereo_control_json`, `test_camera_session_policy` |
| `ugripper_runtime_logic` | runtime 纯逻辑与状态机库 | `test_button_logic`, `test_episode_manager`, `test_runtime_health`, `test_process_policy` |
| `camera_recorder` | app shell / executable | 不作为第一批单测直接链接对象 |
| `sensor_recorder` | app shell / executable | 不作为第一批单测直接链接对象 |
| `record_runtime` | app shell / executable | 不作为第一批单测直接链接对象 |

这张示意图的核心目的只有一个：

- 测试默认链接 core library
- 不默认链接 app 壳层

### 9.3 测试目标命名建议

建议对齐 `pp_main` 的扫描体验：

- `test_env_utils`
- `test_camera_config`
- `test_camera_command_builder`
- `test_runtime_button_logic`
- `test_episode_manager`
- `test_bsp_crc`

不要用：

- `all_tests`
- `misc_test`
- `tmp_test`

### 9.4 现场脚本不要混进单测构建

建议把：

- `test/scripts/*`

重命名或迁移为：

- `test/field/*`

并在文档里单独说明运行方法。

建议补充规则：

- `test/field/*` 保留在仓库中
- 不加入默认 `ctest`
- 不加入默认 CI job
- 需要单独 README 或 `make field-test` 一类的显式入口

## 10. 推荐执行顺序

下面这个顺序更适合真实落地。

### 10.1 Phase 与测试目标对齐表

这张表用于把 Phase 和第 8.4 节的映射表直接对齐，便于拆 PR。

| Phase | 重点范围 | 对应测试目标 |
| --- | --- | --- |
| A1 | `utils`、`gripper_hmi` | `test_env_utils`, `test_file_utils`, `test_time_utils`, `test_hmi_protocol`, `test_hmi_led_effects` |
| A2 | `sensor_recorder` 公共逻辑 | `test_bsp_crc`, `test_encoder_protocol`, `test_imu_protocol`, `test_imu_batch_timestamp`, `test_sensor_write_queue` |
| A3 | `camera_recorder` 纯逻辑 | `test_camera_types`, `test_camera_config`, `test_camera_command_builder`, `test_stereo_control_json`, `test_camera_session_policy` |
| A4 | `record_runtime` 纯逻辑 | `test_button_logic`, `test_episode_manager`, `test_runtime_health`, `test_process_policy` |
| A5 | `camera` / `runtime` 组件测试与并仓映射 | 稳定的 `component` 测试集，以及并仓后的测试迁移方案 |

### Phase A0：采用当前工作树为基线

目标：

- 明确后续所有测试与重构都基于当前这版代码推进
- 不把未提交重构当成前置阻塞
- 先把“已经收口到哪一步了”写清楚

动作：

- 建立测试方案文档
- 记录当前主链 smoke 方法
- 记录当前已存在的可利用收口：
  - `utils` 库
  - `gripper_hmi` 库
  - `sensor_recorder` 公共源文件
  - `camera_recorder` / `record_runtime` 薄 `main.cpp`

说明：

- 这一阶段不要求清理工作树，只要求后续方案都以当前代码事实为准。

### Phase A1：先让现有库目标进入正式测试

目标：

- 建立 `test/CMakeLists.txt`
- 引入 GTest
- 先补最稳定、最无争议、且已经有库边界的单测

动作：

- `utils_smoke_test` 保留
- `gripper_hmi_test` 保留为 tool/smoke
- 同时新增真正的：
  - `test/src/utils/*`
  - `test/src/gripper_hmi/*`

完成标准：

- 本地可一键构建并运行第一批正式单测
- `utils` 与 `gripper_hmi` 不再只依赖 smoke 程序做回归

### Phase A2：把 `sensor_recorder` 的公共逻辑从“复用源文件”提升为“可测试模块”

目标：

- 把 `SENSOR_COMMON_SOURCES` 对应逻辑提升为正式可测模块

动作：

- 先补 `bsp_crc` 测试
- 把协议、时间平滑、队列规则从 app 路径里剥出
- 逐步形成 `sensor_core`

完成标准：

- `sensor_recorder` 和 `zeroing` 不再只靠共享 `.cpp` 文件复用逻辑
- `sensor` 模块开始拥有正式单测入口

### Phase A3：拆 `camera_recorder` 的纯逻辑

目标：

- 从大应用中抽出 `camera_types/config/command_builder`

动作：

- 先不动真实采集实现
- 先把 YAML、mode、命令拼装、filter、session policy 独立出来

完成标准：

- 相机配置和命令拼装具备无硬件测试

### Phase A4：拆 `record_runtime` 的状态机和 episode 逻辑

目标：

- 把最容易回归、最值得测试的流程逻辑从应用壳层剥离

动作：

- 独立 button logic
- 独立 health/fault
- 独立 episode metadata/calibration 处理

完成标准：

- 录制主状态机的核心规则可单测

### Phase A5：补齐 `camera` / `runtime` 组件测试，并收敛并仓映射

目标：

- 让后续并入 `pp_main` 时，测试与模块边界一起迁移

动作：

- 建立 fake process runner / fake clock
- 增加无硬件组件测试
- 明确哪些模块进入 `pp_main/src`，哪些保持 standalone app 私有

完成标准：

- 并仓不是“只搬代码”，而是“连测试资产一起搬”

## 11. 并入 `pp_main` 时的建议落位

这里建议分两类处理。

### 11.1 可复用公共库

适合后续进入 `pp_main/src/...` 或与其公共库风格对齐的内容：

- 轻量 `utils`
- 纯协议模型
- 与业务无关的文件/时间/进程 helper
- 明确通用的配置和状态机工具

对应测试可对齐到：

- `pp_main/test/src/utils`
- 或新增同级模块测试目录

### 11.2 仍带明显 standalone 语义的模块

例如：

- `UgripperRuntime`
- `CameraRecorder`
- `SensorRecorder`

更适合保持 app 私有结构，测试也应和 app 一起迁移，不必硬塞进 `pp_main/test/src/utils` 这种公共测试域。

更务实的方式是：

- 公共能力进入 `pp_main/src`
- app 私有逻辑留在 `pp_main/standalone/<AppName>/`
- app 级测试跟随 app

## 12. 这份方案的验收标准

如果后续按本文推进，最终至少应满足下面 10 条：

1. `ugripper` 有独立测试入口，不再只有 smoke 工具。
2. 单元测试、组件测试、硬件测试、现场测试边界明确。
3. `camera_recorder` 的配置和命令拼装不再需要依赖真设备才能回归。
4. `record_runtime` 的按键逻辑、health 逻辑、episode 逻辑可自动化测试。
5. `sensor_recorder` 的 CRC、协议和时间平滑逻辑可自动化测试。
6. `gripper_hmi` 的协议和 LED effect 逻辑可自动化测试。
7. 测试资产和测试代码按模块归档，不再散落。
8. 主应用只做壳层组装，不再长期承载大量纯业务逻辑。
9. 并入 `pp_main` 时，模块和测试可以一起迁移。
10. 后续新增设备、模式或录制规则时，优先补测试而不是继续堆大文件。

## 13. 最终建议

这件事不要理解成“先重构，再顺便补几个测试”。

更准确的推进方式应该是：

- 先用 `pp_main/test` 的模式建立测试组织骨架
- 再反向推动 `ugripper` 代码拆成可测试模块
- 每拆出一块纯逻辑，就立刻把测试补上

只有这样，`ugripper` 后续完整重构和并入 `pp_main` 时，测试才会成为迁移资产，而不是迁移阻力。
