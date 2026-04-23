# Repo Migration Map

本文档用于给出“当前目录或关键路径 -> 目标动作”的映射表。
它服务于渐进式重构，不要求一次性完成所有迁移。

动作定义：

- `keep`：当前阶段保留位置不动
- `move`：迁移到新目录
- `split`：按职责拆分后分别归位
- `freeze`：冻结，不再新增主链引用，暂不物理迁移
- `archive`：完成断引用与验证后再迁入归档区

## 1. 一级目录与关键路径映射

| 当前路径 | 当前角色 | 目标动作 | 目标位置或规则 | 阶段 | 默认进包 | 风险 |
| --- | --- | --- | --- | --- | --- | --- |
| `src/record_runtime` | 产品主链应用 | `keep` | Phase 1 保持在 `src/`；后续视情况再评估 `apps/record_runtime` | P2 之后再议 | 是 | 中 |
| `src/camera_recorder` | 产品主链应用 | `keep` | Phase 1 保持在 `src/`；后续视情况再评估 `apps/camera_recorder` | P2 之后再议 | 是 | 中 |
| `src/sensor_recorder` | 产品主链应用 | `keep` | Phase 1 保持在 `src/`；后续视情况再评估 `apps/sensor_recorder` | P2 之后再议 | 是 | 中 |
| `src/gripper_hmi` | 产品主链库 | `keep` | Phase 1 保持在 `src/`；后续视情况再评估 `libs/gripper_hmi` | P2 之后再议 | 是 | 中 |
| `src/utils` | 主链公共库 | `keep` | Phase 1 保持在 `src/`；继续作为最小公共层真源 | 当前阶段 | 是 | 低 |
| `src/third_party` | 第三方依赖 | `keep` | 暂不迁移；后续按构建系统统一管理 | 后续再议 | 是 | 低 |
| `run_record.sh` | 运行时入口脚本 | `keep` | 先保留根目录；后续再评估 `scripts/runtime/` | P2 之后再议 | 是 | 高 |
| `auto_update/` | 主链更新与 U 盘流程 | `keep` | 先保留目录，内部逐步拆成 `lib/` 与 `steps/`；后续再评估迁入 `scripts/update/` | P4 | 是 | 高 |
| `auto_calibration/` | 主链校准流程 | `keep` | 先保留目录；后续逐步收口到 `scripts/calibration/` 与 `deploy/systemd/` | P4 | 是 | 中 |
| `config/` | 运行时配置与 udev 规则来源 | `move` | 目标为 `configs/runtime/` 与 `deploy/udev/`，但先由 manifest 收口 | P2 | 是 | 中 |
| `audio/` | 混合运行时目录 | `keep` | 先按“代码 / 资源 / 调优资产”分类，不急于物理迁移 | P2 之后再议 | 是 | 中 |
| `audio_en/` | 运行时音频资源 | `keep` | 先保留；后续可与 `audio/` 统一到资源目录 | P2 之后再议 | 是 | 中 |
| `pack_script/` | 部署来源目录 | `split` | `.service -> deploy/systemd/`，deb 模板 -> `deploy/deb/`，辅助 shell -> `scripts/package/` | P2-P3 | 是 | 高 |
| `py_script/` | 现场工具与诊断脚本 | `move` | 迁入 `tools/python/`；是否进包由 manifest 控制 | P2 | 否，例外显式声明 | 中 |
| `test/` | 测试真源目录 | `keep` | Phase 1 保持 `test/src/` + `test/scripts/` 结构；后续如有必要再细分 `field/` 或 `support/` | 当前阶段 | 否 | 低 |
| `docs/` | 文档真源目录 | `keep` | 持续作为计划、baseline、变更口径真源 | 持续维护 | 否 | 低 |
| `build/` | 构建产物目录 | `keep` | 保留为生成目录；不作为实现真源，不纳入路径迁移落点 | 持续排除 | 否 | 低 |
| `tmp/` | 运行/临时产物目录 | `keep` | 保留为临时目录；不作为实现真源，不纳入迁移范围 | 持续排除 | 否 | 低 |
| `.local-deps/` | 本地验证依赖缓存 | `keep` | 仅用于本地 Linux 验证；不进主链、不进打包、不作为实现真源 | 本地验证期 | 否 | 低 |
| `ugripper/` | 历史副本冻结候选 | `freeze` | 停止继续叠加主链逻辑；断引用后再决定删除或归档 | P3 | 否 | 中 |
| `ASR/` | 历史/实验内容 | `freeze` | 先冻结并断引用，不立即迁出；后续再评估 `frozen/` 或 `archive/` | P3 | 否 | 低 |
| `time_sync/` | 历史/停用链路候选 | `freeze` | 先冻结并断引用，不立即迁出；后续再评估 `frozen/` 或 `archive/` | P3 | 否 | 低 |
| `docs/agent/overview.md` | 当前系统说明 | `keep` | 继续作为当前系统主说明 | 持续维护 | 否 | 低 |
| `docs/CHANGELOG.md` | 历史变更追溯 | `keep` | 保留历史追溯角色，不再承担当前行为说明 | 持续维护 | 否 | 低 |
| `build_deb.sh` | 主包打包入口 | `keep` | 先保留入口，内部改为 manifest 驱动；后续再评估迁入 `scripts/package/` | P1-P2 | 否 | 高 |
| `usb_updater_build.sh` | updater 打包入口 | `keep` | 先保留入口；后续按主包打包方式统一 | P2-P3 | 否 | 中 |

