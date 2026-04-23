# Naming And YAML Contract

本文档冻结 `ugripper` 在 `Post-A3 Step 4` 阶段的命名与 YAML 契约。

目标是：

- 先固定规则，不做全仓大规模 rename
- 先固定 `camera_recorder.yaml` 的兼容边界，不提前重写 parser
- 让后续 Step 5/6 的模块抽取、测试补齐和并仓准备有统一口径

## 1. 适用范围

当前文档优先约束：

- 新增或新抽出的 C++ 公共符号
- `camera_recorder` 相关 include 路径和配置契约
- `config/camera_recorder.yaml` 的兼容策略
- `test/src/camera/config_samples/` 的测试资产口径

当前文档不要求：

- 立即重命名现有 `main.cpp`
- 立即把所有现有 namespace 改成 `ugripper::*`
- 立即让 parser 支持 `schema_version: 1`

## 2. 命名契约

### 2.1 新增公共 namespace 目标

从 `Step 4` 开始，新增公共符号默认目标 namespace 为：

- `ugripper::camera`
- `ugripper::runtime`
- `ugripper::sensor`
- `ugripper::hmi`
- `ugripper::config`
- `ugripper::utils`

过渡规则：

- 现有模块不做一次性 namespace 迁移
- 新抽出的正式模块优先直接使用目标 namespace
- 若旧模块暂时保留历史 namespace，必须在文档或 PR 描述中说明其为过渡状态

### 2.2 文件与目录命名

- 目录名：`lower_snake_case`
- 库/实现文件名：`lower_snake_case`
- 入口文件：
  - 当前仓库阶段允许继续保留 `main.cpp`
  - 并入 `standalone` 时再统一评估 `main.cc`

### 2.3 C++ 标识符命名

新增或新抽出的公共接口遵守：

- 类型：`UpperCamelCase`
- 常量：`kCamelCase`
- 成员变量：尾随下划线

过渡规则：

- 不为了风格纯化而批量重命名旧 API
- 现有模块内部若仍混用 `Prepare()` / `empty()` / `had_failure()`，在 Step 4 只记录，不立即统一
- 新抽出的库 API 应避免继续扩大这种混用

### 2.4 include 路径规则

公共头文件默认使用模块前缀：

- `"camera_recorder/camera_recorder.h"`
- `"sensor_recorder/..."` 
- `"gripper_hmi/..."`
- `<utils/...>`

过渡规则：

- 已存在的非前缀 include 不在 Step 4 强制改动
- 新增公共头文件和新抽出的模块头文件，必须优先采用模块前缀风格
- 同目录私有实现头在模块内部可继续使用局部 include，但不要向外暴露

## 3. Camera YAML 现状事实

当前 parser 真实入口是：

- 文件：`config/camera_recorder.yaml`
- 实现：`src/camera_recorder/src/camera_recorder.cpp`
- 入口函数：`LoadCameraConfigList()`

当前可执行的旧格式事实：

- YAML 顶层必须有非空 `cameras` 序列
- 每个 `camera` 条目必须是 map
- 当前必填字段：
  - `name`
  - `device`
  - `mode`
  - `width`
  - `height`
  - `fps`
  - `output_files`
- 当前可选字段：
  - `uvc_roll_absolute`
  - `input_format`
  - `capture_width`
  - `capture_height`
  - `output_fps`
  - `eye_width`
  - `eye_height`
  - `video_filter`
  - `input_thread_queue_size`
  - `qp_init`
  - `qp_max`
  - `qp_min`
  - `qp_max_i`
  - `qp_min_i`
- 当前 `mode` 仅接受：
  - `direct-copy-h265`
  - `hybrid-decode-encode`
  - `stereo-hybrid-decode-encode`
- 当前 `output_files` 必须是非空序列
- 当前实现大量路径只使用 `output_files.at(0)`

因此，Step 4 明确冻结以下事实：

- 旧格式仍然是当前唯一可执行输入格式
- `output_files[0]` 是唯一受主链保证的 primary output
- 旧格式里的多元素 `output_files` 暂不承诺主链语义

## 4. Camera YAML v1 契约目标

Step 4 先冻结目标结构，不在本步骤实现 parser 支持。

`schema_version: 1` 的目标口径如下：

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

Step 4 只冻结以下结构原则：

- 顶层根键：`CameraRecorder`
- 顶层版本：`schema_version: 1`
- camera 身份与分组单独表达
- `device`、`record`、`output` 分层
- `output.file_name` 对应 legacy `output_files[0]`

## 5. Legacy -> v1 字段映射

| 旧字段 | v1 目标位置 | Step 4 状态 |
| --- | --- | --- |
| `name` | `id` | `deprecated but accepted` |
| `device` | `device.path` | `deprecated but accepted` |
| `mode` | `record.pipeline` 或等价策略字段 | `deprecated but accepted` |
| `uvc_roll_absolute` | `device.uvc.roll_absolute` | `deprecated but accepted` |
| `input_format` | `device.input_format` | 继续接受 |
| `capture_width` | `device.capture_width` | 继续接受 |
| `capture_height` | `device.capture_height` | 继续接受 |
| `width` | `device.width` 或派生输出尺寸 | `deprecated but accepted` |
| `height` | `device.height` 或派生输出尺寸 | `deprecated but accepted` |
| `fps` | `device.fps` | `deprecated but accepted` |
| `output_fps` | `record.output_fps` | 继续接受 |
| `video_filter` | `record.video_filter` | 继续接受 |
| `output_files` | `output.file_name` | `deprecated but accepted`，仅 `output_files[0]` 为主链保证 |
| `input_thread_queue_size` | `record.input_thread_queue_size` | 继续接受 |
| `qp_init` | `record.qp.init` | `deprecated but accepted` |
| `qp_max` | `record.qp.max` | `deprecated but accepted` |
| `qp_min` | `record.qp.min` | `deprecated but accepted` |
| `qp_max_i` | `record.qp.max_i` | `deprecated but accepted` |
| `qp_min_i` | `record.qp.min_i` | `deprecated but accepted` |

## 6. 兼容策略

### 6.1 Step 4

- 只成文化，不改 parser 行为
- 不输出 deprecation warning
- 不禁止旧格式
- 新增测试样例，但 `schema_version: 1` 只作为 contract 资产，不要求当前 parser 可执行通过

### 6.2 Step 6

- 开始在兼容 parser 中支持旧格式和 `schema_version: 1`
- 对旧平铺字段输出一次性 deprecation warning
- 补齐 parser 兼容测试

### 6.3 禁用旧格式

只有在以下条件同时满足后，才允许单独立项禁用旧格式：

- 默认配置已迁到 v1
- `test/src/camera/config_samples/` 已有稳定的 v1 合法/非法样例
- field 配置样例和运行文档已完成迁移
- `REFACTOR_LOG` 中显式记录禁用动作和回退策略

## 7. 测试资产口径

`Step 4` 新增的样例目录为：

- `test/src/camera/config_samples/`

当前样例分成三类：

- legacy flat valid
- schema v1 valid contract sample
- invalid sample

执行口径：

- Step 4 可执行验证只要求：
  - legacy valid 样例可被当前 parser 读入
  - invalid legacy 样例能稳定报错
- `schema_version: 1` 的可执行解析验证后移到 Step 6

## 8. 本步骤不做

- 批量重命名文件
- 批量替换 namespace
- 立即把 `camera_recorder.yaml` 改成 v1
- 立即把 parser 从旧格式切到双格式
