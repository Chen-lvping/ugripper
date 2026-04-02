# RGB / Stereo / IMU Calibration Payload Summary

本文档对应当前 `UMI calibration data format version = 0x00010000` 的 `1024-byte` payload。
下面列出的字段就是当前会被写入夹爪 HMI 标定 bin 的字段集合，包含：

- RGB 主相机参数
- 双目 `cam0` / `cam1` 参数
- `cam0` / `cam1` 到 `imu0` 的外参矩阵与时间偏移
- `imu0` 更新率、离散噪声密度与随机游走
- cam / imu 残差统计

当前 payload 结构版本：

```yaml
calibration_data_format_version: 0x00010000
```

## 1. RGB 主相机

```yaml
camera_model_enum: 1
distortion_coeffs: [-0.08088456630446411, 0.003074053759177666, -0.004561233975813844, 0.0006520059107677026]
intrinsics: [833.5170139130338, 833.4122891914288, 956.1183277958914, 547.0810775920527]
resolution: [1920.0, 1080.0]
```

说明：

- `camera_model_enum = 1` 对应 `pinhole + equidistant`
- `intrinsics` 顺序固定为 `[fx, fy, cx, cy]`
- `resolution` 顺序固定为 `[width, height]`

## 2. 双目参数

```yaml
cam0:
  target_camera_model_enum: 1
  focal_length: [292.2042946009311, 292.12869419082494]
  principal_point: [290.82141621911137, 206.99925418270405]
  distortion_coefficients: [-0.03021296825458688, -0.007363766071583267, 0.002503300559774206, -0.0011163281632751711]

cam1:
  target_camera_model_enum: 1
  focal_length: [294.0759320631834, 294.0176604407103]
  principal_point: [322.5762989238057, 205.6584778924258]
  distortion_coefficients: [-0.02923123799220039, -0.012092716435796225, 0.007347984424999385, -0.0026971079140426803]
```

## 3. 相机到 IMU 外参与双目基线标量

```yaml
T_ic_cam0_to_imu0:
  - [-0.00744193, 0.49690602, 0.86777245, 0.19986122]
  - [-0.99991021, 0.00597347, -0.01199568, -0.10455597]
  - [-0.01114434, -0.8677838, 0.49681695, 0.20295772]
  - [0.0, 0.0, 0.0, 1.0]
timeshift_cam0_to_imu0: -0.02365692619181131

T_ic_cam1_to_imu0:
  - [-0.00159787, 0.49417093, 0.8693633, 0.19993157]
  - [-0.99996961, 0.00584388, -0.00515974, -0.03910082]
  - [-0.00763025, -0.86934512, 0.49414657, 0.20333428]
  - [0.0, 0.0, 0.0, 1.0]
timeshift_cam1_to_imu0: -0.023657424638209523

baseline_norm: 0.06545627221013298
```

说明：

- 当前 payload 写入的是 `T_ic`，即 `cam -> imu`
- 当前 payload 保存 `baseline_norm` 标量，未单独保存整块 `Baseline (cam0 to cam1)` 4x4 矩阵

## 4. IMU0 参数

```yaml
imu0:
  update_rate: 200.0
  accelerometer:
    noise_density_discrete: 0.0282842712474619
    random_walk: 0.0002
  gyroscope:
    noise_density_discrete: 0.002262741699796952
    random_walk: 2.2e-05
```

说明：

- 当前 payload 写入的是离散噪声密度 `noise_density_discrete`
- 原始文件中的连续噪声密度 `noise_density`、`T_ib` 与 `imu0 -> imu0 time offset` 未单独写入 payload

## 5. 残差统计

```yaml
residuals:
  reprojection_error_cam0_px:
    mean: 0.15386934743142472
    median: 0.12493305538233092
    std: 0.14266912006815546
  reprojection_error_cam1_px:
    mean: 0.13987290918159923
    median: 0.11296134553561135
    std: 0.13600162794033593
  gyroscope_error_imu0_rad_s:
    mean: 0.0002113544013155163
    median: 9.583410207543815e-05
    std: 0.00038161111000160626
  accelerometer_error_imu0_m_s2:
    mean: 0.00017347497149546018
    median: 9.347762578303628e-05
    std: 0.0003699893310083964
```

## 6. 当前 payload 未单独存储的原始结果

以下内容仍可从原始 `output-results-imucam.txt` 中获取，但当前 `0x00010000` payload 未单独占位存储：

- `Normalized Residuals`
- `T_ci (imu0 to cam0 / cam1)`
- `Baseline (cam0 to cam1)` 4x4 矩阵
- `Gravity vector in target coords`
- IMU 连续噪声密度 `Noise density`
- `T_ib (imu0 to imu0)` 与 `time offset with respect to IMU0`
