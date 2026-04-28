# Ugripper Current Status Overview

本文档用于给 agent 快速读取当前仓库状态。
它只保留当前仍成立的事实、边界和维护口径。
历史过程、详细设计推导和逐轮测试推进，交给其他文档承载。

## 1. 当前仓库一句话
- 截至 `2026-04-23`，`ugripper` 已经是当前正式源码真源，不再只是等待并仓的中间仓库。
- 当前仓库同时承载主实现、测试入口、打包部署资产和维护文档。
- 后续若作为 `pp_main` submodule 使用，应把本仓视为 `ugripper` 的正式来源。

## 2. 当前真源目录

### 2.1 主实现真源
- `standalone/UgripperRuntime`
- `standalone/CameraRecorder`
- `standalone/SensorRecorder`
- `standalone/GripperHmiTool`
- `src/utils`
- `src/third_party/mcap_builder`

### 2.2 测试真源
- `test/src`
- `test/scripts`

### 2.3 部署与交付真源
- `run_record.sh`
- `build_deb.sh`
- `usb_updater_build.sh`
- `pack_script/`
- `auto_update/`
- `auto_calibration/`
- `config/`
- `audio/`
- `audio_en/`

### 2.4 文档真源
- `docs/agent/overview.md`
- `docs/agent/current-status.md`
- `docs/ugripper-refactor-architecture.md`
- `docs/ppmain-ugripper-test-summary.md`
- `docs/CHANGELOG.md`
- `docs/REFACTOR_LOG.md`

### 2.5 非当前实现真源
- 旧 `src/record_runtime`
- 旧 `src/camera_recorder`
- 旧 `src/sensor_recorder`
- 旧 `src/gripper_hmi`
- `build/`
- `tmp/`
- `.local-deps/`

### 2.6 已归档历史文档
- `docs/archive/2026-refactor-history/`
- `docs/archive/2026-refactor-history/baseline/*.md`

## 3. 当前系统一句话
- 当前系统默认以单机双手、本地控制方式运行。
- 主控制链路当前以 `run_record.sh -> /opt/ugripper/bin/UgripperRuntime/UgripperRuntime` 作为安装入口。
- 默认录制产物为 **2 路主相机 + 2 路 stereo + 4 路触觉相机 + 左右两份传感器 MCAP**。
- HMI 按键、RGB 灯效、提示音、pre/post 音频、停录校验和双键关机请求都已纳入当前运行时。

## 4. 当前安装布局与入口
- 安装目录：`/opt/ugripper`
- 主服务：`pack_script/ugripper.service`
- 主入口：`/opt/ugripper/run_record.sh`
- 主运行时：`/opt/ugripper/bin/UgripperRuntime/UgripperRuntime`
- 数据目录：`/mnt/data_disk/<device_sn_lower>/data`
- 持久化标定目录：`/etc/ugripper/config/calibration`
- U 盘升级入口：`auto_update/usb_auto_update.sh`
- 校准执行入口：`auto_calibration/run_calibration.sh`

### 4.1 当前关键配置入口
- 环境变量入口：`/etc/environment`
- 运行时主配置：`config/camera_recorder.yaml`
- fallback 标定样例：`config/fakeCamCalib.json`
- 持久化标定：`/etc/ugripper/config/calibration/calibration.json`
- 导入标定留档：`/etc/ugripper/config/calibration/imported/<DEVICE_SN>/<STAMP>`
- U 盘配置入口：`config.txt`
- 校准触发文件：`/mnt/data_disk/calibration.txt`

### 4.2 当前关键环境变量
| 键 | 当前用途 | 主要读取方 | 主要写入方 |
| --- | --- | --- | --- |
| `DEVICE_SN` | 设备 SN、日志命名、标定导入目录选择、episode 元数据 | `run_record.sh`、runtime、标定导入脚本 | 当前仓库内未见主链写入逻辑 |
| `UGRIPPER_LANG` | 音频语言选择 | runtime、`audio_play.py`、`usb_auto_update.sh` | `usb_auto_update.sh` |
| `CAMERA_CODEC` | camera 编码器口径 `h264/h265` | runtime、camera recorder、`usb_auto_update.sh` | `usb_auto_update.sh` |

