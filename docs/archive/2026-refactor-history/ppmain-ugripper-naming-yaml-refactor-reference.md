# PPMain And Ugripper Naming/YAML Refactor Reference

本文档用于回答一个更具体的问题：

- 如果后续要把 `ugripper` 完整重构后并入 `pp_main`，命名规范和 YAML 规范应该先怎么对齐

它不是“立刻改哪些文件”的逐文件 patch 清单，而是：

- 基于当前 `ugripper` 工作树现状给出问题分解
- 参照当前 `pp_main` 已经稳定存在的命名和 YAML 使用方式
- 形成一份适合放进 `docs/`、供完整重构计划引用的执行基线

相关参考：

- `pp_main/src/utils/yaml_parser.h`
- `pp_main/src/utils/config_manager.h`
- `pp_main/standalone/MessageBridge/config/MessageBridge.yaml`
- `pp_main/standalone/Puppeteer/config/Puppeteer.yaml`
- `pp_main/standalone/Puppetry/config/Puppetry.yaml`
- `pp_main/test/src/utils/test_yaml_parser.cc`
- `ugripper/config/camera_recorder.yaml`
- `ugripper/src/camera_recorder/*`
- `ugripper/src/record_runtime/*`
- `ugripper/src/sensor_recorder/*`
- `ugripper/src/gripper_hmi/*`
- `docs/standalone-merge-plan.md`
- `docs/ugripper-refactor-plan.md`
- `docs/ppmain-ugripper-camera-refactor-reference.md`
- `docs/ppmain-ugripper-test-refactor-reference.md`

## 1. 先讲结论

结论先压缩成 10 句话：

- `pp_main` 值得复用的不是某个单独类名，而是“目录、文件、类型、配置入口”四层一致的规范感。
- `ugripper` 当前最大的问题不是“个别名字不好看”，而是“命名、目录真源、配置边界”没有收口成一套口径。
- 后续完整重构必须先定义“唯一真源目录”，否则重命名和并仓都会被旧目录、构建产物、临时目录干扰。
- `pp_main` 当前的稳定特征是：目录和文件大多使用 `snake_case`，类型名使用 `UpperCamelCase`，配置文件用“应用名作为 YAML 顶层根”。
- `ugripper` 当前存在明显的混用：`snake_case`、`camelCase`、`UpperCamelCase`、裸全局符号、带下划线/不带下划线的接口同时存在。
- `ugripper` 当前的 YAML 主要集中在 `config/camera_recorder.yaml`，但很多实际配置语义仍散落在 C++ 常量、JSON 临时文件和脚本参数中。
- `camera_recorder.yaml` 当前并不是“字段太少”，而是“身份信息、设备信息、录制策略、输出语义、调优参数”挤在同一层。
- 直接做全仓大规模 rename 风险很高；正确顺序是“先抽 schema 和边界，再收口命名，再做迁移”。
- 并入 `pp_main` 前，至少要让 `ugripper` 达到“目录真源单一、模块命名统一、YAML 有 schema、测试里有独立 YAML 资产”。
- 因此，后续重构应把“命名规范”和“YAML 规范”当成架构任务，而不是当成代码美化任务。

## 2. `pp_main` 当前可复用的规范基线

这里不讲理想规则，只讲当前仓库里已经能看到的稳定模式。

### 2.1 目录与文件命名

从 `pp_main/src/*` 可见的稳定模式：

- 模块目录多数使用 `lower_snake_case`
- 文件名多数使用 `lower_snake_case`
- 入口 app 的配置文件名和 app 名语义一致，例如 `MessageBridge.yaml`、`Puppeteer.yaml`、`Puppetry.yaml`

这意味着 `pp_main` 的核心习惯是：

- 目录表达模块职责
- 文件表达具体对象或实现
- 配置文件表达应用入口

### 2.2 C++ 标识符命名

