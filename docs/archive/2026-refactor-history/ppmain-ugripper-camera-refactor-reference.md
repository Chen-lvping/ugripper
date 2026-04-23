# PPMain And Ugripper Camera Refactor Reference

本文档用于沉淀 `ugripper` 相机相关代码的重构方案，作为后续完整重构 `ugripper` 并合并到 `pp_main` 仓库时的参考输入。

核心前提先讲清楚：

- `pp_main/src/device/cameras` 的相机类处在“设备抽象层”。
- `ugripper` 当前相机代码处在“录制运行时层 + pipeline 编排层”。
- 因此这里不是建议把 `pp_main` 的 `CameraBase/V4L2Camera` 原样搬过来。
- 真正可复用的是它的规范意识：类型统一、生命周期清晰、错误模型明确、配置分层，而不是接口逐项照抄。

相关参考：

- `pp_main/src/device/cameras/camera_base.h`
- `pp_main/src/device/cameras/camera_types.h`
- `pp_main/src/device/cameras/v4l2_camera.h`
- `ugripper/src/camera_recorder/include/camera_recorder/camera_recorder.h`
- `ugripper/src/camera_recorder/src/camera_recorder.cpp`
- `ugripper/src/record_runtime/src/record_runtime.cpp`
- `docs/ppmain-ugripper-process-thread-refactor-reference.md`

## 1. 先讲结论

结论可以先压缩成 6 句话：

- `pp_main` 的相机抽象解决的是“怎么把一个 camera device 表达成稳定接口”。
- `ugripper` 当前相机代码解决的是“怎么把多路异构视频流录下来，并把 timing / calibration / episode 输出串起来”。
- `ugripper` 当前最大问题不是“没有基类”本身，而是“设备信息、录制策略、输出语义、runtime 编排”混在一个 `CameraConfig` 和一组大类里。
- 后续应引入相机类，但这个类不应只是一层薄基类；它需要把“设备层”和“录制会话层”明确拆开。
- 最值得借鉴 `pp_main` 的点是：公共类型、配置结构、状态转换、错误枚举、能力边界。
- 最不该照搬的点是：让所有 `ugripper` 相机都暴露成一个统一的 `GrabFrame()` 型低层采集接口。

换句话说：

- `pp_main` 提供的是“设备抽象规范样板”
- `ugripper` 需要建设的是“面向录制运行时的相机域模型”

## 2. `pp_main` 相机类设计里值得借鉴什么

参考文件：

- `pp_main/src/device/cameras/camera_types.h`
- `pp_main/src/device/cameras/camera_base.h`
- `pp_main/src/device/cameras/v4l2_camera.h`

可以借鉴的不是某个具体 V4L2 细节，而是下面 5 个设计原则。

### 2.1 类型先统一，再谈实现

`pp_main` 先定义：

- `PixelFormat`
- `CameraFeature`
- `FeatureMode`
- `CameraFrame`
- `CameraFormat`
- `CameraConfig`
- `CameraError`

这件事的价值很高，因为它先把“这个领域里什么是稳定概念”固定下来，再让实现类去适配。

对 `ugripper` 的启发是：

- 先统一“逻辑相机”“设备配置”“采集配置”“录制配置”“时间信息”“错误信息”
- 再把 FFmpeg/GStreamer/V4L2/MJPEG warmup 这些实现细节放到后端类

### 2.2 生命周期接口清晰

`pp_main` 设备类至少有清晰的：

- `Open/Close`
- `StartStreaming/StopStreaming`
- `SetFormat/GetFormat`
- `IsOpen/IsStreaming`

它的意义不是接口看起来完整，而是状态边界清晰。

对 `ugripper` 的启发是：

- 相机设备准备态
- 录制会话开始态
- 录制中态
- finalize 态
- 失败态

这些状态应该被显式建模，不能继续散落在 `bool started_ / running_ / failure_ / finalize_pending_` 和若干 if 分支里。

### 2.3 配置对象和运行时对象分离

`pp_main` 的 `CameraConfig` 是配置，`V4L2Camera` 是运行时对象，两者边界相对清楚。

对 `ugripper` 当前代码的对照则比较明显：

- `CameraConfig` 里同时包含设备身份、采集格式、输出文件、编码参数、frame drop 语义
- `record_runtime` 又在外部重新硬编码一套 camera name / file name / calibration path / stream group

后续必须拆分。

### 2.4 错误模型显式

`pp_main` 至少明确区分：

- `DeviceNotFound`
- `PermissionDenied`
- `DeviceBusy`
- `FormatUnsupported`
- `Timeout`
- `IoError`

`ugripper` 现在更多是字符串错误和进程退出码混合。

这导致两个问题：

- 上层无法做按类型恢复
- 日志有信息，但系统缺少稳定的错误语义

### 2.5 设备能力和业务策略不要混在一起

`pp_main` 的设备类关注：

- 设备能开什么格式
- 能不能流式采集
- 支不支持某个 feature control

它不关心：

- episode 文件叫什么
- 哪些流属于 session camera group
- calibration.json 该改哪个字段

这正是 `ugripper` 当前最需要学习的边界。

## 3. `ugripper` 当前相机代码的主要问题

这里不是泛泛而谈，而是直接按现状代码结构拆。

### 3.1 `CameraConfig` 责任过重

参考：