### 4.3 当前运行时控制文件
- stereo 控制文件：`/tmp/umi_stereo_camera_control.json`
- stereo 状态文件：`/tmp/umi_stereo_camera_status.json`
- 关机触发文件：`/tmp/umi_shutdown_request`
- 音频命令 FIFO：`/tmp/umi_audio_pipe`
- 音频 ready 标记：`/tmp/umi_audio_ready`
- 录制锁文件：`/tmp/umi_recording.lock`

## 5. 当前模块总览
| 模块 | 入口 | 当前职责 | 关键输入/输出 |
| --- | --- | --- | --- |
| systemd 主服务 | `pack_script/ugripper.service` | 拉起录制服务 | `/opt/ugripper/run_record.sh` |
| 薄壳启动脚本 | `run_record.sh` | 等待 `/mnt/data_disk` 可写、维护日志同步、拉起 runtime | `/tmp/umi_sys_<sn>_<date>.log`、`/mnt/data_disk/logs/...` |
| 主运行时 | `bin/UgripperRuntime/UgripperRuntime` | HMI 状态机、LED、提示音、camera/sensor 子进程管理、停录校验、关机请求 | episode 目录、`/tmp/umi_shutdown_request` |
| 相机录制 | `bin/CameraRecorder/CameraRecorder` | 主摄/触觉录制；`--stereo-daemon` 常驻预热、热插拔恢复与 session finalize | 8 路 `mkv` |
| 传感器录制 | `bin/SensorRecorder/SensorRecorder` | 左右 IMU/encoder 录制并分别输出 MCAP | `sensor_data_left.mcap`、`sensor_data_right.mcap` |
| HMI 类库/工具 | `standalone/GripperHmiTool` | 按键读取、灯效生成、RGB 指令发送、SN/标定参数读写 | 按键快照、RGB 指令、SN/标定参数 |
| 音频播放 | `bin/UgripperRuntime/audio/audio_play.py` | 绑定 USB 耳机或回退默认声卡，播放提示音 | `/tmp/umi_audio_pipe` |
| 音频采集 | `audio/record_usb_audio.py` | pre/post 音频录制 | 临时 wav 文件 |
| USB 升级/导入 | `auto_update/usb_auto_update.sh` | `deb` 升级/重装、配置导入、标定导入、encoder 校准触发 | `/etc/environment`、`calibration.json` |
| 校准执行 | `auto_calibration/run_calibration.sh` | 左右编码器 zeroing | `bin/SensorRecorder/zeroing` |

## 6. 当前实现结构结论
- `UgripperRuntime` 已按 `app 壳 + process boundary + control plane` 落地到 `standalone/UgripperRuntime`
- `CameraRecorder` 已按 `app 壳 + camera domain` 落地到 `standalone/CameraRecorder`
- `SensorRecorder` 已按 `protocol + domain + app/runtime 装配` 落地到 `standalone/SensorRecorder`
- `GripperHmiTool` 已按 `protocol + led effects + driver + tool` 落地到 `standalone/GripperHmiTool`
- 正式 host-only 单测与板端 analyzer / script 入口已迁回 `test/`

## 7. 当前构建与打包状态

### 7.1 构建
- 顶层 `CMakeLists.txt` 当前已切到 `standalone/` + `src/utils` + `test`
- 当前可以直接构建本仓目标，不再依赖旧 `src/*` 应用目录
- 当前顶层直接要求系统提供：
  - `yaml-cpp`
  - `nlohmann_json`
- 当前仓库已不再保留 `.local-deps/nlohmann` 的头文件 fallback
- 顶层当前默认不再强制构建 `src/third_party/mcap_builder`
- 若确实需要该可选目标，需显式传 `-DUGRIPPER_ENABLE_MCAP_BUILDER=ON`