从 `pp_main/src/utils/yaml_parser.h`、`pp_main/src/utils/config_manager.h` 可见：

- 类型名使用 `UpperCamelCase`
- 常量使用 `kCamelCase`
- 成员变量使用尾随下划线
- 公共方法更接近 `Load/Get/Initialize/Set/Subscribe` 这类 `UpperCamelCase` 动词接口

这不是说仓库里每个文件都百分百绝对一致，而是说：

- `pp_main` 已经形成了可复用的“公共接口命名口径”

### 2.3 YAML 入口和结构

从 `MessageBridge.yaml`、`Puppeteer.yaml`、`Puppetry.yaml` 可见 3 个稳定特征：

1. YAML 顶层使用应用名作为根键
2. 根键下按子域分组，例如 `network`、`ros_topics`、`frame_ids`、`communication`、`run`
3. 配置键名大多使用 `lower_snake_case`

这套方式的价值是：

- 配置归属清晰
- 程序和配置文件一一对应
- 后续增加 override、热更新、测试样例时边界更稳定

### 2.4 测试里的 YAML 资产组织

`pp_main/test/src/utils/test_yaml_parser.cc` 和同目录 YAML 样例说明：

- YAML 不只是运行时文件，也应该是测试资产
- 合法/非法输入要能单独存放、单独回归
- 配置解析行为应该可以脱离主程序流程独立验证

这对 `ugripper` 尤其重要，因为它后续并仓前必须先把配置读取从“埋在流程代码里”变成“可测试的库行为”。

## 3. `ugripper` 当前在命名和 YAML 上的核心问题

### 3.1 目录真源不唯一

当前工作树里同时存在：

- `ugripper/src/*`
- `ugripper/config/*`
- `ugripper/docs/*`
- `ugripper/ugripper/*`（对应当前仓库根目录下的 `./ugripper/` 历史副本）
- `ugripper/build/*`
- `ugripper/tmp/*`

其中至少有 3 类目录不应该继续和主链混在一起：

- 旧代码或历史副本：当前仓库内相对路径 `ugripper/`（从父目录看为 `ugripper/ugripper`）
- 构建产物：`build`
- 运行/临时产物：`tmp`

如果不先定义“唯一真源目录”，后续任何规范对齐都会遇到两个问题：

- 不知道应该改哪一份
- review 和并仓时无法判断哪些文件属于正式主链

### 3.2 C++ 命名风格混用

当前代码里可以看到以下混用现象：

- `camera_recorder` 使用了 `namespace camera_recorder`
- `record_runtime` 主要暴露全局 `RecordRuntime`、`RecordRuntimeOptions`
- `sensor_recorder` 大量类型和函数位于全局作用域
- `gripper_hmi` 一部分头文件使用 `namespace gripper_hmi`，另一部分仍暴露全局类型

公共接口层也存在同一模块内部风格混用：

- `RecordRuntime` 里有 `initialize()`、`run()`、`requestStop()`
- `CameraRecorderManager` 里有 `Prepare()`、`StartAll()`、`MonitorUntilStop()`、`StopAll()`
- 同一文件里同时存在 `ModeName()`、`RunStereoDaemon()`、`had_failure()`、`empty()`
- `record_runtime.cpp` 里同时出现 `writeTextFileAtomically()` 和 `makeStereoPlaceholderFromTemplate()`

这会带来 3 个实际问题：

- 新增代码时无法判断应该跟哪套风格
- 模块公共 API 不像“稳定接口”，更像“随写随定”
- 后续并入 `pp_main` 时会额外引入第三套接口命名体系

### 3.3 include 路径风格不统一

当前可以看到三种 include 风格并存：

- `"camera_recorder/camera_recorder.h"`
- `"gripper_hmi_driver.h"`
- `<utils/...>`

这说明：

- 有的模块已经按“模块前缀”暴露头文件
- 有的模块仍依赖“当前 include 目录正好能找到”
- 公共依赖和业务依赖的头文件暴露方式不一致

