# 主摄标定数据结构说明

## 1. 背景

当前右主摄支持通过 XU 命令读写相机内部 flash 中的一块固定存储区，用于保存主摄标定参数。

这块存储区的协议特性如下：

- 总共有 `128` 组数据槽位。
- 每次通过 XU 命令收发 `9` 个 `uint8_t`。
- 第 `0` 个字节是 `index`。
- 后面 `8` 个字节才是真正的数据。
- 因此总可用 payload 容量为：`128 * 8 = 1024` 字节。
- 只有在发送“写入 flash”命令后，前面写入的缓存数据才会真正落到 flash，掉电后仍能保留。

当前这份结构定义的是“仅主摄”的标定数据。

---

## 2. 当前 payload 总览

- payload 名称：`MainCameraCalibrationDataV1`
- payload 总大小：`1024` 字节
- magic：`MCAL`
- 数据格式版本：`0x00010000`

其中：

- `MCAL` 表示 `Main Camera CALibration`
- 用于标识“这 1024 字节内容按主摄标定结构解析”

---

## 3. C/C++ 结构定义

```cpp
struct MainCameraCalibrationHeader
{
    char magic[4];
    uint32_t dataFormatVersion;
    uint16_t payloadSize;
    uint16_t headerSize;
    uint32_t validFields;
    uint8_t reserved[16];
};

struct MainCameraIntrinsicsBlock
{
    uint32_t cameraModelEnum;
    float distortionCoefficients[4];
    float intrinsics[4];
    float resolution[2];
    uint8_t reserved[4];
};

struct MainCameraCalibrationDataV1
{
    MainCameraCalibrationHeader header;
    MainCameraIntrinsicsBlock mainCamera;
    uint8_t reserved[944];
};
```

---

## 4. 结构大小

- `sizeof(MainCameraCalibrationHeader) = 32`
- `sizeof(MainCameraIntrinsicsBlock) = 48`
- `sizeof(MainCameraCalibrationDataV1) = 1024`

---

## 5. 字段说明

### 5.1 MainCameraCalibrationHeader

#### `magic[4]`

- 类型：4 字节 ASCII 字符
- 当前值：`"MCAL"`
- 用途：标识当前 payload 是主摄标定数据结构

#### `dataFormatVersion`

- 类型：`uint32_t`
- 当前值：`0x00010000`
- 用途：表示当前结构版本
- 未来如果结构字段变化，可以通过这个版本号区分不同解析方式

#### `payloadSize`

- 类型：`uint16_t`
- 当前值：`80`
- 用途：表示“当前版本真正定义并使用了多少字节”

计算方式：

- `32` 字节 header
- `48` 字节主摄参数块
- 合计 `80` 字节

也就是说：

- 整个存储区仍然是 `1024` 字节
- 但当前版本里真正有定义的数据只用到了前 `80` 字节

#### `headerSize`

- 类型：`uint16_t`
- 当前值：`32`
- 用途：表示 header 自身大小

#### `validFields`

- 类型：`uint32_t`
- 当前值：`0x0000000F`
- 用途：表示哪些字段有效

当前 bit 定义如下：

- bit0：`cameraModelEnum` 有效
- bit1：`distortionCoefficients` 有效
- bit2：`intrinsics` 有效
- bit3：`resolution` 有效

#### `reserved[16]`

- 类型：`uint8_t[16]`
- 用途：预留字段，给后续扩展使用

当前实际使用方式：

- `reserved[0..3]`：存放 `mainCamera` 这 48 字节数据块的 32 位 checksum
- `reserved[4..15]`：当前全部为 `0`

这里的 `reserved` 可以理解为：

- 现在先留着不用的字节
- 后续协议升级时优先复用
- 用来保持结构长度固定、字段偏移稳定

---

### 5.2 MainCameraIntrinsicsBlock

#### `cameraModelEnum`

- 类型：`uint32_t`
- 当前测试映射：
  - `1 = pinhole + equidistant`

#### `distortionCoefficients[4]`

- 类型：`float[4]`
- 含义：主摄畸变参数
- 当前按 `rgb_video_ros-camchain.yaml` 里的顺序原样写入

#### `intrinsics[4]`