### 7.2 主包打包
- 主包入口：`build_deb.sh`
- ARM 容器内一键出包入口：`scripts/build_arm_deb_in_pp_arm_dev.sh`
- 当前主包文件名模式：`ugripper_<version>_arm64.deb`
- 当前版本变量来源仍是脚本变量，而不是外部发布系统：
  - `BASE_VERSION`
  - `VERSION_SUFFIX`
  - `VERSION`
- 当前脚本默认值仍是：
  - `BASE_VERSION=1.2.8`
  - `VERSION_SUFFIX=''`
  - 因此默认产物名会是 `ugripper_1.2.8_arm64.deb`
- 这里的 `1.2.8` 当前只能视为“本仓脚本默认基线值 / 占位值”，不能直接等同于正式发布版本号
- 当前推荐口径不是“直接在宿主机跑 `build_deb.sh`”，而是优先：
  - 在 `pp-arm-dev` 容器内先产出 ARM `build/`
  - 再调用 `build_deb.sh -q` 组包
- 当前仓库已自带 ARM toolchain 文件：`cmake/arm-linux-toolchain.cmake`
- 当前打包优先使用：
  - `standalone/*` 编译产物
  - `bin/<AppName>/...` 安装布局
- 当前已支持优先从 Nexus raw 下载 `.venv` 压缩包，下载解压后再打包
- 当前支持显式指定：
  - `PACKAGED_BUILD_DIR`
  - `PACKAGED_VENV_URL`
  - `PACKAGED_VENV_SOURCE`
- `build_deb.sh` 标准模式当前只构建主包必需目标：
  - `CameraRecorder`
  - `SensorRecorder`
  - `zeroing`
  - `GripperHmiTool`
  - `UgripperRuntime`
- `build_deb.sh` 当前会在 staging 前强校验：
  - `.venv/bin/python3` 的 ELF 架构
  - 主包 5 个核心二进制的 ELF 架构
- 当前已修复 `.venv` 下载/解压缓存与 manifest 残留误入 `deb` 的 staging 污染问题

#### 7.2.1 推荐出包流程
- 推荐命令：`./scripts/build_arm_deb_in_pp_arm_dev.sh`
- 当前这条路径的定位是“给日常维护者和 agent 的默认安全路径”
- 它默认在 `pp-arm-dev` 容器内部执行，并依次完成：
  - 设置 `PKG_CONFIG_PATH=/usr/lib/aarch64-linux-gnu/pkgconfig`
  - 用本仓 `cmake/arm-linux-toolchain.cmake` 把 5 个主包必需目标编到 `build/arm_container_release`
  - 在容器内调用 `PACKAGED_BUILD_DIR=build/arm_container_release ./build_deb.sh -q`
  - 如果脚本以 root 运行，把 `build/`、`temp_build_deb*` 和生成的 `.deb` 统一 `chown` 回仓库目录 owner
- 当前一键脚本会把 `BASE_VERSION` / `VERSION_SUFFIX` / `VERSION` 透传给 `build_deb.sh`，因此最终 `.deb` 文件名与主打包脚本保持同一口径，不再写死版本号
- 当前一键脚本默认要求在容器环境内运行；若确实要在非容器环境复用，需要显式传 `ALLOW_HOST_RUN=1`
- 当前这样设计的原因是：
  - `build_deb.sh` 自己不负责 cross toolchain 配置
  - `build_deb.sh` 标准模式默认是“直接本机 cmake 编译”
  - 当前包架构固定是 `arm64`
  - 所以在 `x86_64` 宿主机上直接跑 `./build_deb.sh` 标准模式，会生成错误架构二进制并在后续 ELF 校验阶段失败

#### 7.2.2 `build_deb.sh` 当前实际流程
- `build_deb.sh` 固定打 `arm64` 包，产物名形如 `ugripper_<version>_arm64.deb`
- 脚本内部按 5 个阶段执行：
  - `1/5` 初始化 `temp_build_deb`
  - `2/5` 编译或校验主包 5 个核心二进制
  - `3/5` 组装 staging，并同步 `.venv`
  - `4/5` 拷贝 `systemd` / `udev` / `DEBIAN` 控制文件并做变量替换
  - `5/5` 调用 `dpkg-deb` 生成最终包