这会直接影响：

- 目标库边界
- 头文件可迁移性
- 并仓后的 include 稳定性

### 3.4 文件和脚本命名仍有历史噪音

当前仓库仍可见明显不统一命名，例如：

- `im648_CMD.h` / `im648_CMD.cpp`
- `bitrateTest.py`
- `serialTest.py`
- `testVideoPipe.sh`
- `micASR.py`

这些名字的问题不只是“不好看”，而是：

- 不能快速表达职责
- 不利于 grep / 批量重命名 / 规则匹配
- 和当前希望对齐的 `pp_main` 风格不一致

### 3.5 YAML 结构过平，语义过载

当前 `config/camera_recorder.yaml` 的单个 camera 配置同时承载：

- camera 身份
- 设备节点
- 编解码模式
- UVC 控制项
- 分辨率/FPS
- 输出文件列表
- pipeline 调优参数

例如同一层里同时出现：

- `name`
- `device`
- `mode`
- `uvc_roll_absolute`
- `width`
- `height`
- `output_files`
- `qp_init`
- `qp_max_i`

这类结构的直接问题是：

- 无法区分“设备采集参数”和“录制策略参数”
- 无法清楚表达“业务身份”和“输出语义”
- 解析代码不得不在实现里补隐式规则

### 3.6 YAML 和运行时语义没有形成单一真源

当前很多关键语义仍然散落在代码里，而不是由配置统一表达，例如：

- `record_runtime` 里硬编码了 camera 集合、关键设备列表、时序阈值、路径默认值
- `camera_recorder` 里硬编码了 mode 字符串到内部枚举的映射
- stereo 控制/状态通过临时 JSON 文件表达，但没有和正式配置 schema 协同定义

这意味着当前真实状态更接近：

- YAML 是部分真源
- C++ 常量是部分真源
- JSON 临时协议又是另一部分真源

这会让后续并仓非常困难，因为主仓需要的是：

- 边界清晰的配置入口
- 可测试的 schema
- 可迁移的运行时协议

### 3.7 YAML 字段设计和实现使用存在偏差

当前 `camera_recorder.yaml` 中 `output_files` 是序列，但实现中大量场景只取 `.at(0)`。

这说明 schema 和实现认知并不完全一致：

- 从 YAML 看，似乎支持一个 camera 产出多个文件
- 从流程实现看，主链更多按“一个 camera 一个主输出”思考

更具体地说，当前代码的实际行为是：

- parser 要求 `output_files` 是非空序列
- `CameraConfig` 内部也保留为 `std::vector<std::string>`
- 但主录制命令拼装、stereo session finalization、输出文件存在性检查都只使用 `output_files.at(0)`
- 当前仓库内的正式配置也全部使用单元素数组

这种偏差在重构时必须先澄清，否则后续会出现：

- schema 继续膨胀
- 实现继续只支持最常见单文件路径
- 测试又难以定义“到底哪种行为是正确的”

因此这里不建议 Phase 1 直接破坏现有字段形态，而应采用更保守的收敛方式：

- 语义上先按“一台 camera 一个主输出文件”建模
- 兼容性上继续接受当前 `output_files: [main.mkv]` 形式
- Phase 1 明确定义：`output_files[0]` 是唯一受主链保证的 primary output
- 产品默认配置继续维持单元素数组
- 只有后续确认存在稳定且必要的多输出业务需求时，再显式扩展为 `sidecar_outputs` 或等价结构

这样做的好处是：

- 先让 schema 和主链实现重新对齐
- 不会立刻破坏当前 YAML 和 parser 行为
- 避免 `CameraOutputConfig` 一开始就带着含糊的多输出假设
- 测试可以先围绕稳定主路径建立，不必同时覆盖未真正落地的扩展语义

## 4. 建议采用的目标规范

这里的目标规范分为两层：

