# Ugripper V2

当前仓库对应 **Ugripper V2**，默认部署形态为单机双手、本地控制的数据采集系统。

## 文档入口
- 当前维护状态：`docs/agent/current-status.md`
- 唯一总览文档：`docs/agent/overview.md`
- 当前架构说明：`docs/ugripper-refactor-architecture.md`
- 当前测试摘要：`docs/ppmain-ugripper-test-summary.md`
- 历史变更记录：`docs/CHANGELOG.md`
- 仓库内测试脚本说明：`test/README.md`

推荐先读 `docs/agent/current-status.md`，再读 `docs/agent/overview.md`；`docs/CHANGELOG.md` 只用于追溯阶段性变更，不作为当前功能口径。

## 系统摘要
- systemd 入口：`pack_script/ugripper.service`
- 主入口：`run_record.sh`
- 主运行时：优先 `bin/UgripperRuntime/UgripperRuntime`，兼容 fallback `build/src/record_runtime/record_runtime`
- 默认录制集合：`left_cam_main`、`right_cam_main`、4 路 tactile、左右传感器 MCAP
- 数据目录：`/mnt/data_disk/<device_sn_lower>/data/episode_YYYYMMDD_NNNN`
- 持久化标定：`/etc/ugripper/config/calibration/calibration.json`
- U 盘入口：`auto_update/usb_auto_update.sh`

## 时钟同步
`ugripper-ntp-sync.service` 每次开机和安装后触发一次，最多等待网络 `45s`。脚本先检查 `/tmp/umi_recording.lock`：录制中直接跳过；空闲时优先用 `sntp -S` 一次性校时，再 fallback 到 `ntpd -q -g` / `timedatectl`。同步结束、失败或检测到录制开始后都会停止常驻 NTP 服务，避免录制时间轴跳变。

## 常用命令
```bash
sudo systemctl status ugripper.service
sudo systemctl restart ugripper.service
sudo journalctl -u ugripper.service -f
```

## 仓库内测试脚本
`test/scripts/` 目前收纳现场验证脚本，不属于默认安装主链路，也不会随当前 `deb` 默认安装到 `/opt/ugripper`。

`test/src/gripper_disconnect_repro_sop/` 是可单独打包给测试同学的夹爪掉线复现包。进入该文件夹后可在主机侧一键启动自动软件录制压力测试，实时刷新板端 `hws` 状态，并在报错时抓取现场快照：

```bash
cd test/src/gripper_disconnect_repro_sop
./scripts/prepare_240_repro.sh
```

常见入口：

```bash
bash test/scripts/camera_test.sh
bash test/scripts/camera_crash_capture.sh
bash test/scripts/testVideoPipe.sh
```

更具体的脚本说明、环境变量和注意事项见 `test/README.md`。

## 首次交叉编译环境准备
首次在开发机上为 `ugripper` 出 `arm64 deb` 时，建议先拉起统一的 ROS2 Humble ARM 交叉编译容器。后续推荐进入容器后直接执行出包脚本，因此容器工作目录建议保持为本仓 `standalone/UGripper`。

### 拉取 Docker 镜像
```bash
docker pull harbor.dmrobot.com/library/ros2-humble-arm
```

### 启动交叉编译容器
以下命令按当前仓库所在路径 `/home/dm/proj/pp-main` 准备，宿主机挂载 `/home/dm/proj` 后，容器内可以继续使用同一路径访问本仓库：

```bash
docker run -dit \
  --name pp-arm-dev \
  --privileged \
  --gpus all \
  --runtime=nvidia \
  --env NVIDIA_VISIBLE_DEVICES=all \
  --env NVIDIA_DRIVER_CAPABILITIES=compute,utility,graphics \
  --env="DISPLAY" \
  --env="QT_X11_NO_MITSHM=1" \
  --network host \
  --device=/dev/bus/usb \
  -v /home/dm/proj:/home/dm/proj \
  -v /tmp/.X11-unix:/tmp/.X11-unix:rw \
  -v /usr/local/cuda:/usr/local/cuda:ro \
  -v /tmp:/tmp \
  -v /dev:/dev \
  -v /run/udev:/run/udev:ro \
  -v /sys/bus/usb:/sys/bus/usb:ro \
  --device=/dev/dri/renderD128 \
  --device=/dev/snd \
  -v /run/user/1000/pulse/native:/run/user/1000/pulse/native \
  -v $HOME/.config/pulse:/root/.config/pulse \
  -e PULSE_SERVER=unix:/run/user/1000/pulse/native \
  -w /home/dm/proj/pp-main/standalone/UGripper \
  harbor.dmrobot.com/library/ros2-humble-arm
```