- `ugripper/src/camera_recorder/include/camera_recorder/camera_recorder.h`
- `ugripper/src/camera_recorder/src/camera_recorder.cpp`

当前 `CameraConfig` 同时承担了：

- 逻辑相机标识：`name`
- 设备绑定：`device`
- pipeline 策略：`mode`
- 设备控制：`uvc_roll_absolute`
- 采集输入：`input_format/capture_width/capture_height/fps`
- 输出语义：`output_files`
- 编码策略：`qp_*`
- 变换策略：`video_filter/output_fps`

这会带来三个直接后果：

- 配置字段没有层次，后续很难扩展
- 很多字段只对部分相机/模式有效，但类型上看不出来
- 上层只能靠 `mode + name + if/else` 猜这个相机是什么

### 3.2 相机“类”其实还是 pipeline 类

当前几个 recorder：

- `MainCameraRecorder`
- `HybridCameraRecorder`
- `StereoHybridRecorder`

本质上是：

- 基于 shell command 的录制任务类
- 不是设备抽象类

这本身不一定错，但结果是“相机”这个概念没有真正被建模，只建模了“怎么启动一条录制命令”。

### 3.3 同一份代码里混了两套采集模型

当前同时存在：

- shell/ffmpeg/gstreamer 进程式采集
- `StereoWarmupCapture` 里的进程内 V4L2 MMAP 采集

而且这两套模型之间没有统一抽象，只是在同一个 `.cpp` 文件里并列存在。

这导致：

- stereo 是特殊路径
- timing、状态、错误处理都出现双轨逻辑
- 后续如果再加一种相机，会继续堆条件分支

### 3.4 `record_runtime` 对 camera identity 有大量硬编码

参考：

- `ugripper/src/record_runtime/src/record_runtime.cpp`

当前存在大量硬编码常量：

- `kSessionCameraStreamsCsv`
- `kStereoCameraStreamsCsv`
- `kEpisodeVideoArtifacts`
- `kCriticalDevicePaths`
- `kEncoderTailCheckTargets`
- `kTactileCalibrationTargets`

这说明当前系统里“相机拓扑”没有成为配置化数据模型，而是散在多个运行时模块中各自维护。

这会产生非常现实的问题：

- 改 YAML 不等于系统真的完成改造
- 加一台相机或改一个名字，需要同时改多处 C++ 常量
- 合并到 `pp_main` 之后会更难维护

### 3.5 timing 元信息获取方式耦合到输出日志格式

当前 `ShellCameraRecorder` 通过解析：

- ffmpeg `-stats_mux_pre`
- `-debug_ts`
- gstreamer `identity`
- `progress`

来推导 `record_time_offset_us` 和首末帧时间。

这说明“时间信息采集”是系统级需求，但当前依附在命令输出文本上，缺少稳定的独立抽象。

### 3.6 stereo 相机已经暴露出“单个相机不是最小单位”

`StereoCameraTrack` 实际上包含：

- 一个 warmup capture
- 一个 session recorder
- 一套 ready/recovering/finalize 状态机

这说明对 `ugripper` 来说，某些“相机”并不是一个 `/dev/videoX`，而是一个带业务语义的 camera node。

因此后续抽象层级必须高于 `pp_main::CameraBase`。

## 4. 重构目标：不是统一成一个基类，而是拆成三层

建议把 `ugripper` 相机域拆成三层。

### 4.1 第一层：相机描述层

职责：

- 描述这台逻辑相机是谁
- 它属于哪一类
- 它有哪些固定角色和静态属性

建议类型：

- `CameraId`
- `CameraRole`
- `CameraSide`
- `CameraKind`
- `CameraGroup`

建议语义：

- `CameraId`：`left_cam_main`、`right_stereo` 这种稳定逻辑 ID
- `CameraRole`：`main`、`stereo`、`tactile`
- `CameraSide`：`left`、`right`、`none`
- `CameraKind`：`main_uvc`、`stereo_mjpeg`、`tactile_uvc`
- `CameraGroup`：`session`、`stereo_daemon`、`critical_health_check`

这一层只描述身份，不描述“怎么录”。

### 4.2 第二层：设备与采集层

职责：

- 打开设备
- 应用设备控制
- 校验格式
- 管理 warmup / ready
- 对需要进程内采集的路径提供帧源

建议接口不要直接照抄 `pp_main`，而是收缩成 `ugripper` 真正需要的能力：

```cpp
struct CameraDeviceConfig {
    std::string device_path;
    std::string input_format;
    int capture_width = 0;
    int capture_height = 0;
    int fps = 0;
    std::optional<int> uvc_roll_absolute;
};

enum class CameraDeviceState {
    Closed,
    Ready,
    Streaming,
    Error,
};

enum class CameraStatusCode {
    Ok,
    DeviceMissing,
    PermissionDenied,
    DeviceBusy,
    UnsupportedFormat,
    WarmupTimeout,
    IoError,
    BackendError,
    InvalidConfig,
};

struct CameraStatus {
    CameraStatusCode code = CameraStatusCode::Ok;
    std::string detail;
};

class CameraDevice {
public:
    virtual ~CameraDevice() = default;

    virtual CameraStatus Prepare(const CameraDeviceConfig& config) = 0;
    virtual void Shutdown() = 0;
    virtual bool Ready() const = 0;
    virtual CameraDeviceState State() const = 0;
};
```

对不同设备类型再派生：