- Phase 1：在当前 `ugripper` 仓内生效
- Phase 2：作为并入 `pp_main` 时的目标样式

### 4.1 目录真源规则

从现在开始应明确：

- 主链真源目录：`src/`、`config/`、`docs/`、`test/`
- 非真源目录：`build/`、`tmp/`
- 冻结候选目录：当前仓库内相对路径 `ugripper/`

为了避免把“真源”理解得过窄，这里再拆成两类：

- 实现真源：直接定义主链代码、主链配置、主链文档、测试资产
- 交付真源：虽然不属于 `src/`，但确实是产品运行或部署要交付的正式资产

也就是说：

- 前文提到的 `src/`、`config/`、`docs/`、`test/` 是最核心的实现真源
- 但并不意味着所有其他目录都自动变成“可忽略残留”

建议执行时使用下面这张表：

| 目录 | 身份 | 是否真源 | 备注 |
| --- | --- | --- | --- |
| `src/` | 代码真源 | 是 | C++ 主模块与公共库的唯一代码真源 |
| `config/` | 配置真源 | 是 | YAML/JSON 基线配置真源 |
| `docs/` | 文档真源 | 是 | 架构、口径、计划、变更记录真源 |
| `test/` | 测试真源 | 是 | 测试脚本、测试样例、回归资产真源 |
| `audio/`、`audio_en/` | 资源真源 | 是 | 运行时音频资产，属于资源真源，不是代码真源 |
| `auto_update/` | 部署真源 | 是 | 更新、启动相关的 service、rules、脚本属于部署真源 |
| `auto_calibration/` | 部署真源 | 是 | 标定部署与现场编排资产；若仍是产品链路的一部分，应视为部署真源 |
| `pack_script/` | 部署真源 | 是 | 打包元数据与安装脚本，属于交付侧部署真源 |
| `py_script/` | 工具真源 | 是，但非产品主链 | 默认只算仓内工具真源，不纳入主链规范对齐范围；只有当某脚本被确认进入正式运行链路时，才按主链真源要求收口 |
| `ASR/`、`time_sync/` | 冻结目录 | 否 | 除非重新纳入主链，否则不作为当前规范对齐目标 |
| `build/`、`tmp/` | 非真源 | 否 | 明确属于构建和临时产物 |
| `ugripper/` | 冻结目录 | 否，冻结候选 | 当前仓库内的历史副本；Phase 1 不要求立即删除，但应先从构建、文档和主链引用中去主链化；确认无引用后再归档或删除 |

执行要求：

- 后续重构只改真源目录
- 冻结候选目录停止继续叠加新逻辑
- 在文档和脚本中显式去主链化当前仓库内的 `ugripper/`

### 4.2 C++ 命名规则

建议对齐 `pp_main` 的主口径，并在 `ugripper` 内补上缺失约束。

#### 目录和文件

- 目录名：`lower_snake_case`
- 库/实现文件名：`lower_snake_case`
- 入口文件：Phase 1 可保留 `main.cpp`，Phase 2 并仓时统一为 `main.cc`
- 头文件路径应体现模块前缀，例如 `camera_recorder/...`、`gripper_hmi/...`、`sensor_recorder/...`

推荐把 include 规则写成明确约束：

推荐：

```cpp
#include "camera_recorder/camera_config.h"
#include "gripper_hmi/gripper_hmi_driver.h"
#include "utils/file_utils.hpp"
```

不推荐：

```cpp
#include "gripper_hmi_driver.h"
#include "file_utils.hpp"
```

原因是：

- include 路径本身就是模块边界的一部分
- 只有带模块前缀，后续迁目录、拆目标、并入 `pp_main` 时才更稳定
- 这类规则越早统一，后续越少需要做机械替换

#### 类型与接口

- namespace：`lower_snake_case`
- 类型、枚举、结构体：`UpperCamelCase`
- 公共类方法：优先 `UpperCamelCase`
- 私有成员：尾随下划线
- 常量：`kCamelCase`
- CLI 参数：`--kebab-case`

