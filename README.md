# Ugripper V2

当前仓库对应 **Ugripper V2**，默认部署形态为单机双手、本地控制的数据采集系统。

## 文档入口
- 唯一总览文档：`docs/agent/overview.md`
- 历史变更记录：`docs/CHANGELOG.md`
- 仓库内测试脚本说明：`test/README.md`

推荐先读 `docs/agent/overview.md`；`docs/CHANGELOG.md` 只用于追溯阶段性变更，不作为当前功能口径。

## 系统摘要
- systemd 入口：`pack_script/ugripper.service`
- 主运行时：`build/src/record_runtime/record_runtime`
- 默认录制集合：`left_cam_main`、`right_cam_main`、4 路 tactile、左右传感器 MCAP
- 数据目录：`/mnt/data_disk/<device_sn_lower>/data/episode_YYYYMMDD_NNNN`
- 持久化标定：`/etc/ugripper/config/calibration/calibration.json`
- U 盘入口：`auto_update/usb_auto_update.sh`

## 常用命令
```bash
sudo systemctl status ugripper.service
sudo systemctl restart ugripper.service
sudo journalctl -u ugripper.service -f
```

## 仓库内测试脚本
`test/scripts/` 目前收纳现场验证脚本，不属于默认安装主链路，也不会随当前 `deb` 默认安装到 `/opt/ugripper`。

常见入口：

```bash
bash test/scripts/camera_test.sh
bash test/scripts/camera_crash_capture.sh
bash test/scripts/testVideoPipe.sh
```

更具体的脚本说明、环境变量和注意事项见 `test/README.md`。

## Python 环境初始化
首次部署时，手动用 `uv` 按依赖表重建项目内可打包的 `.venv`。
目标结构是把 CPython 运行时收进 `.venv/.python-runtime/`，避免 `.venv/bin/python3` 链到机器外部路径。

```bash
cd /path/to/ugripper_v2

PY_VER="$(sed -n 's/^requires-python = "==\([^"]*\)"$/\1/p' pyproject.toml | head -n 1)"
PY_MM="$(printf '%s' "$PY_VER" | cut -d. -f1,2)"
TMP_PY_DIR="$(mktemp -d)"

rm -rf .venv
uv python install --install-dir "$TMP_PY_DIR" "$PY_VER"

PY_BIN="$(find "$TMP_PY_DIR" -path "*/bin/python${PY_MM}" -type f | head -n 1)"
UV_LINK_MODE=copy uv venv --relocatable --python "$PY_BIN" .venv
VIRTUAL_ENV="$PWD/.venv" UV_LINK_MODE=copy uv sync --active --frozen --no-editable --no-install-project --python "$PY_BIN"

mkdir -p .venv/.python-runtime
mv "$(dirname "$(dirname "$PY_BIN")")" .venv/.python-runtime/

REL_PY="$(realpath --relative-to="$PWD/.venv/bin" "$PWD/.venv/.python-runtime/$(basename "$(dirname "$(dirname "$PY_BIN")")")/bin/python${PY_MM}")"
ln -snf "$REL_PY" .venv/bin/python
ln -snf python .venv/bin/python3
ln -snf python .venv/bin/python${PY_MM}

rm -rf "$TMP_PY_DIR"
./.venv/bin/python3 -c "import sys, pygame; print(sys.executable); print(sys.base_prefix)"
```

验证通过后，再执行 `./build_deb.sh` 或 `./build_deb.sh -q` 打包；包内会直接携带这套 `.venv`。

当前 `build_deb.sh` 的打包 staging 默认按 `rsync` 增量复用：
- 项目主体与 `.venv` 分两次同步，避免每次都先删掉再重拷 `.venv`。
- 默认 `dpkg-deb` 压缩口径改为 `xz -1`，在当前包体和构建速度之间取更平衡的默认值。
- `-q/--quick` 默认继续跳过 C++ 编译，并沿用 `xz -1` 的较快压缩口径，优先缩短出包时间。
- 如需手动覆盖压缩参数，可在打包前设置 `DPKG_DEB_COMPRESSOR`、`DPKG_DEB_LEVEL`、`DPKG_DEB_STRATEGY`、`DPKG_DEB_UNIFORM_COMPRESSION`。
- 如需直接复用仓库外或主目录已有的 `.venv` / `build` 产物，可设置 `PACKAGED_VENV_SOURCE`、`PACKAGED_BUILD_DIR`。

示例：

```bash
./build_deb.sh
./build_deb.sh -q
DPKG_DEB_COMPRESSOR=xz DPKG_DEB_LEVEL=3 ./build_deb.sh
PACKAGED_VENV_SOURCE=/home/ubuntu/proj/ugripper_v2/.venv \
PACKAGED_BUILD_DIR=/home/ubuntu/proj/ugripper_v2/build \
./build_deb.sh -q
```