如果宿主机仓库不在 `/home/dm/proj/pp-main`，需要同步调整 `-v` 的宿主机路径和 `-w` 工作目录。

### 初始化 ARM 交叉编译依赖
首次创建容器后，进入容器并执行一次环境初始化：

```bash
docker exec -it pp-arm-dev bash
cd /home/dm/proj/pp-main/standalone/UGripper
bash ./scripts/setup_arm_build_env.sh
```

该脚本会在仓库内生成隔离的 ARM64 sysroot 和构建环境文件：
- `.local-deps/sysroot-arm64`
- `.local-deps/downloads/apt`
- `.local-deps/arm-build-env.sh`

当前 ARM sysroot 需要包含以下主链 C++ 目标开发依赖：
- `libyaml-cpp-dev:arm64`
- `nlohmann-json3-dev`
- `liblz4-dev:arm64`
- `libserialport-dev:arm64`
- `libusb-1.0-0-dev:arm64`
- `libfmt-dev:arm64`
- `libspdlog-dev:arm64`
- `libgstreamer1.0-dev:arm64`
- `libgstreamer-plugins-base1.0-dev:arm64`

后续如果 `.local-deps/arm-build-env.sh` 已存在，推荐的一键出包脚本会直接复用；如果文件缺失，脚本也会在容器内自动补齐。

## 推荐出包路径
当前推荐进入容器后直接执行 ARM 一键出包脚本：

```bash
docker exec -it pp-arm-dev bash
cd /home/dm/proj/pp-main/standalone/UGripper
./scripts/build_arm_deb_in_pp_arm_dev.sh
```

默认会在当前容器内自动完成：
- 设置 `PKG_CONFIG_PATH=/usr/lib/aarch64-linux-gnu/pkgconfig`
- 编译主包必需目标
- 调用 `build_deb.sh -q` 产出 `arm64 deb`
- 如果脚本以 root 运行，把 `build/`、`temp_build_deb*` 和生成的 `.deb` 所有权回收给仓库目录 owner

脚本默认要求在容器内运行；如确实需要在非容器环境中复用，可显式设置 `ALLOW_HOST_RUN=1`。

如果需要手动补齐容器环境，可单独执行：

```bash
bash ./scripts/setup_arm_build_env.sh
```

如果 ARM 构建目录已经存在，只想重新组包，直接执行：

```bash
PACKAGED_BUILD_DIR=build/arm_container_release ./build_deb.sh -q
```

## 已验证前提
- `build_deb.sh` 默认优先从 Nexus raw 下载归档好的 `.venv`，再完成打包。
- 当前默认 `.venv` 归档 URL 已内置在 `build_deb.sh`，用于 `ugripper-v2-uv-venv/py311-v1`。
- 标准模式只编主包必需目标：`CameraRecorder`、`SensorRecorder`、`zeroing`、`GripperHmiTool`、`UgripperRuntime`。
- 打包前会校验 `.venv/bin/python3` 和以上 5 个核心二进制的 ELF 架构，避免把 `x86_64` 产物误打进 `arm64` 包。
- 顶层默认不再强制构建 `src/third_party/mcap_builder`，普通主包出包不会再被这个可选目标阻塞。
- 顶层当前直接依赖系统 `yaml-cpp` 和 `nlohmann_json`；仓库内不再保留 `.local-deps/nlohmann` fallback。
- C++ 日志后端当前对齐 `pp_main` spdlog 实现，主包运行依赖已包含 `libfmt8` 和 `libspdlog1`；若板端离线安装，需要确保这两个运行库能通过系统源或本地依赖包安装。

## 当前版本口径
- 当前主包文件名模式：`ugripper_<version>_arm64.deb`
- 当前脚本默认值仍是：
  - `BASE_VERSION=1.2.8`
  - `VERSION_SUFFIX=''`
  - 因此默认产物名会是 `ugripper_1.2.8_arm64.deb`
- 这里的 `1.2.8` 只是当前脚本里的默认基线值，方便本地构建验证；它不是从代码、tag、changelog 或发布系统自动推导出来的正式版本源。
- 如果本次出包需要可识别的版本，请显式传：
  - `VERSION_SUFFIX=+<tag>`
  - 或 `VERSION=<full-version>`
- 实际发布时，不要默认沿用脚本里的 `1.2.8`，应按当次发布口径显式覆盖。

## 常用覆盖项
只在默认路径不适用时再覆盖：