#### 内部实现

- 文件局部 helper：`lower_snake_case`
- 不再新增裸全局函数名的 `UpperCamelCase` 工具函数
- 不再让同一类中混用 `initialize()`、`Start()`、`StopAll()`、`had_failure()`

### 4.3 建议的 namespace 口径

当前不建议立即做大规模目录迁移，但建议先把公共符号口径统一下来。

建议目标：

- `ugripper::camera`
- `ugripper::runtime`
- `ugripper::sensor`
- `ugripper::hmi`
- `ugripper::config`
- `ugripper::utils`

这条规则的价值是：

- 先把符号边界收口
- 后续再决定目录是否迁到 `apps/`、`libs/` 时不会再次重做一遍命名

这里应明确 Phase 1 和 Phase 2 的关系：

- Phase 1：先要求新增公共符号遵守该 namespace 口径
- Phase 1：被触碰模块优先把新抽出的公共类型放进目标 namespace
- Phase 2：逐步迁移现有裸全局类型和历史公共符号
- 不要求一次性清空所有旧全局符号

这条约束很重要，因为它和本文的总原则一致：

- 先收口新增和增量修改
- 再逐步消化历史遗留
- 不为了命名纯度发起一次性全仓 rename

### 4.4 Shell / Python 命名规则

- 文件名：`lower_snake_case`
- 函数名：`lower_snake_case`
- 环境变量：`UPPER_SNAKE_CASE`
- service / rules / path 文件：保持 Linux 生态既有命名，但仓库内脚本名不再新增 camelCase

建议后续逐步重命名：

- `bitrateTest.py` -> `bitrate_test.py`
- `serialTest.py` -> `serial_test.py`
- `testVideoPipe.sh` -> `test_video_pipe.sh`
- `micASR.py` -> `mic_asr.py`
- `im648_CMD.*` -> `im648_cmd.*`

### 4.5 YAML 规范规则

后续 YAML 统一采用下面 6 条规则：

1. 顶层根键使用应用名
2. 增加 `schema_version`
3. 键名统一 `lower_snake_case`
4. 按职责分组，而不是平铺堆字段
5. 配置文件只描述声明式事实，不描述临时状态
6. 合法/非法 YAML 样例要进入测试资产

其中第 1 条建议再写得更硬一点：

- YAML 顶层根键使用 app 类型名，采用 `UpperCamelCase`
- 根键内部所有字段统一使用 `lower_snake_case`
- YAML 文件名也应与 app 名语义一致；当前仓内可先保留 `camera_recorder.yaml`、`sensor_recorder.yaml` 这类过渡命名，并在并入 `pp_main` 时再收敛到主仓 app 风格，例如 `CameraRecorder.yaml`

例如：

- `MessageBridge`
- `Puppeteer`
- `CameraRecorder`
- `UgripperRuntime`

关于 `schema_version` 的演进策略，建议先明确一个保守原则：

- Phase 1 允许 parser 同时兼容旧格式和 `schema_version: 1` 的新格式
- 直到配置库、测试资产和默认配置文件都稳定后，再清退旧格式
- 若后续出现破坏性 schema 升级，应通过显式版本判断或独立迁移脚本处理，而不是在业务逻辑里隐式猜测

这样做的好处是：

- 允许重构分阶段推进
- 避免“文档先升级、实现没跟上”导致现场配置立刻失效
- 把兼容策略前置为工程决策，而不是留给实现层临时发挥

## 5. 推荐的 YAML 重构方式

### 5.1 camera 配置的目标结构

当前 `config/camera_recorder.yaml` 建议保留文件入口，但重构内部结构。

Phase 1 对 camera YAML 的兼容策略建议如下：

