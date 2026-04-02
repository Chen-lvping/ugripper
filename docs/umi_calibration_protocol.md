# UMI Calibration Read/Write Protocol

本文档描述当前仓库约定的 UMI 标定参数与 SN 读写协议。

## 1. Bin Byte Mapping

当前 `calibration data format version = 0x00010000` 的 payload 固定为 `1024 byte`，按 `#pragma pack(push, 1)` 严格对齐。

### 1.1 整体块布局

| Offset | Size | Block | 当前实际写入参数 |
| --- | --- | --- | --- |
| `0x000` | `32` | `header` | `magic`、`dataFormatVersion`、`payloadSize`、`headerSize`、`validFields` |
| `0x020` | `48` | `rgbCamera` | RGB 主相机 `camera_model_enum`、`distortion_coefficients[4]`、`intrinsics[4]`、`resolution[2]` |
| `0x050` | `48` | `stereoCam0` | 双目 `cam0` 的 `camera_model_enum`、`focal_length[2]`、`principal_point[2]`、`distortion_coefficients[4]` |
| `0x080` | `48` | `stereoCam1` | 双目 `cam1` 的 `camera_model_enum`、`focal_length[2]`、`principal_point[2]`、`distortion_coefficients[4]` |
| `0x0B0` | `160` | `extrinsics` | `T_ic_cam0_to_imu0[16]`、`timeshift_cam0_to_imu0`、`T_ic_cam1_to_imu0[16]`、`timeshift_cam1_to_imu0`、`baseline_norm` |
| `0x150` | `32` | `imu0` | `update_rate`、`accelerometer_noise_density_discrete`、`accelerometer_random_walk`、`gyroscope_noise_density_discrete`、`gyroscope_random_walk` |
| `0x170` | `64` | `residuals` | `cam0/cam1 reprojection` 与 `imu gyro/acc` 的 `mean/median/stddev` |
| `0x1B0` | `592` | `reserved` | 当前保留；旧解析器不得依赖其内容 |

### 1.2 Block 内字段映射

#### `header` @ `0x000`

| Offset | Size | 字段 | 说明 |
| --- | --- | --- | --- |
| `0x000` | `4` | `magic` | 固定 `UCAL` |
| `0x004` | `4` | `dataFormatVersion` | 当前固定 `0x00010000` |
| `0x008` | `2` | `payloadSize` | 内部有效数据长度；传输层仍需补满 `1024 byte` |
| `0x00A` | `2` | `headerSize` | 当前固定 `32` |
| `0x00C` | `4` | `validFields` | 各块有效位掩码 |
| `0x010` | `16` | `reserved` | 预留 |

#### `rgbCamera` @ `0x020`

| Offset | Size | 字段 | 来源 |
| --- | --- | --- | --- |
| `0x020` | `4` | `cameraModelEnum` | `camera_model_enum` |
| `0x024` | `16` | `distortionCoefficients[4]` | `distortion_coeffs` |
| `0x034` | `16` | `intrinsics[4]` | `[fx, fy, cx, cy]` |
| `0x044` | `8` | `resolution[2]` | `[width, height]` |
| `0x04C` | `4` | `reserved` | 预留 |

#### `stereoCam0` @ `0x050`

| Offset | Size | 字段 | 来源 |
| --- | --- | --- | --- |
| `0x050` | `4` | `cameraModelEnum` | `cam0.target_camera_model_enum` |
| `0x054` | `8` | `focalLength[2]` | `cam0.focal_length` |
| `0x05C` | `8` | `principalPoint[2]` | `cam0.principal_point` |
| `0x064` | `16` | `distortionCoefficients[4]` | `cam0.distortion_coefficients` |
| `0x074` | `12` | `reserved[3]` | 预留 |

#### `stereoCam1` @ `0x080`

| Offset | Size | 字段 | 来源 |
| --- | --- | --- | --- |
| `0x080` | `4` | `cameraModelEnum` | `cam1.target_camera_model_enum` |
| `0x084` | `8` | `focalLength[2]` | `cam1.focal_length` |
| `0x08C` | `8` | `principalPoint[2]` | `cam1.principal_point` |
| `0x094` | `16` | `distortionCoefficients[4]` | `cam1.distortion_coefficients` |
| `0x0A4` | `12` | `reserved[3]` | 预留 |

#### `extrinsics` @ `0x0B0`