- 其中 `.venv` 处理口径是：
  - 默认优先从内置 `PACKAGED_VENV_URL` 下载 Nexus raw 归档
  - 下载后解压到临时辅助目录
  - 自动定位实际 `.venv` 根目录
  - 校验 `bin/python3` 的 ELF 架构必须匹配 `arm64`
  - 再单独 rsync 到 staging 的 `/opt/ugripper/.venv`
- 二进制处理口径是：
  - 只认 5 个主包目标：`CameraRecorder`、`SensorRecorder`、`zeroing`、`GripperHmiTool`、`UgripperRuntime`
  - 会逐个用 `file` 检查 ELF 架构
  - 不匹配则直接失败，不允许混入 `x86_64` 产物
- staging 组装口径是白名单式，不再整仓库粗暴拷贝：
  - `run_record.sh`
  - `scripts/`
  - `auto_update/`
  - `auto_calibration/`
  - `bin/*` 下的 5 个核心二进制及对应资源
  - `py_script/` 下当前保留的少量音频脚本
  - `99-fixed-usb-map.rules`
  - `99-serial.rules`
- 当前已明确不会进包的临时内容包括：
  - `.packaged_venv_download`
  - `.packaged_venv_extract`
  - `.auto_release_ugripper.manifest`

#### 7.2.3 `build_deb.sh` 的两种运行方式
- 标准模式：`./build_deb.sh`
  - 会先本地编译 5 个核心目标，再组包
  - 只适合在 `arm64` 原生环境使用
  - 对当前日常开发机 `x86_64` 场景不推荐
- 快速模式：`./build_deb.sh -q`
  - 跳过编译，只复用现有 `PACKAGED_BUILD_DIR`
  - 仍然会做 5 个二进制和 `.venv` 的架构校验
  - 这是当前 `x86_64 + 容器交叉编译` 的正确组包模式

#### 7.2.4 当前手动出包方法
- 方法 A：推荐，整套自动
  - 前提：已进入 `pp-arm-dev` 容器，并位于仓库根目录
  - 命令：`cd /home/dm/proj/pp-main/standalone/UGripper && ./scripts/build_arm_deb_in_pp_arm_dev.sh`
- 方法 B：手动拆成“先编译，再组包”
  - 适用：要单独控制容器内编译步骤，或排查编译依赖问题
  - 步骤 1：在 `pp-arm-dev` 容器内手动编译
    - `export PKG_CONFIG_PATH=/usr/lib/aarch64-linux-gnu/pkgconfig:$PKG_CONFIG_PATH`
    - `cmake -S . -B build/arm_container_release -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF -DUGRIPPER_ENABLE_MCAP_BUILDER=OFF -DCMAKE_TOOLCHAIN_FILE=$PWD/cmake/arm-linux-toolchain.cmake`
    - `cmake --build build/arm_container_release --target CameraRecorder SensorRecorder zeroing GripperHmiTool UgripperRuntime --parallel 8`
  - 步骤 2：在仓库根目录组包
    - 默认走 Nexus `.venv`：`PACKAGED_BUILD_DIR=build/arm_container_release ./build_deb.sh -q`
    - 若要强制改用本地 `.venv`：`PACKAGED_VENV_URL='' PACKAGED_VENV_SOURCE=/path/to/.venv PACKAGED_BUILD_DIR=build/arm_container_release ./build_deb.sh -q`
- 方法 C：原生 `arm64` 环境一次性编译加打包
  - 适用：板子或原生 ARM 开发机
  - 命令：`./build_deb.sh`
  - 注意：这条路径当前不是宿主机主推荐路径，只在本机本来就是 `arm64` 时成立