- 默认新增和测试资产直接使用 `CameraRecorder/schema_version: 1` 新格式
- parser 允许临时兼容当前旧平铺格式
- 对旧格式中的 `output_files` 保持兼容读取，但只将首元素视为主链保证的主输出
- 当 `camera_config` 库和测试样例稳定后，再停止接受旧格式

这样既能让重构开始推进，也不会要求当前仓库在第一步就一次性重写所有现场配置。

建议从当前：

```yaml
cameras:
  - name: left_cam_main
    device: /dev/left_cam_main
    mode: direct-copy-h265
    uvc_roll_absolute: 4
    width: 1920
    height: 1080
    fps: 60
    output_files: [left_cam_main.mkv]
```

迁移为：

```yaml
CameraRecorder:
  schema_version: 1

  defaults:
    ffmpeg_bin: ffmpeg
    gst_bin: gst-launch-1.0
    container: mkv

  cameras:
    - id: left_cam_main
      role: main
      side: left
      kind: main_uvc
      groups: [session, critical]

      device:
        path: /dev/left_cam_main
        backend: v4l2
        input_format: h265
        capture_width: 1920
        capture_height: 1080
        fps: 60
        controls:
          uvc_roll_absolute: 4

      record:
        pipeline: direct_copy
        codec_policy: passthrough
        output_fps: 60

      output:
        file_name: left_cam_main.mkv
        calibration_key: observation.images.left_cam_main
```

对于 stereo：

```yaml
CameraRecorder:
  schema_version: 1
  cameras:
    - id: left_stereo
      role: stereo
      side: left
      kind: stereo_mjpeg
      groups: [stereo_daemon, critical]

      device:
        path: /dev/left_stereo
        backend: v4l2_mmap
        input_format: mjpeg
        capture_width: 1280
        capture_height: 400
        fps: 60

      record:
        pipeline: stereo_session_encode
        codec_policy: transcode
        output_fps: 30
        input_thread_queue_size: 128
        qp:
          init: 31
          max: 38
          min: 24
          max_i: 38
          min_i: 20

      output:
        file_name: left_stereo.mkv
        calibration_key: observation.images.left_stereo
```

### 5.2 为什么要这样分层

这套结构的核心不是“更漂亮”，而是把职责拆开：

- `id/role/side/kind/groups` 表达业务身份
- `device` 表达采集设备事实
- `record` 表达录制策略
- `output` 表达产物语义

这样做后，后续逻辑可以分开测试：

- schema 校验
- mode / pipeline 选择
- output 路径生成
- health scope 推导

### 5.3 `record_runtime` 不应继续只有 C++ 默认值

当前 `RecordRuntimeOptions` 中存在大量路径、阈值和工具入口默认值。

建议新增独立配置：

- `config/ugripper_runtime.yaml`

推荐结构：

```yaml
UgripperRuntime:
  schema_version: 1

  paths:
    disk_root: /mnt/data_disk
    env_file: /etc/environment
    persist_calibration_file: /etc/ugripper/config/calibration/calibration.json
    fallback_calibration_file: ./config/fakeCamCalib.json

  tools:
    camera_recorder_bin: ./build/src/camera_recorder/camera_recorder
    sensor_recorder_bin: ./build/src/sensor_recorder/sensor_recorder
    audio_play_script: ./audio/audio_play.py
    audio_record_script: ./audio/record_usb_audio.py

  audio:
    pipe: /tmp/umi_audio_pipe
    ready_file: /tmp/umi_audio_ready
    temp_dir: /tmp/umi_audio
    noise_profile: ./audio/noise.prof

  buttons:
    long_press_threshold_ms: 800
    dual_long_press_threshold_ms: 4000
    shutdown_prompt_threshold_ms: 2000

  runtime:
    poll_ms: 20
```

这样做的意义是：

- `record_runtime` 不再承载过多“安装/部署假设”
- 测试可对路径和阈值做独立替换
- 并仓后可以更自然迁到 `standalone/UgripperRuntime/config/UgripperRuntime.yaml`

