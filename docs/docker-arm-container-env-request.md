# `ros2-humble-arm:latest` 容器环境补充申请

## 背景
- 目标镜像：`harbor.dmrobot.com/library/ros2-humble-arm:latest`
- 验证时间：`2026-04-27`
- 目标：在统一容器内完成 `ugripper` 的 `arm64` 交叉编译与 `deb` 出包

当前验证结论是：该镜像可用于 `ugripper` ARM 出包，但需要先补齐 `arm64` foreign architecture、APT 源配置和若干系统依赖。建议将这些补充固化进统一镜像基线，避免业务侧在运行时重复修改共享环境。

## 需要固化的环境修改

### 1. 增加 `arm64` 架构
```bash
dpkg --add-architecture arm64
```

### 2. 配置 `amd64` APT 源
将 `/etc/apt/sources.list` 调整为：

```bash
cat >/etc/apt/sources.list <<'EOF'
# Ubuntu jammy amd64
deb [arch=amd64] http://archive.ubuntu.com/ubuntu/ jammy main restricted
deb [arch=amd64] http://archive.ubuntu.com/ubuntu/ jammy-updates main restricted
deb [arch=amd64] http://archive.ubuntu.com/ubuntu/ jammy universe
deb [arch=amd64] http://archive.ubuntu.com/ubuntu/ jammy-updates universe
deb [arch=amd64] http://archive.ubuntu.com/ubuntu/ jammy multiverse
deb [arch=amd64] http://archive.ubuntu.com/ubuntu/ jammy-updates multiverse
deb [arch=amd64] http://archive.ubuntu.com/ubuntu/ jammy-backports main restricted universe multiverse
deb [arch=amd64] http://security.ubuntu.com/ubuntu/ jammy-security main restricted
deb [arch=amd64] http://security.ubuntu.com/ubuntu/ jammy-security universe
deb [arch=amd64] http://security.ubuntu.com/ubuntu/ jammy-security multiverse
EOF
```

### 3. 新增 `arm64` ports 源
```bash
mkdir -p /etc/apt/sources.list.d

cat >/etc/apt/sources.list.d/arm64-ports.list <<'EOF'
deb [arch=arm64] http://ports.ubuntu.com/ubuntu-ports jammy main restricted universe multiverse
deb [arch=arm64] http://ports.ubuntu.com/ubuntu-ports jammy-updates main restricted universe multiverse
deb [arch=arm64] http://ports.ubuntu.com/ubuntu-ports jammy-backports main restricted universe multiverse
deb [arch=arm64] http://ports.ubuntu.com/ubuntu-ports jammy-security main restricted universe multiverse
EOF
```

### 4. 刷新 APT 索引
```bash
apt-get update
```

### 5. 安装交叉编译与 `arm64` 目标依赖
```bash
apt-get install -y \
  bash \
  build-essential \
  ca-certificates \
  cmake \
  curl \
  dpkg-dev \
  file \
  findutils \
  git \
  grep \
  make \
  pkg-config \
  rsync \
  sed \
  tar \
  binutils-aarch64-linux-gnu \
  gcc-aarch64-linux-gnu \
  g++-aarch64-linux-gnu \
  libyaml-cpp-dev:arm64 \
  nlohmann-json3-dev \
  liblz4-dev:arm64 \
  libzstd-dev:arm64 \
  libserialport-dev:arm64 \
  libusb-1.0-0-dev:arm64 \
  libgstreamer1.0-dev:arm64 \
  libgstreamer-plugins-base1.0-dev:arm64
```

## 建议补充的环境变量
```bash
export PKG_CONFIG_PATH=/usr/lib/aarch64-linux-gnu/pkgconfig:${PKG_CONFIG_PATH:-}
```

## 验证结果
- 在上述环境补充完成后，已成功完成 `ugripper` ARM 出包验证
- 产物：`ugripper_1.2.8_arm64.deb`