```bash
PARALLEL=12 ./scripts/build_arm_deb_in_pp_arm_dev.sh
VERSION_SUFFIX=+merge1 ./scripts/build_arm_deb_in_pp_arm_dev.sh
HOST_UID=$(id -u) HOST_GID=$(id -g) ./scripts/build_arm_deb_in_pp_arm_dev.sh
ALLOW_HOST_RUN=1 ./scripts/build_arm_deb_in_pp_arm_dev.sh
PACKAGED_BUILD_DIR=/home/ubuntu/proj/ugripper_v2/build ./build_deb.sh -q
PACKAGED_VENV_URL='' PACKAGED_VENV_SOURCE=/home/ubuntu/proj/ugripper_v2/.venv ./build_deb.sh -q
VERSION_SUFFIX=+merge1 ./build_deb.sh -q
DPKG_DEB_COMPRESSOR=xz DPKG_DEB_LEVEL=3 ./build_deb.sh
```

变量说明：
- `PARALLEL`：容器内编译并行度
- `BASE_VERSION` / `VERSION_SUFFIX` / `VERSION`：透传给 `build_deb.sh`，决定最终 `.deb` 文件名；当前默认 `BASE_VERSION=1.2.8` 只是脚本占位值
- `HOST_UID` / `HOST_GID`：脚本以 root 运行时，出包后回收文件所有权使用的用户 / 组；默认取仓库目录 owner
- `ALLOW_HOST_RUN`：默认不允许在非容器环境中运行；设为 `1` 后跳过容器环境检查
- `PACKAGED_BUILD_DIR`：已有 ARM 构建产物目录
- `PACKAGED_VENV_URL`：归档 `.venv` 下载地址；设为空则不下载
- `PACKAGED_VENV_SOURCE`：本地 `.venv` 来源目录
- `VERSION_SUFFIX`：临时附加包版本后缀

## Python 环境初始化
只有在需要更新或重建可打包 `.venv` 时，才需要执行这一步。

这套 `.venv` 必须和目标包架构一致。给 `arm64` 板出包时，`.venv` 也必须来自 `aarch64/arm64` 环境，不能直接复用开发机或 `x86_64` 容器里的 Python 运行时。

推荐命令：

```bash
./scripts/build_runtime_venv.sh
VENV_VERSION_TAG=1.2.8+merge1-arm64 ./scripts/build_runtime_venv.sh
```

常见覆盖：

```bash
./scripts/build_runtime_venv.sh --target-dir /tmp/ugripper-arm64-venv
./scripts/build_runtime_venv.sh --python-version 3.11.15 --version-tag 1.2.8+merge1
```

脚本会自动：
- 从 `pyproject.toml` 读取固定 Python 版本
- 用 `uv.lock` 重建 relocatable `.venv`
- 验证 `pygame` 可导入
- 写入 `.venv/.ugripper-venv-manifest.json`

维护约定：
- 仓库保留 `pyproject.toml`、`uv.lock` 和当前工作 `.venv`
- 服务器按版本号归档 `.venv` 成品和 `.deb`
- 恢复时优先回取服务器归档；只剩仓库时再按锁文件重建

## 可选 updater 包打包
`das-usb-updater` 现在是独立可选包，不属于主 `ugripper` 包安装主线。

默认打包命令：

```bash
./usb_updater_build.sh
```

如需为现场验证包加可识别后缀：

```bash
PKG_VERSION_SUFFIX=+merge1 ./usb_updater_build.sh
```

默认产物位置：

```bash
build/package/updater/das-usb-updater_1.0.0_all.deb
```

已部署旧 `ugripper-usb-updater` 的板端可使用一次性过渡包完成同次插盘迁移。U 盘同时放入过渡包和 `das-usb-updater` 包后，旧 updater 会先安装过渡包，过渡包 postinst 会重新拉起本次 USB 流程，再安装 `das-usb-updater`：

```bash
./usb_updater_transition_build.sh
```

过渡包默认产物位置：

```bash
build/package/updater-transition/ugripper-usb-updater_1.2.5_all.deb
```

和主包一样，可通过 `DPKG_DEB_COMPRESSOR`、`DPKG_DEB_LEVEL`、`DPKG_DEB_STRATEGY`、`DPKG_DEB_UNIFORM_COMPRESSION` 覆盖 `dpkg-deb` 压缩参数。

补充：
- `pp_main/package_ugripper_stage.sh` 当前也会尝试把 `UGRIPPER_ROOT/.venv` 收进主包
- 如需覆盖来源，可显式设置 `PACKAGED_VENV_SOURCE`