### 5.4 `sensor_recorder` 也应有自己的配置入口

当前 `sensor_recorder` 也有大量设备和输出约定埋在代码里。

建议新增：

- `config/sensor_recorder.yaml`

推荐分层：

- `devices`
- `output`
- `mcap`
- `timing`

即使 Phase 1 先只迁出一部分参数，也应尽快建立文件入口，避免继续扩大硬编码范围。

### 5.5 JSON 临时协议应和正式 schema 分开

当前 stereo 控制/状态 JSON 文件是运行时协议，不应和长期配置 YAML 混在一起。

建议明确区分：

- YAML：静态配置、部署配置、测试样例
- JSON：运行时状态、一次性控制、episode 元数据

并为 JSON 协议单独补文档：

- 字段含义
- 写入方/读取方
- 兼容策略

否则后续很容易把“临时状态字段”误塞进 YAML。

## 6. 建议的重构顺序

这里给出适合当前工作树的执行顺序，重点是降低一次性大改风险。

### Phase 0：冻结真源

目标：

- 定义唯一真源目录
- 停止对当前仓库内的 `ugripper/` 历史副本继续叠加主链逻辑
- 从文档上明确 `build/`、`tmp/` 非真源

产出：

- 一份目录真源清单
- 一份冻结目录说明

### Phase 1：先定规则，不做全仓 rename

目标：

- 新增代码必须遵守新规则
- 老代码只在“触碰该模块时”顺带收敛命名

执行规则：

- 不做“为了统一而统一”的全仓批量 rename
- 优先统一公共 API、头文件路径、测试目标名、配置文件 schema
- 保留最小必要兼容层

### Phase 2：抽出配置库

目标：

- 从 `camera_recorder.cpp` 中抽出 `camera_config` 相关代码
- 把 YAML 解析、schema 校验、默认值补齐做成独立库

建议拆分为：

- `ugripper_camera_config`
- `ugripper_runtime_config`
- `ugripper_sensor_config`

这一步是后续测试和并仓的前置条件。

### Phase 3：配置驱动替代硬编码

目标：

- 用 YAML 驱动 camera group、critical scope、路径默认值、阈值和工具入口
- 收敛当前散落在 `record_runtime` 的硬编码列表

优先替换对象：

- camera 名称集合
- critical device 列表
- 录制输出文件命名
- 按键阈值
- 工具路径

### Phase 4：测试补齐

目标：

- 引入独立 `test/CMakeLists.txt`
- 增加配置解析测试
- 增加合法/非法 YAML 样例

推荐目录：

```text
test/
  CMakeLists.txt
  src/
    camera_config/
      CMakeLists.txt
      test_camera_config.cc
      data/
        valid_camera.yaml
        invalid_missing_device.yaml
        invalid_bad_qp.yaml
    runtime_config/
      test_runtime_config.cc
      data/
        valid_runtime.yaml
```

### Phase 5：并仓前收口

目标：

- 目录、头文件、目标名、配置文件名和 app 名最终对齐 `pp_main/standalone`
- 迁移时尽量只做“结构移动”，少做“语义再改写”

推荐最终 app 对应关系：

- `record_runtime` -> `UgripperRuntime`
- `camera_recorder` -> `CameraRecorder`
- `sensor_recorder` -> `SensorRecorder`
- `gripper_hmi_test` 或类似工具 -> `GripperHmiTool`

## 7. 推荐的模块与配置映射

| 当前对象 | 当前问题 | 建议目标 |
| --- | --- | --- |
| `config/camera_recorder.yaml` | 平铺字段过多、职责混杂 | `CameraRecorder` 根键 + `device/record/output` 分层 |
| `RecordRuntimeOptions` | 路径和阈值硬编码过多 | `UgripperRuntime` YAML + 结构化 options |
| `sensor_recorder` 设备/输出约定 | 基本靠代码常量表达 | `SensorRecorder` YAML |
| `camera_recorder.cpp` 内 YAML 解析 | 解析、校验、业务编排混在一起 | 独立 `camera_config` 库 |
| `record_runtime` 中 camera 集合常量 | 与 YAML 重复定义 | 从 camera config 派生或从 runtime config 引用 |
| 临时 JSON 控制文件 | 缺少协议边界定义 | 单独文档化为 runtime protocol |