- `UvcMainCameraDevice`
- `StereoWarmupCameraDevice`
- `UvcTactileCameraDevice`

注意：

- 这里只放设备相关能力
- 不放输出文件、编码器、session 路径

### 4.3 第三层：录制会话层

职责：

- 把设备输入接到某种 recorder backend
- 管理一段 episode 的开始、停止、finalize
- 产出 `info.json` 所需 timing 元数据
- 汇报 session 级状态

建议接口：

```cpp
struct RecordConfig {
    std::string codec;
    std::string container;
    int width = 0;
    int height = 0;
    int fps = 0;
    int output_fps = 0;
    std::string video_filter;
    int input_thread_queue_size = 0;
    int qp_init = 30;
    int qp_max = 38;
    int qp_min = 24;
    int qp_max_i = 38;
    int qp_min_i = 20;
};

struct CameraSessionInfo {
    std::string camera_id;
    std::string output_file;
    int64_t record_time_offset_us = 0;
    std::optional<int64_t> first_frame_pts_us;
    std::optional<int64_t> first_frame_system_time_us;
    std::optional<int64_t> last_frame_pts_us;
    std::optional<int64_t> last_frame_system_time_us;
};

class CameraSession {
public:
    virtual ~CameraSession() = default;

    virtual CameraStatus Start(const std::string& episode_dir) = 0;
    virtual void Poll() = 0;
    virtual void RequestStop() = 0;
    virtual void Stop() = 0;
    virtual bool IsRunning() const = 0;
    virtual bool HasFailure() const = 0;
    virtual std::optional<CameraSessionInfo> GetSessionInfo() const = 0;
};
```

然后把当前 recorder 变成“backend”而不是“camera 本体”：

- `ShellRecordBackend`
- `DirectCopyRecordBackend`
- `HybridEncodeRecordBackend`
- `StereoSessionRecordBackend`

## 5. 推荐的类设计

下面给的是更接近落地代码的建议结构。

### 5.1 公共类型层

建议新增：

- `camera_domain_types.h`
- `camera_domain_types.cc`

内容建议包括：

- `enum class CameraRole`
- `enum class CameraSide`
- `enum class CameraKind`
- `enum class CameraPipelineMode`
- `enum class CameraStatusCode`
- `struct CameraIdentity`
- `struct CameraDeviceConfig`
- `struct CameraRecordConfig`
- `struct CameraOutputConfig`
- `struct CameraSessionInfo`
- `struct CameraSpec`

其中 `CameraSpec` 是新的核心配置对象：

```cpp
struct CameraSpec {
    CameraIdentity identity;
    CameraDeviceConfig device;
    CameraRecordConfig record;
    CameraOutputConfig output;
};
```

### 5.2 相机规范类

建议新增一个不直接持有录制进程的“规范对象”：

```cpp
class CameraNode {
public:
    virtual ~CameraNode() = default;

    virtual const CameraSpec& spec() const = 0;
    virtual CameraNodeStatus Status() const = 0;
    virtual CameraStatus Prepare() = 0;
    virtual void Poll() = 0;
    virtual void Shutdown() = 0;
    virtual bool Ready() const = 0;
};
```

这里建议显式放弃 `BuildStatusJson()` 作为模块间主接口。

原因是：

- JSON 适合最终导出，不适合作为模块内状态合同
- 一旦 node 状态只靠 JSON 交换，状态字段很容易再次散回字符串和临时约定

因此应补一个强类型状态对象：

```cpp
struct CameraNodeStatus {
    std::string camera_id;
    bool ready = false;
    CameraDeviceState device_state = CameraDeviceState::Closed;
    CameraSessionState session_state = CameraSessionState::Idle;
    CameraStatus last_status{};
    bool has_failure = false;
};
```

这样后续无论是 `record_runtime`、`camera_recorder` manager，还是单元测试，统一观测的都是强类型状态，而不是 JSON 或日志文本。

可以按业务语义派生，而不是按 ffmpeg 命令派生：

- `MainCameraNode`
- `StereoCameraNode`
- `TactileCameraNode`

这三个类的职责：

- `MainCameraNode`：管理主摄准备、UVC 控制、session recorder 创建
- `StereoCameraNode`：管理 warmup capture、session recorder、ready/finalize 状态机
- `TactileCameraNode`：管理触觉摄像头准备和 recorder

这里的重点是：

- 类名按业务语义命名
- pipeline backend 作为它们的内部依赖

### 5.2.1 `CameraNode / CameraDevice / CameraSession` 关系图

这一层必须画清楚，否则实现时很容易出现：

- backend 被挂在错误层级
- `CameraNode` 退化成纯转发壳
- `CameraSession` 和 `CameraDevice` 都去碰 YAML / episode / runtime 状态

推荐关系如下：

```text
RecordRuntime / CameraRecorderMain
            |
            v
       CameraRegistry
            |
            v
        CameraNode  <------------------------------------ 顶层业务对象
        /       \
       /         \
      v           v
CameraDevice   CameraSession  <-------------------------- 一次录制会话对象
  ^                 |
  |                 v
  |          RecordBackend / ShellRecorderProcess
  |                 ^
  |                 |
  +-----------------+------------------------------------ 设备输入提供给 session 使用
```

对应关系明确写成 4 句话：