- 类型：`float[4]`
- 含义：主摄内参
- 顺序定义为：
  - `intrinsics[0] = fx`
  - `intrinsics[1] = fy`
  - `intrinsics[2] = cx`
  - `intrinsics[3] = cy`

#### `resolution[2]`

- 类型：`float[2]`
- 含义：图像分辨率
- 顺序定义为：
  - `resolution[0] = width`
  - `resolution[1] = height`

当前虽然分辨率本质上是整数，但这里仍存成 `float`，是为了让结构布局简单，同时后续兼容更方便。

#### `reserved[4]`

- 类型：`uint8_t[4]`
- 用途：主摄参数块内的预留字段
- 当前全部为 `0`

---

### 5.3 尾部 `reserved[944]`

- 类型：`uint8_t[944]`
- 用途：把总 payload 固定到 `1024` 字节
- 当前全部为 `0`

这块后续可以扩展存放例如：

- 标定时间
- 相机序列号
- 标定来源标识
- 安装位姿
- 曝光/焦距/温度等附加信息
- 更多畸变参数

---

## 6. 字节偏移布局

- `0x000 - 0x003`：`magic = "MCAL"`
- `0x004 - 0x007`：`dataFormatVersion`
- `0x008 - 0x009`：`payloadSize`
- `0x00A - 0x00B`：`headerSize`
- `0x00C - 0x00F`：`validFields`
- `0x010 - 0x01F`：`header.reserved[16]`

- `0x020 - 0x023`：`cameraModelEnum`
- `0x024 - 0x033`：`distortionCoefficients[4]`
- `0x034 - 0x043`：`intrinsics[4]`
- `0x044 - 0x04B`：`resolution[2]`
- `0x04C - 0x04F`：`mainCamera.reserved[4]`

- `0x050 - 0x3FF`：尾部 `reserved[944]`

---

## 7. 本次写入右主摄的测试数据

来源文件：

- `tmp/ugripper_calib/ugripper_calib/dag91126320001d6/rgb_video_ros-camchain.yaml`

源数据内容：

- `camera_model: pinhole`
- `distortion_model: equidistant`
- `distortion_coeffs`：
  - `-0.07540654369424184`
  - `0.004718508977632818`
  - `-0.01154659181409967`
  - `0.004264896746500091`
- `intrinsics`：
  - `829.3014944706887`
  - `828.1721097578554`
  - `948.7861807787111`
  - `542.8676060091866`
- `resolution`：
  - `1920`
  - `1080`

映射后的 payload 实际写入值：

- `magic = "MCAL"`
- `dataFormatVersion = 0x00010000`
- `payloadSize = 80`
- `headerSize = 32`
- `validFields = 0x0000000F`
- `checksum = 0x5353C2E6`
- `cameraModelEnum = 1`
- `distortionCoefficients`：
  - `-0.07540654369424184`
  - `0.004718508977632818`
  - `-0.01154659181409967`
  - `0.004264896746500091`
- `intrinsics`：
  - `829.3014944706887`
  - `828.1721097578554`
  - `948.7861807787111`
  - `542.8676060091866`
- `resolution`：
  - `1920.0`
  - `1080.0`

---

## 8. 读回校验结果

写入 flash 后，从右主摄读回得到：

- `magic = MCAL`
- `data_format_version = 0x10000`
- `payload_size = 80`
- `header_size = 32`
- `valid_fields = 0xf`
- `camera_model_enum = 1`
- `distortion_coeffs = -0.0754065, 0.00471851, -0.0115466, 0.0042649`
- `intrinsics = 829.302, 828.172, 948.786, 542.868`
- `resolution = 1920x1080`
- `stored_checksum = 0x5353c2e6`
- `computed_checksum = 0x5353c2e6`

结论：

- 写入成功
- flash 落盘成功
- 读回校验成功

---

## 9. 备注

- 这份结构是“仅主摄”的标定结构，不包含双目和 IMU 字段。
- 相机 XU 收发协议本身没有变化，这里只是重新定义了 `1024` 字节 payload 的内容。
- 普通固件升级理论上不会清掉这块参数区。
- 如果使用整片 flash 擦除类烧录工具，可能会把这块参数一起清掉。