#### 7.2.5 当前 agent 需要知道的操作性结论
- 如果用户只说“重新出个包”，默认应优先执行 `./scripts/build_arm_deb_in_pp_arm_dev.sh`
- 如果用户说“不要重编，只重新组包”，默认应执行 `PACKAGED_BUILD_DIR=build/arm_container_release ./build_deb.sh -q`
- 如果用户说“本地环境不可信，优先用归档 Python 环境”，当前默认行为已经满足，不需要额外传参
- 如果用户说“不要走 Nexus，改用手头本地 `.venv`”，才显式传 `PACKAGED_VENV_URL=''`
- 如果用户在 `x86_64` 宿主机上直接跑 `./build_deb.sh` 失败，优先怀疑不是代码问题，而是走错了出包路径
- 如果用户是通过容器一键出包，且脚本以 root 运行，当前脚本会默认把构建产物所有权回收给仓库目录 owner
- 如有特殊工作区映射或 CI 用户，允许显式覆盖：
  - `HOST_UID=<uid>`
  - `HOST_GID=<gid>`
- 如需在非容器环境强制复用一键脚本，必须显式传 `ALLOW_HOST_RUN=1`
- 如果用户要“正式出包”而不是“本地验证出包”，不要默认沿用 `BASE_VERSION=1.2.8`；应明确传入：
  - `VERSION_SUFFIX=+<tag>`
  - 或 `VERSION=<full-version>`

#### 7.2.6 当前 ARM 出包容器所需环境
- 当前默认容器名：`pp-arm-dev`
- 当前脚本假设调用者已经进入开发容器；脚本不会负责创建容器或从宿主机 `docker exec`
- 当前仓库已提供环境初始化脚本：`scripts/setup_arm_build_env.sh`
- 该脚本是“环境补齐入口”，不是一键出包主链的必经步骤
- 若要手动补容器环境，推荐在容器内直接执行：`bash ./scripts/setup_arm_build_env.sh`
- 容器内依赖分两层理解：

第一层：镜像必须预装，脚本不会补
- `bash`
- `apt-get`
- `cmake`
- `make` 或可被 `cmake --build` 调起的默认构建工具
- `pkg-config`
- `dpkg-deb`
- `rsync`
- `curl`
- `tar`
- `file`
- `sed`
- `grep`
- `find`
- ARM 交叉工具链：
  - `aarch64-linux-gnu-gcc`
  - `aarch64-linux-gnu-g++`
  - `aarch64-linux-gnu-as`
  - `aarch64-linux-gnu-ld`
  - `aarch64-linux-gnu-ar`
- 当前本仓 `cmake/arm-linux-toolchain.cmake` 默认就假设以上交叉工具链在 `/usr/bin`

第二层：环境初始化脚本可安装的开发依赖
- `scripts/setup_arm_build_env.sh` 可一并安装基础构建工具和 ARM 交叉工具链
- `libyaml-cpp-dev:arm64`
- `nlohmann-json3-dev`
- `liblz4-dev:arm64`
- `libserialport-dev:arm64`
- `libusb-1.0-0-dev:arm64`
- `libfmt-dev:arm64`
- `libspdlog-dev:arm64`
- `libgstreamer1.0-dev:arm64`
- `libgstreamer-plugins-base1.0-dev:arm64`

主包运行依赖新增日志后端运行库：
- `libfmt8`
- `libspdlog1`
- 原因：当前 `src/utils` 已切到 `pp_main` 同款 spdlog 后端，`CameraRecorder`、`SensorRecorder`、`zeroing`、`GripperHmiTool`、`UgripperRuntime` 都会动态依赖 `libspdlog.so.1` 与 `libfmt.so.8`

第三层：和当前出包策略绑定的外部条件
- 容器内需要能访问 Nexus raw，因为 `build_deb.sh` 默认会下载归档 `.venv`
- 如果不希望依赖 Nexus，组包时要显式传：
  - `PACKAGED_VENV_URL=''`
  - `PACKAGED_VENV_SOURCE=<local arm64 .venv path>`

第四层：当前最小可用判断
- 若只想判断容器是否满足当前 ARM 出包前提，至少应能在容器内通过：
  - `command -v aarch64-linux-gnu-gcc`
  - `command -v cmake`
  - `command -v dpkg-deb`
  - `command -v rsync`
  - `command -v curl`
  - `command -v tar`
  - `command -v file`
  - `bash ./scripts/setup_arm_build_env.sh --help`
  - `source .local-deps/arm-build-env.sh && pkg-config --exists fmt && pkg-config --exists spdlog`

