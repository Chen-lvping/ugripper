---
name: auto-release-deb
description: 在代码修改完成后，自动确认改动影响范围，并判断 ugripper 与 updater 是否需要全量构建；不再负责版本号修改。
---

# auto-release-deb

## 适用场景
- 用户要求“修改完代码后判断怎么打包/编译更合适”。
- 用户要求“确认本次改动影响了哪些发布包（ugripper/updater）”。

## 默认规则
1. 本 skill 不修改任何版本号（`build_deb.sh` / `usb_updater_build.sh` 都不改）。
2. 自动识别改动范围：
   - 是否影响 `ugripper` 主包。
   - 是否影响 `ugripper-usb-updater` 包。
3. 自动判断 `ugripper` 构建模式：
   - 命中 C/C++/CMake 改动时，判定为全量构建（`./build_deb.sh`）。
   - 未命中上述改动且 quick 所需二进制齐全时，判定可快速构建（`./build_deb.sh -q`）。
   - quick 所需二进制缺失时，回退全量构建。
4. `updater` 无 quick 模式：
   - 只要命中 updater 相关改动，判定需要执行 `./usb_updater_build.sh`（全量）。
5. `docs/CHANGELOG.md` 不由本 skill 维护；功能改动日志由 `add-feature` 流程维护。

## 执行步骤
1. 在仓库根目录执行：
   - `bash .codex/skills/auto-release-deb/scripts/auto_release_deb.sh`
2. 向用户汇报：
   - 改动范围（ugripper / updater / both / none）。
   - `ugripper` 是否必须全量构建，以及原因。
   - `updater` 是否需要构建，以及原因。
   - 建议构建命令（仅建议，不自动执行）。

## 输出约束
- 汇报中必须明确“未改版本号”。
- 若用户仅询问判定结果，不得自行执行构建命令。