## 8. 命名迁移策略

### 8.1 可以立即生效的规则

- 新文件名只允许 `lower_snake_case`
- 新 YAML 键只允许 `lower_snake_case`
- 新公共类方法优先使用 `UpperCamelCase`
- 新头文件 include 统一使用模块前缀路径

### 8.2 需要按模块渐进迁移的规则

- 全局类型迁入模块 namespace
- 旧 camelCase 脚本文件重命名
- `main.cpp` 在并仓阶段统一为 `main.cc`
- 旧头文件裸 include 改为模块路径 include

### 8.3 不建议现在就做的事

- 全仓一次性 rename 所有旧接口
- 为了风格统一立刻移动全部目录
- 在尚未抽出配置库前先重写全部 YAML

原因很简单：

- 如果边界未收口，rename 只会制造更大的 review 成本和回归面

## 9. 作为完整重构计划输入时，建议优先落地的 12 项任务

1. 明确 `src/`、`config/`、`docs/`、`test/` 为唯一真源目录。
2. 把当前仓库内的 `ugripper/` 标记为冻结候选，停止继续增加主链逻辑。
3. 在顶层文档中补一份“命名与配置基线”摘要，避免规则只存在于单篇参考文档里。
4. 从 `camera_recorder` 提取独立配置解析库和 schema 校验。
5. 为 camera YAML 建立 `schema_version` 和应用根键。
6. 为 `record_runtime` 新增独立 YAML 配置入口，迁出路径和阈值默认值。
7. 为 `sensor_recorder` 新增独立 YAML 配置入口，迁出设备和输出约定。
8. 统一头文件暴露方式，模块公共头全部带模块前缀路径。
9. 统一新公共接口命名为 `UpperCamelCase`，内部 helper 为 `lower_snake_case`。
10. 为 YAML 解析建立独立测试目录和测试样例资产。
11. 把 JSON 临时控制/状态协议单独文档化，不再与长期配置语义混用。
12. 在并入 `pp_main` 前完成 app 名、配置文件名、目标名的一次收口。

## 10. 验收标准

当下面这些条件成立时，可以认为 `ugripper` 的命名和 YAML 规范已经达到“适合作为并仓前基线”的程度：

- 主链真源目录唯一且明确
- 新增文件和脚本不再出现 camelCase 历史命名
- 公共头文件 include 风格统一
- 主要业务模块不再暴露大面积裸全局公共符号
- `camera_recorder`、`record_runtime`、`sensor_recorder` 都有明确配置入口
- YAML 带应用根键和 `schema_version`
- camera YAML 已分层表达 `identity/device/record/output`
- YAML 解析、校验、默认值补齐可脱离主程序独立测试
- 测试目录内有独立 YAML 样例资产
- 并入 `pp_main` 时主要工作已经从“猜语义 + 大改名”降为“结构迁移 + 小范围兼容”

## 11. 最终建议

后续完整重构里，命名规范和 YAML 规范不应拆成两个平行小任务，而应作为同一个“配置边界收口任务”推进。

原因是：

- 命名统一的前提是边界清楚
- YAML 规范化的前提是职责清楚
- 这两件事本质上都在解决“什么是这个模块真正的公共接口”

因此更合理的执行方式是：

- 先冻结真源
- 再抽配置库
- 再用 schema 反推命名口径
- 最后把 app 名、文件名、配置名一起收口到 `pp_main` 风格

这样后续并仓时，`ugripper` 才不是“带着历史包袱搬家”，而是“已经按主仓规范整理好的独立模块集合”。