### 7.3 Python 运行环境
- 当前默认 Python 运行环境由 `.venv` 随主包一起交付
- 当前固定 Python 版本来自 `pyproject.toml`：`3.11.15`
- 当前本地默认 `.venv` 真源路径：
  - 仓库工作目录：`<repo_root>/.venv`
  - manifest：`<repo_root>/.venv/.ugripper-venv-manifest.json`
- `scripts/build_runtime_venv.sh` 的默认输出路径也是：
  - `TARGET_DIR=<repo_root>/.venv`
- 当前打包链里的 `.venv` 路径口径：
  - 本地默认来源：`PACKAGED_VENV_SOURCE=.venv`
  - 包内最终落点：`/opt/ugripper/.venv`
  - staging 临时落点：`temp_build_deb/opt/ugripper/.venv`
- 当前默认打包策略偏向“优先下载已归档 `.venv` 后再出包”，减少本地环境不一致风险

#### 7.3.1 当前 Nexus `.venv` 归档口径
- raw 仓库基名：`ugripper-v2-uv-venv`
- 当前版本目录：`py311-v1`
- 当前默认归档文件：`ugripper_venv_20260423_143813.tar.gz`
- 当前脚本内置完整 URL：
  - `http://nexus.dmrobot.com:8081/repository/dmrobot_raw_hosted/ugripper-v2-uv-venv/py311-v1/ugripper_venv_20260423_143813.tar.gz`
- 这三层的语义分别是：
  - `ugripper-v2-uv-venv`：当前 `.venv` 包名 / 归档名
  - `py311-v1`：当前 `.venv` 版本号目录
  - `ugripper_venv_20260423_143813.tar.gz`：这次实际上传的归档文件名

#### 7.3.2 当前 `.venv` 的使用顺序
- 若 `PACKAGED_VENV_URL` 保持默认值：
  - `build_deb.sh` 会先下载 Nexus raw 中的归档 `.venv`
  - 解压到 `temp_build_deb_aux/.packaged_venv_extract/`
  - 自动解析出实际 `.venv` 根目录
  - 再同步到 `temp_build_deb/opt/ugripper/.venv`
- 若显式设置 `PACKAGED_VENV_URL=''`：
  - `build_deb.sh` 不下载归档
  - 直接使用 `PACKAGED_VENV_SOURCE` 指向的本地 `.venv`

#### 7.3.3 当前维护动作
- 若要重建本地可打包 `.venv`：
  - 执行 `./scripts/build_runtime_venv.sh`
- 若要给重建出的 `.venv` 写版本标记：
  - 执行 `VENV_VERSION_TAG=<tag> ./scripts/build_runtime_venv.sh`
- 当前 agent 在汇报 `.venv` 版本时，应优先同时说明：
  - Python 版本：`3.11.15`
  - raw 包名：`ugripper-v2-uv-venv`
  - 当前版本目录：`py311-v1`
  - 当前默认归档文件：`ugripper_venv_20260423_143813.tar.gz`

### 7.4 可选 updater 包
- updater 可单独通过 `usb_updater_build.sh` 打包
- 当前主包与 updater 包边界仍保留

### 7.5 当前主包默认收包口径
- 当前默认交付口径已经固定为纯新布局
- 当前主包默认进入包的核心内容包括：
  - `bin/CameraRecorder/CameraRecorder`
  - `bin/CameraRecorder/config/camera_recorder.yaml`
  - `bin/SensorRecorder/SensorRecorder`
  - `bin/SensorRecorder/zeroing`
  - `bin/SensorRecorder/99-serial.rules`
  - `bin/GripperHmiTool/GripperHmiTool`
  - `bin/UgripperRuntime/UgripperRuntime`
  - `bin/UgripperRuntime/audio/*`
  - `bin/UgripperRuntime/audio_en/*`
  - `bin/UgripperRuntime/config/fakeCamCalib.json`
  - `run_record.sh`
  - `scripts/`
  - `auto_update/`
  - `auto_calibration/`