| Offset | Size | 字段 | 来源 |
| --- | --- | --- | --- |
| `0x0B0` | `64` | `tIcCam0ToImu0[16]` | `T_ic_cam0_to_imu0` |
| `0x0F0` | `4` | `timeshiftCam0ToImu0` | `timeshift_cam0_to_imu0` |
| `0x0F4` | `12` | `reserved0[3]` | 预留 |
| `0x100` | `64` | `tIcCam1ToImu0[16]` | `T_ic_cam1_to_imu0` |
| `0x140` | `4` | `timeshiftCam1ToImu0` | `timeshift_cam1_to_imu0` |
| `0x144` | `4` | `baselineNorm` | `baseline_norm` |
| `0x148` | `8` | `reserved1[2]` | 预留 |

#### `imu0` @ `0x150`

| Offset | Size | 字段 | 来源 |
| --- | --- | --- | --- |
| `0x150` | `4` | `updateRate` | `imu0.update_rate` |
| `0x154` | `4` | `accelerometerNoiseDensityDiscrete` | `imu0.accelerometer.noise_density_discrete` |
| `0x158` | `4` | `accelerometerRandomWalk` | `imu0.accelerometer.random_walk` |
| `0x15C` | `4` | `gyroscopeNoiseDensityDiscrete` | `imu0.gyroscope.noise_density_discrete` |
| `0x160` | `4` | `gyroscopeRandomWalk` | `imu0.gyroscope.random_walk` |
| `0x164` | `12` | `reserved[3]` | 预留 |

#### `residuals` @ `0x170`

每组统计块固定 `16 byte`，布局为 `mean / median / stddev / reserved`。

| Offset | Size | 字段 | 来源 |
| --- | --- | --- | --- |
| `0x170` | `16` | `reprojectionErrorCam0Px` | `residuals.reprojection_error_cam0_px` |
| `0x180` | `16` | `reprojectionErrorCam1Px` | `residuals.reprojection_error_cam1_px` |
| `0x190` | `16` | `gyroscopeErrorImu0RadS` | `residuals.gyroscope_error_imu0_rad_s` |
| `0x1A0` | `16` | `accelerometerErrorImu0MS2` | `residuals.accelerometer_error_imu0_m_s2` |

### 1.3 当前 payload 已存与未存

当前 `0x00010000` payload 已存：

- RGB 主相机模型、畸变、内参与分辨率
- 双目 `cam0/cam1` 模型、焦距、主点、畸变
- `cam0/cam1 -> imu0` 外参矩阵与时间偏移
- `baseline_norm`
- IMU0 更新率、离散噪声密度与随机游走
- cam / imu 残差统计 `mean/median/stddev`

当前未单独占位存储、仍保留在原始标定文件中的内容：

- `Normalized Residuals`
- `T_ci (imu -> cam)` 矩阵
- `Baseline (cam0 -> cam1)` 4x4 矩阵
- `Gravity vector in target coords`
- IMU 连续噪声密度 `Noise density`
- `T_ib (imu0 to imu0)` 与 `time offset with respect to IMU0`

## 2. 版本定义

- `HMI firmware communication protocol version`: `V1.1`
  说明：该版本是夹爪 HMI 固件串口命令集版本。
- `calibration data format version`: `0x00010000`
  说明：该版本写入 `header.dataFormatVersion`，表示当前 `1024-byte` bin 数据结构版本。

## 3. 基本约定

- 串口参数：`115200 8N1`
- Host -> HMI 帧头：`0x5A 0x5A`
- HMI -> Host 帧头：`0xA5 0xA5`
- SN：固定 `32-byte` 字段，当前 SN 文本按 ASCII 存放，不足部分补 `0x00`
- 标定参数：固定 `1024-byte payload`
- 标定参数读写按 `16-byte` chunk 分包，共 `64` 包，包序号范围 `0x00..0x3F`

校验口径：

- 命令帧、状态帧、标定写入帧：校验位为“最后 1 byte 之前所有前导字节”的 XOR
- 标定读回数据帧 `A5 A5 <seq> <16-byte chunk> XOR`：校验位为 `16-byte chunk` 的 XOR

## 4. 命令一览