- `CameraNode` 是顶层业务对象，负责把一个逻辑相机的 identity、device、session、状态机和状态输出组织起来。
- `CameraDevice` 是设备准备层，负责设备打开、格式/控制设置、warmup、ready 判断，以及必要时提供进程内帧源。
- `CameraSession` 是一次录制会话层，负责某个 episode 的开始、停止、finalize 和 session timing 产出。
- backend 是 `CameraSession` 的内部依赖，不是 `CameraNode` 的直接主依赖；`CameraNode` 持有的是 `CameraSession`，由 `CameraSession` 再持有具体 recorder backend。

如果按依赖方向写得更严格一点：

- `CameraNode -> CameraDevice`
- `CameraNode -> CameraSession`
- `CameraSession -> RecordBackend`
- `RecordBackend` 只关心“怎么写这段录制”，不关心“这台相机在系统中的业务身份”

这一点对 stereo 也成立，只是 stereo 的 `CameraDevice` 和 `CameraSession` 都会更复杂：

- `StereoCameraNode -> StereoWarmupDevice`
- `StereoCameraNode -> StereoCameraSession`
- `StereoCameraSession -> StereoSessionRecordBackend`

### 5.3 recorder backend 层

当前 `ShellCameraRecorder` 其实很适合保留，但应改名降级为 backend：

- `ShellRecorderProcess`
- `ShellRecordBackend`

它负责：

- 启动/停止子进程
- 读 stdout/stderr
- 收集 timing
- 退出码管理

而不再负责表达“这是一台什么相机”。

### 5.4 Stereo 专属组合类

当前 stereo 之所以难看，不是因为代码量大，而是它确实不是普通 camera。

因此建议明确承认这一点，单独建组合类：

- `StereoWarmupDevice`
- `StereoSessionRecorder`
- `StereoCameraNode`

关系是：

- `StereoWarmupDevice` 负责设备级预热和帧流
- `StereoSessionRecorder` 负责把帧写成 session 输出
- `StereoCameraNode` 负责业务状态机

这样比把 stereo 勉强塞进通用 recorder 基类干净很多。

这里再把 reviewer 很可能追问的 3 个边界先写清楚：

- stereo 仍然遵守 `CameraNode -> CameraDevice -> CameraSession` 主关系，不单独发明第二套抽象
- `StereoWarmupDevice` 提供的是“已就绪帧流”，由 `StereoCameraSession` 消费
- finalize 由 `StereoCameraSession` 内部负责，runtime 只负责 start/stop orchestration 和等待最终结果

拥有关系建议固定为：

- `StereoDaemonRunner` 持有 `StereoCameraNode`
- `StereoCameraNode` 持有 `StereoWarmupDevice`
- `StereoCameraNode` 持有 `StereoCameraSession`
- `StereoCameraSession` 持有 `StereoSessionRecordBackend`

### 5.5 相机注册表

这是 `ugripper` 现在最缺的一层。

建议新增：

- `CameraRegistry`
- `CameraCatalog`

职责：

- 从 YAML 加载所有 `CameraSpec`
- 提供按 `CameraGroup/CameraRole/CameraSide/CameraId` 查询
- 统一产出：
  - session camera 列表
  - stereo daemon camera 列表
  - health check 关键设备列表
  - episode artifact 列表
  - calibration target 列表

这层一旦存在，`record_runtime` 里的一大批硬编码数组就可以消掉。

建议先给出一个最小头文件草案，避免后续 registry 做成“又一个杂物类”：

```cpp
#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "camera_spec.h"

namespace camera_domain {

namespace fs = std::filesystem;

struct EpisodeArtifact {
    std::string camera_id;
    std::string file_name;
    CameraGroup group = CameraGroup::Session;
};

struct CalibrationTarget {
    std::string camera_id;
    std::string side;
    std::string device_path;
    std::string calibration_key;
    std::string serial_placeholder;
};

class CameraRegistry {
public:
    static bool LoadFromYaml(const fs::path& yaml_path,
                             CameraRegistry* out,
                             std::string* error_message);

    const std::vector<CameraSpec>& All() const;
    const CameraSpec* GetById(const std::string& camera_id) const;
    std::vector<const CameraSpec*> GetByGroup(CameraGroup group) const;

    std::vector<EpisodeArtifact> BuildEpisodeArtifacts() const;
    std::vector<std::string> CriticalDevicePaths() const;
    std::vector<CalibrationTarget> CalibrationTargets() const;

    std::string JoinIds(CameraGroup group, const std::string& separator = ",") const;

private:
    std::vector<CameraSpec> specs_;
};

}  // namespace camera_domain
```

这个接口草案刻意保持最小，只先承载当前 runtime 真正依赖的能力：

- `LoadFromYaml`：让 YAML 成为唯一真源
- `GetById`：服务定向查询和调试
- `GetByGroup`：替代 session/stereo 常量列表
- `BuildEpisodeArtifacts`：替代视频产物常量表
- `CriticalDevicePaths`：替代健康检查常量表
- `CalibrationTargets`：替代触觉标定映射表

可以暂时不放进去的内容：

- 复杂索引缓存
- 运行时可变覆写
- 动态订阅
- 工厂方法

先把 registry 做小、做稳，比一次塞太多能力更重要。

这里还要把错误出口写明确，避免后续实现时在“抛异常 / fatal / 返回空对象”之间摇摆。

Phase 1 建议固定为：

