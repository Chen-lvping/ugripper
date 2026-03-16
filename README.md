# Ugripper V2

当前仓库对应 **Ugripper V2**，默认部署形态为单机双手、本地控制的数据采集系统。

## 文档入口
- 唯一总览文档：`docs/agent/overview.md`
- 历史变更记录：`docs/CHANGELOG.md`

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