| 功能 | 方向 | 命令/索引 | 载荷 | 说明 |
| --- | --- | --- | --- | --- |
| 读 SN | Host -> HMI | `5A 5A 52 53 00 00 XOR` | 无 | 请求当前 SN 文本 |
| 写 SN 准备 | Host -> HMI | `5A 5A 57 53 4E 43 XOR` | 无 | 进入 SN 写入 |
| 写 SN 数据 | Host -> HMI | `2 x (5A 5A <16-byte sn chunk> XOR)` | `32-byte SN 字段` | 连续写入两个 `16-byte` chunk |
| 开始写标定参数 | Host -> HMI | `5A 5A 57 5A 72 6F XOR` | 无 | 进入 `1024-byte` 标定参数写入模式 |
| 写标定参数分包 | Host -> HMI | `5A 5A <seq> <16-byte chunk> XOR` | `seq + 16-byte chunk` | `seq` 从 `0x00` 开始 |
| 停止写标定参数 | Host -> HMI | `5A 5A 53 74 6F 70 5F 57 72 69 74 65 49 6E 44 61 74 61 00 XOR` | 无 | 退出写入模式 |
| 中止写标定参数 | Host -> HMI | `5A 5A 41 62 6F 72 74 57 72 69 74 65 49 6E 44 61 74 61 00 XOR` | 无 | 当上次写标定未正常退出、后续读写返回 `F3/FE` 时可发送一次恢复固件写入状态 |
| 读标定参数范围 | Host -> HMI | `5A 5A 52 5A 00 3F XOR` | 无 | 请求 `0x00..0x3F` 共 `64` 个 chunk |
| 读标定参数单包 | Host -> HMI | `5A 5A 52 5A 00 <seq> XOR` | 无 | 请求指定 `seq` 的单个 chunk |

## 5. 响应约定

### 5.1 通用状态响应

```text
A5 A5 <token> <status> 00 00 A5 XOR
```

- `<status>`：
  - `0x54`: 正确状态
  - `0x88`: 地址超限
  - `0xF1`: 检索字错误
  - `0xF2`: 功能字错误
  - `0xF3`: 数据缺帧
  - `0xF4`: 指令无效
  - `0xFE`: 写标定当前包零数据校验错误
  - `0xFF`: 校验码错误

### 5.2 读 SN 响应

```text
A5 A5 <16-byte sn chunk> XOR
```

- Host 侧会连续接收两份 `16-byte` 数据帧并拼回 `32-byte` SN 字段
- 当前现场 SN 文本示例：`DAG91126320001D6`

### 5.3 读标定参数响应

```text
A5 A5 <seq> <16-byte chunk> XOR
```

- `<seq>` 当前实测为 `0x00..0x3F`
- 单次范围读取会连续返回 `64` 个 `20-byte` 数据帧，总计 `1280 byte`
- Host 侧按 `seq=0..63` 组装成完整 `1024-byte payload`

## 6. 代码落点

- 协议 builder / parser：
  [gripper_hmi_protocol.h](/home/ubuntu/proj/proj/ugripper_v2/src/gripper_hmi/include/gripper_hmi_protocol.h)
- 标定结构定义：
  [gripper_hmi_calibration_data.h](/home/ubuntu/proj/proj/ugripper_v2/src/gripper_hmi/include/gripper_hmi_calibration_data.h)
- HMI 驱动读写：
  [gripper_hmi_driver.h](/home/ubuntu/proj/proj/ugripper_v2/src/gripper_hmi/include/gripper_hmi_driver.h)
- 定向测试 CLI：
  [gripper_hmi_test.cpp](/home/ubuntu/proj/proj/ugripper_v2/src/gripper_hmi/test/gripper_hmi_test.cpp)

## 7. 当前实现说明

- 标定 payload 固定 `1024 byte`；`header.payloadSize` 表示内部有效数据长度，但传输层写入时仍必须补满 `64 x 16B`
- 写入完成后需要保留固件刷写时间，再进行读取或下一个命令
- 当前 `0xFE` 视为“本次写入 chunk 的零数据校验错误”，Host 会优先重发当前 chunk；若连续出现 `0xFE`，或后续读 SN / 读标定返回 `0xF3/0xFE`，驱动会先发送一次 `AbortWriteInData` 再重试，尝试把固件从未完成的写标定状态机中拉回
- 当前 `gripper_hmi_test --read-calib` 会打印完整 RGB / stereo / extrinsics / IMU / residuals 摘要，便于现场确认 bin 中实际写入内容