- `LoadFromYaml(...) -> bool`
- 成功时写满 `out`
- 失败时不返回半初始化对象
- `error_message` 返回面向日志和 CLI 的可读错误

至少应覆盖这些错误场景：

- YAML 文件不存在
- YAML 语法错误
- `camera_id` 重名
- output file 冲突
- 非法 `role/group/kind` 字符串
- 非法组合，例如 `stereo` camera 却声明不兼容的 pipeline/backend

如果后续仓库升级到更明确的结果类型，也可以再收敛到 `expected<CameraRegistry, RegistryError>` 风格；但在当前阶段，`bool + out + error_message` 更贴近现有工程代码风格，也更容易落地。

## 6. 推荐的 YAML 重构方向

当前 YAML 的问题不是字段不够，而是字段没有分层。

建议把单个 camera 的配置从：

```yaml
- name: left_cam_main
  device: /dev/left_cam_main
  mode: direct-copy-h265
  uvc_roll_absolute: 4
  width: 1920
  height: 1080
  fps: 60
  output_files: [left_cam_main.mkv]
```

重构为：

```yaml
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
      container: mkv
      output_fps: 60

    output:
      file: left_cam_main.mkv
      calibration_key: observation.images.left_cam_main
```

对于 stereo：

```yaml
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
      container: mkv
      output_fps: 30
      input_thread_queue_size: 128
      qp:
        init: 31
        max: 38
        min: 24
        max_i: 38
        min_i: 20

    output:
      file: left_stereo.mkv
      calibration_key: observation.images.left_stereo
```

这样拆分的好处是：

- `device` 只描述采集端
- `record` 只描述录制策略
- `output` 只描述产物语义
- `identity/groups` 只描述业务身份

这里建议再加一条保守约束：

- Phase 1 默认先用 `groups + output` 推导 session membership、episode artifact、critical health scope 等派生语义

但不要把这条约定神化。应明确保留一个判断标准：

- 如果某个运行时需求不能稳定由 `groups + output` 唯一表达，就应补显式字段，而不是继续靠隐式规则和 if/else 猜

后续最可能补上的显式字段包括：

- `enabled`
- `episode_output`
- `health_critical`

也就是说，当前方案不是否认这些字段将来需要存在，而是要求先验证：

- 现有结构能否稳定表达
- 不能稳定表达时，再正式加字段，不要让派生语义继续漂

## 7. `record_runtime` 应怎么改

这是这次重构最关键的一点，因为只改 `camera_recorder` 不够。

### 7.1 去掉对 camera name 的硬编码编排

当前这几类信息不该继续硬编码在 `record_runtime.cpp`：

- session streams CSV
- stereo streams CSV
- episode video artifact 列表
- critical device path 列表
- tactile calibration target 列表

建议改成：

- `CameraRegistry::GetByGroup(CameraGroup::Session)`
- `CameraRegistry::GetByGroup(CameraGroup::StereoDaemon)`
- `CameraRegistry::BuildEpisodeArtifacts()`
- `CameraRegistry::CriticalDevicePaths()`
- `CameraRegistry::CalibrationTargets()`

### 7.2 把 runtime 对相机的理解从“字符串常量”改为“查询结果”

例如今天的：

- `--only left_cam_main,right_cam_main,...`

后续应来自：

- `registry.JoinIds(CameraGroup::Session)`

例如今天的：

- `kEpisodeVideoArtifacts`

后续应来自：

- 所有启用 `episode_output` 的 camera spec

### 7.3 timing merge 仍然可以保留在 runtime，但输入应标准化

`info.json` 合并逻辑仍可以在 `record_runtime`，但上游不应再返回松散字段名拼接的 JSON。

建议 `camera_recorder` 输出统一结构，例如：

```json
{
  "cameras": {
    "left_cam_main": {
      "record_time_offset_us": 123,
      "first_written_frame_pts_us": 0,
      "first_written_frame_system_time_us": 456,
      "last_written_frame_pts_us": 789,
      "last_written_frame_system_time_us": 999
    }
  }
}
```

这样 `record_runtime` 只做 merge，不再依赖 `${camera_name}_record_time_offset_us` 这种字段拼接。

### 7.4 timing 模型约定

这一节建议在真正开工前先固定下来，否则后面会反复因为“字段名一样但语义不同”返工。

#### 时间字段定义

- `record_time_offset_us`
  - 定义：`system_time_us - frame_pts_us`
  - 语义：把媒体时间轴映射到系统时间轴所需的平移量
- `first_written_frame_pts_us`
  - 定义：最终写入输出文件的第一帧媒体 PTS
- `first_written_frame_system_time_us`
  - 定义：与第一帧写入帧对应的系统时间
- `last_written_frame_pts_us`
  - 定义：最终写入输出文件的最后一帧媒体 PTS
- `last_written_frame_system_time_us`
  - 定义：与最后一帧写入帧对应的系统时间

#### 时间基准

- `*_pts_us` 统一表示媒体时间轴上的微秒值
- `*_system_time_us` 统一表示 Unix epoch 微秒值
- 所有导出字段统一使用 `us`，避免在 JSON 和 merge 逻辑中混用 `ms/ns/sec`
- 若实现内部使用 steady clock 或 ffmpeg timebase，必须在导出前转换到上述统一口径

#### 可空字段语义

- `record_time_offset_us`
  - 期望非空，是 merge 的主合同字段