## 2. pack_script 拆分映射

`pack_script/` 不应整体搬迁，按文件职责拆分如下。

| 当前路径 | 目标动作 | 目标位置 | 说明 |
| --- | --- | --- | --- |
| `pack_script/ugripper.service` | `move` | `deploy/systemd/ugripper.service` | systemd 主服务定义 |
| `pack_script/control` | `move` | `deploy/deb/control` | deb 控制文件模板 |
| `pack_script/postinst` | `move` | `deploy/deb/postinst` | 安装后脚本模板 |
| `pack_script/prerm` | `move` | `deploy/deb/prerm` | 卸载前脚本模板 |
| `pack_script/postrm` | `move` | `deploy/deb/postrm` | 卸载后脚本模板 |

## 3. audio 目录内部分类

Phase 1 不迁目录，只定义内部职责。

| 当前路径 | 当前职责 | 目标动作 | 说明 |
| --- | --- | --- | --- |
| `audio/audio_play.py` | 运行时代码 | `keep` | 后续可迁入运行时脚本或 Python 模块目录 |
| `audio/record_usb_audio.py` | 运行时代码 | `keep` | 与主链强耦合，先不动 |
| `audio/*.wav` | 运行时资源 | `keep` | 后续可统一归位到音频资源目录 |
| `audio_en/*.wav` | 运行时资源 | `keep` | 与 `audio/*.wav` 联动管理 |
| `audio/noise.prof` | 调优资产 | `keep` | 由 manifest 明确是否随包交付 |

## 4. 冻结区处理规则

以下路径在 Phase 3 前只允许做三类动作：

- 加文档说明
- 搜索引用并断主链引用
- 从默认打包中排除

不建议在确认前直接删除或移动。

| 当前路径 | 当前状态 | 当前动作 | 后续动作 |
| --- | --- | --- | --- |
| `ASR/` | `frozen-candidate` | `freeze` | 断引用完成后再决定迁入 `frozen/` 或 `archive/` |
| `time_sync/` | `frozen-candidate` | `freeze` | 断引用完成后再决定迁入 `frozen/` 或 `archive/` |

## 5. 推荐执行顺序

1. 建立 `manifest`，让打包边界先稳定下来。
2. 标注并冻结 `ASR/`、`time_sync/`。
3. 新建 `tools/`、`deploy/`、`configs/` 的落点。
4. 从 `pack_script/` 开始做按职责拆分。
5. 再推进 `py_script/` 等外围目录迁移；`test/` 是否细分目录留待测试体系稳定后再议。
6. 最后再评估是否值得做 `src -> apps/libs`。