- 当前默认不会再进入主包的 legacy 兼容内容包括：
  - `/opt/ugripper/build/src/...`
  - `/opt/ugripper/audio/...`
  - `/opt/ugripper/audio_en/...`
  - `/opt/ugripper/config/fakeCamCalib.json`

### 7.6 当前安装的 systemd / udev 事实
- 当前主包安装的 systemd unit：
  - `ugripper.service`
  - `ugripper-calibration.service`
  - `ugripper-network-monitor.service`
  - `umi-shutdown-trigger.service`
  - `umi-shutdown-trigger.path`
- 当前主包安装的 udev 规则：
  - `/etc/udev/rules.d/99-fixed-usb-map.rules`
  - `/etc/udev/rules.d/99-serial.rules`
- 当前 `postinst` 会 enable + restart：
  - `ugripper.service`
  - `ugripper-network-monitor.service`
  - `umi-shutdown-trigger.service`
  - `umi-shutdown-trigger.path`
- 当前 `ugripper-calibration.service` 会安装，但默认不在 `postinst` 里自动 enable
- 当前仓库里还存在但不属于主包默认安装主线的 unit：
  - `usb-auto-update@.service`
  - `ugripper-boot-install.service`
  - 它们当前目标归属是可选独立包 `ugripper-usb-updater`

## 8. 当前测试状态

### 8.1 测试分层
- `host-only / gtest / ctest`
- `离线 analyzer`
- `板端标准脚本`
- `板端人工主动刺激`
- `长录 / 压力 / release gate`

### 8.2 当前测试真源
- `test/src`
- `test/scripts`

### 8.3 当前已稳定在用的入口
- `board_sensor_smoke.sh`
- `board_camera_stress.sh`
- `board_service_integration_check.sh`
- `board_service_episode_loop_check.sh`
- `board_gripper_hmi_active_check.sh`
- `check_video_windows.py`
- `check_sensor_mcap`
- `check_gripper_ack_log.py`
- `check_hmi_event_log.py`

### 8.4 当前代表性结论
- 以 `docs/ppmain-ugripper-test-summary.md` 为当前测试结果摘要入口
- `merge16` 这轮已有多组真机正样本
- `camera / service / gripper_hmi` 三条主路径都已有板端实测证据
- `20min`、`1h` soak 和 `90%+ CPU` 压力样本已覆盖
- `motion alert` 曾出现过低概率偶发样本，但后续 `5/5` 静止短录未复现

## 9. 当前已知边界

### 9.1 已经收口的
- 四个核心对象已回迁到本仓
- 旧 `src/*` 应用目录已退出主链
- 测试入口和 analyzer 已回迁到本仓 `test/`
- 文档已区分“当前入口”和“历史归档”

### 9.2 仍需保守理解的
- `service 1h` 的主摄 full sweep 这轮是在宿主机上做的，不是板端原地全扫
- `motion alert` 的启动期偶发样本目前只能归类为低概率现象，不能直接宣称根因关闭
- deploy / calibration / updater 仍是本仓内保留边界，尚未进一步做最终形态拆分
- `ASR/`、`time_sync/` 当前仍属于冻结候选，不是主链真源

### 9.3 当前不建议做的事
- 不要再把旧 `src/record_runtime`、`src/camera_recorder`、`src/sensor_recorder`、`src/gripper_hmi` 当成开发落点
- 不要把 `tmp/`、`build/`、`.local-deps/` 当成真源目录
- 不要把历史归档文档重新当成当前实现说明入口

## 10. 当前维护入口
- 给 agent 的快速读取入口：`docs/agent/current-status.md`
- 当前系统完整说明：`docs/agent/overview.md`
- 当前架构与实现落地说明：`docs/ugripper-refactor-architecture.md`
- 当前测试结果摘要：`docs/ppmain-ugripper-test-summary.md`
- 历史详细变更追溯：`docs/REFACTOR_LOG.md`
- 历史规划 / reference 归档：`docs/archive/2026-refactor-history/`