- `first_written_frame_pts_us`
  - 可空，表示无法稳定获得媒体首帧 PTS
- `first_written_frame_system_time_us`
  - 可空，表示无法稳定锁定首帧对应系统时间
- `last_written_frame_pts_us`
  - 可空，表示停止阶段未能拿到稳定尾帧 PTS
- `last_written_frame_system_time_us`
  - 可空，表示停止阶段未能拿到稳定尾帧系统时间

建议 contract 再加一条：

- 如果 `record_time_offset_us` 为空，则这路 camera session 视为 timing 不合格，默认不能静默 merge 成成功状态

#### 来源标记

建议在 session 输出里增加来源标记，避免后续难以判断这个 timing 到底怎么来的：

```cpp
enum class TimingSource {
    Unknown,
    FfmpegStatsMuxPre,
    FfmpegDebugTs,
    GstreamerIdentity,
    InProcessFrameSubmission,
    VideoProbeFallback,
};
```

对应 JSON 可以落成：

```json
{
  "record_time_offset_us": 123,
  "timing_source": "ffmpeg_stats_mux_pre"
}
```

这样做的价值是：

- 出问题时能知道是采集链路不稳，还是 fallback 在生效
- runtime 可以按来源决定日志等级和容错策略

#### merge 时的 contract

`record_runtime` 与 `camera_recorder` 之间建议固定以下契约：

1. `camera_recorder` 必须按 `camera_id -> timing info` 输出结构化结果。
2. `record_runtime` 不再拼接动态字段名，不再解析 recorder stdout。
3. `record_runtime` 至少要求每路输出有：
   - `record_time_offset_us`
   - `timing_source`
4. 如果只有 `record_time_offset_us` 存在，而首末帧字段缺失：
   - 可以允许 episode 通过基础 merge
   - 但应记录 warning，并标明这是降级 timing
5. 如果 `record_time_offset_us` 缺失：
   - 默认视为该路 timing merge 失败
   - 除非该 camera kind 明确声明允许 fallback

也就是说，后续 merge 判断应从“字段尽量多就行”改成“有明确主合同字段，其余字段分级容忍”。

## 8. 推荐的目录和代码切分方式

如果以“先在 `ugripper` 内部重构，再并入 `pp_main`”为顺序，建议先按下面方式切。

### 8.1 在 `ugripper` 内部的建议切分

建议新建：

```text
src/camera_domain/
  include/camera_domain/
    camera_types.h
    camera_spec.h
    camera_status.h
    camera_node.h
    camera_registry.h
    camera_backend.h
  src/
    camera_spec_loader.cpp
    camera_registry.cpp
    shell_record_backend.cpp
    main_camera_node.cpp
    tactile_camera_node.cpp
    stereo_camera_node.cpp
    stereo_warmup_device.cpp
```

然后把现有 `camera_recorder` 变成一个装配层可执行程序：

- 读 registry
- 根据 group 选择 camera node
- 启动对应 session

### 8.2 Phase 1 施工目录树

如果按“先抽公共类型和 registry，再逐步替换 runtime 依赖”的思路落地，Phase 1 建议直接把目录树定成下面这样：

```text
src/camera_domain/
  CMakeLists.txt
  include/camera_domain/
    camera_types.h
    camera_status.h
    camera_spec.h
    camera_registry.h
    camera_device.h
    camera_session.h
    camera_node.h
    camera_backend.h
    camera_artifacts.h
    camera_timing.h
  src/
    camera_spec_loader.cpp
    camera_registry.cpp
    camera_status.cpp
    camera_timing.cpp
    shell_record_backend.cpp
```

Phase 1 只建议真正实现这几块：

- `camera_types.h`
  - `CameraRole/CameraSide/CameraKind/CameraGroup`
- `camera_status.h`
  - `CameraStatusCode/CameraStatus`
- `camera_spec.h`
  - `CameraIdentity/CameraDeviceConfig/CameraRecordConfig/CameraOutputConfig/CameraSpec`
- `camera_registry.h`
  - YAML 加载与查询接口
- `camera_artifacts.h`
  - `EpisodeArtifact/CalibrationTarget`
- `camera_timing.h`
  - `TimingSource/CameraTimingInfo`
- `camera_backend.h`
  - 先只放最小 backend 抽象
- `shell_record_backend.cpp`
  - 先把旧 `ShellCameraRecorder` 下沉改名

而下面这些头文件在 Phase 1 可以先只给接口草案，不必全部实现：

- `camera_device.h`
- `camera_session.h`
- `camera_node.h`

这样做的目的不是偷懒，而是控制首轮重构范围：

- 先把配置真源和类型边界立住
- 再让 runtime 改依赖 registry
- 最后再把 node/device/session 真正接进来

### 8.3 Phase 1 最小头文件草案

为了更接近开工，下面给出建议中的最小头文件轮廓。

`camera_device.h`

```cpp
#pragma once

#include "camera_spec.h"
#include "camera_status.h"

namespace camera_domain {

enum class CameraDeviceState {
    Closed,
    Preparing,
    Ready,
    Streaming,
    Error,
};

class CameraDevice {
public:
    virtual ~CameraDevice() = default;

    virtual CameraStatus Prepare(const CameraDeviceConfig& config) = 0;
    virtual void Poll() = 0;
    virtual void Shutdown() = 0;
    virtual bool Ready() const = 0;
    virtual CameraDeviceState State() const = 0;
};

}  // namespace camera_domain
```

`camera_session.h`

```cpp
#pragma once

#include <optional>
#include <string>

#include "camera_spec.h"
#include "camera_status.h"
#include "camera_timing.h"

namespace camera_domain {

enum class CameraSessionState {
    Idle,
    Starting,
    Running,
    Stopping,
    Finalizing,
    Completed,
    Error,
};

class CameraSession {
public:
    virtual ~CameraSession() = default;

    virtual CameraStatus Start(const CameraSpec& spec, const std::string& episode_dir) = 0;
    virtual void Poll() = 0;
    virtual void RequestStop() = 0;
    virtual void Stop() = 0;
    virtual CameraSessionState State() const = 0;
    virtual bool IsRunning() const = 0;
    virtual bool HasFailure() const = 0;
    virtual std::optional<CameraTimingInfo> Timing() const = 0;
};

}  // namespace camera_domain
```

建议把状态转移约束也提前写死：

```text
Idle -> Starting -> Running
Running -> Stopping -> Finalizing -> Completed
任意状态出现不可恢复 fault -> Error
Completed 之后不可再次 Start，必须销毁并重建 session 对象
```

同时要明确 `RequestStop()` 和 `Stop()` 的区别，避免不同 backend 各自理解：

- `RequestStop()`
  - 异步请求
  - 触发优雅停录
  - 允许立即返回
- `Stop()`
  - 阻塞收尾
  - 保证子进程、线程、文件句柄等最终资源回收完成
  - 返回后 session 不再运行

这条语义约束对 shell backend、stereo session backend 和未来其他 backend 都应一致。

`camera_node.h`

```cpp
#pragma once

#include <memory>

#include "camera_device.h"
#include "camera_session.h"
#include "camera_spec.h"

namespace camera_domain {

class CameraNode {
public:
    virtual ~CameraNode() = default;

    virtual const CameraSpec& Spec() const = 0;
    virtual CameraNodeStatus Status() const = 0;
    virtual CameraStatus Prepare() = 0;
    virtual CameraStatus StartSession(const std::string& episode_dir) = 0;
    virtual void Poll() = 0;
    virtual void RequestStopSession() = 0;
    virtual void Shutdown() = 0;
    virtual bool Ready() const = 0;
};

protected:
    std::unique_ptr<CameraDevice> device_;
    std::unique_ptr<CameraSession> session_;
};

}  // namespace camera_domain
```

`camera_backend.h`

```cpp
#pragma once

#include <optional>
#include <string>

#include "camera_spec.h"
#include "camera_status.h"
#include "camera_timing.h"

namespace camera_domain {

class CameraBackend {
public:
    virtual ~CameraBackend() = default;

    virtual CameraStatus Start(const CameraSpec& spec, const std::string& episode_dir) = 0;
    virtual void Poll() = 0;
    virtual void RequestStop() = 0;
    virtual void Stop() = 0;
    virtual bool IsRunning() const = 0;
    virtual std::optional<CameraTimingInfo> Timing() const = 0;
};

}  // namespace camera_domain
```

这个 backend 接口不追求终局完美，只是为了在第 3 步过渡时把旧 `ShellCameraRecorder` 收敛成一个稳定依赖面，避免到时再次围绕“backend 应该长什么样”重新讨论。

### 8.4 合并到 `pp_main` 时的建议落点

后续并入 `pp_main` 时，不建议把 `ugripper` 这些类直接塞进 `pp_main/src/device/cameras`。

更合理的关系是：

- `pp_main/src/device/cameras`
  - 保留通用设备层抽象
- `pp_main/standalone/ugripper/...` 或 `pp_main/src/ugripper/...`
  - 承载 `ugripper` 自己的 camera domain / runtime / recorder 逻辑

即：

- `pp_main::device::cameras` 是下层
- `ugripper camera domain` 是上层业务模块

如果未来真的要共享实现，应该共享：

- V4L2 打开与格式协商工具
- 通用 CameraError / PixelFormat 风格类型
- UVC control 操作封装

而不是共享整个 `CameraNode` 业务层。

## 9. 建议的迁移顺序

不要试图一步到位替换所有 camera 代码，建议 4 步迁移。

### 第 1 步：先抽公共类型和配置层，不改运行时行为

目标：

- 新增 `CameraSpec` / `CameraRegistry`
- 新 YAML 支持加载
- 现有 `camera_recorder` 仍然生成同样的命令

验收标准：

- `--dry-run` 输出命令不变
- `record_runtime` 行为不变

### 第 2 步：让 `record_runtime` 改为依赖 `CameraRegistry`

目标：

- 去掉 `kSessionCameraStreamsCsv`
- 去掉 `kStereoCameraStreamsCsv`
- 去掉 `kEpisodeVideoArtifacts`
- 去掉 `kCriticalDevicePaths`
- 去掉 `kTactileCalibrationTargets`

验收标准：

- 配置成为相机拓扑的唯一真源
- 改 camera id / file name 不再需要同步改多份 C++

这一阶段建议额外明确一个约束：

- 不允许新 `CameraRegistry` 与旧 runtime 常量长期双轨并存

更直白一点：

- 一旦 `CameraRegistry` 能稳定产出 session/stereo/artifact/device/calibration 查询结果，就应尽快删掉 `record_runtime` 里的对应硬编码数组

否则很容易出现：

- 新 registry 在长
- 旧常量还在生效
- 两边数据逐渐漂移
- 最后变成“抽象层数更多，但系统并没有真正收口”

### 第 3 步：把 recorder 类从“相机类”改成“backend 类”

目标：

- `MainCameraRecorder/HybridCameraRecorder/StereoHybridRecorder`
  改造成 backend
- 新增 `MainCameraNode/TactileCameraNode/StereoCameraNode`

验收标准：

- 业务语义和命令拼装分离
- stereo 状态机从大文件中独立出来

### 第 4 步：评估与 `pp_main` 设备层的收敛点

目标：

- 看哪些 V4L2/UVC 公共逻辑能下沉到 `pp_main`
- 保持 `ugripper` 的 runtime 层独立

验收标准：

- 合并后目录边界清楚
- `ugripper` 不会被迫退化成“所有相机都按一台普通摄像头处理”

## 10. 哪些设计可以直接借鉴，哪些不能直接照搬

### 10.1 可以直接借鉴

- 先定义公共类型，再写实现
- 显式状态机和错误枚举
- 配置对象和运行时对象分离
- 生命周期接口清晰
- 设备能力与业务策略分层

### 10.2 不能直接照搬

- `CameraBase::GrabFrame()` 作为所有相机统一主接口
- 用单一 `CameraConfig` 覆盖设备层和录制层全部配置
- 默认假设所有 camera 都是“打开设备后直接开始抓帧”
- 把 stereo 这种组合型节点硬塞成普通 camera

### 10.3 需要选择性复用

- `CameraError` 风格的错误模型：建议复用思想，不必逐项相同
- `PixelFormat` 风格类型：可保留风格，但 `ugripper` 更需要 `pipeline/input/output` 的三段描述
- `V4L2Camera` 的实现：可复用部分低层能力，但不应成为 `ugripper` 主抽象

### 10.4 当前阶段不建议做的事

- 过早把 `ugripper camera domain` 塞进 `pp_main/src/device/cameras`
- 为了“共享实现”而把高层 `CameraNode` 设计压扁成低层设备接口
- 在 `pp_main` 和 `ugripper` 之间提前抽一个过大的通用 camera 框架

当前更合适的共享范围仍然是：

- 类型风格
- 错误模型风格
- V4L2/UVC 低层工具

而不是共享整个 `ugripper` camera domain。

## 11. runtime / recorder / registry 边界补充

这一节建议额外补清楚，否则 reviewer 很容易追问“registry 到底是谁在用、谁说了算、CLI 能改多少”。

### 11.1 `CameraRegistry` 是 runtime 和 camera_recorder 共用的真源

建议明确：

- `CameraRegistry` 不是 runtime 专用对象
- `CameraRegistry` 也不是 recorder 私有对象
- 它是相机拓扑与配置的共享真源

建议分工如下：

- `record_runtime`
  - 负责 orchestration
  - 负责 episode 生命周期
  - 负责 health check
  - 负责 merge session 输出
- `camera_recorder`
  - 读取同一份 `CameraRegistry`
  - 根据查询结果实例化 `CameraNode/CameraSession/backend`
  - 输出结构化 timing 和录制结果

也就是说：

- episode artifact 的语义真源来自 registry
- artifact 是否实际产生成功，由 recorder/session 输出结果确认

### 11.2 `--only` 这类 CLI 覆写只允许做筛选，不允许改 spec 语义

Phase 1 建议明确：

- YAML 是唯一 spec 真源
- CLI override 只允许筛选和运行范围控制
- CLI 不允许重写 `role/kind/group/output/calibration` 这类语义字段

因此：

- `--only a,b,c` 可以保留
- `--config-yaml path` 可以保留
- 但不建议新增“临时把某路 camera 改成别的 kind/group/output”这类覆盖

对 test / dry-run / partial selection 的处理方式建议是：

- test：使用独立 YAML
- dry-run：读取 registry 后只打印选择结果和命令
- partial selection：通过 `--only` 在 registry 查询结果上做过滤

### 11.3 deployment 层的设备路径变更不建议由 CLI 临时覆盖

如果部署环境需要换设备路径，建议通过：

- 部署生成不同 YAML
- 或部署阶段模板渲染 YAML

而不是通过运行时 CLI 临时改 spec。

原因是：

- 设备路径属于 topology/spec 范畴
- 不是一次临时执行参数

这样才能保证：

- runtime
- recorder
- dry-run
- test

看到的是同一份相机配置真相

## 12. 最终建议

如果只保留一句执行建议，那就是：

- 不要把这次重构理解成“给 `camera_recorder` 补一个基类”

而应理解成：

- 先把 `ugripper` 的相机域从“命令拼装集合”重构为“相机拓扑 + 设备节点 + 录制会话 + runtime 编排”

在这个前提下，`pp_main/src/device/cameras` 提供的参考价值主要有三点：

- 提醒我们先统一类型和错误模型
- 提醒我们把生命周期做成显式接口
- 提醒我们把设备层和业务层分开

但 `ugripper` 最终需要形成的是比 `pp_main` 相机类更高一层的抽象：

- `pp_main` 解决“怎么操作 camera device”
- `ugripper` 解决“怎么管理一个带业务身份和输出语义的 camera node”

这才是后续并入 `pp_main` 时既能复用规范、又不丢业务边界的正确方向。
