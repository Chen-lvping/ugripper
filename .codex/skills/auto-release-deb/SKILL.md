---
name: auto-release-deb
description: 在代码修改完成后，自动确认改动影响范围，并判断 ugripper 与 updater 的构建方式；默认执行建议编译命令，并在构建成功后自动安装对应新包；不负责版本号修改。
---

# auto-release-deb

## 适用场景
- 用户要求“修改完代码后判断怎么打包/编译更合适”。
- 用户要求“确认本次改动影响了哪些发布包（ugripper/updater）”。

## 默认规则
1. 本 skill 不修改任何版本号（`build_deb.sh` / `usb_updater_build.sh` 都不改）。
2. 改动识别不依赖 git 提交历史，基于自动生成的源码 manifest 对比：
   - `ugripper` 基线：`temp_build_deb/.auto_release_ugripper.manifest`
   - `updater` 基线：`temp_build_usb_updater/.auto_release_updater.manifest`
   - 基线由自动判定脚本 `write_source_manifest.sh` 生成与更新。
   - 若某基线缺失，对应包按安全策略强制建议构建。
3. 自动识别改动范围：
   - 是否影响 `ugripper` 主包。
   - 是否影响 `ugripper-usb-updater` 包。
4. 自动判断 `ugripper` 构建模式：
   - 命中 C/C++/CMake 改动时，判定为全量 ARM 构建（`./scripts/build_arm_deb_in_pp_arm_dev.sh`）。
   - 未命中上述改动且 quick 所需 ARM 二进制齐全时，判定可快速构建（`./scripts/build_arm_deb_in_pp_arm_dev.sh -q`）。
   - quick 所需二进制缺失时，回退全量构建。
5. `updater` 无 quick 模式：
   - 只要命中 updater 相关改动，判定需要执行 `./usb_updater_build.sh`（全量）。
6. `docs/CHANGELOG.md` 不由本 skill 维护；功能改动日志由 `add-feature` 流程维护。
7. 默认在判定后执行建议构建命令；仅当用户明确要求“只判定不编译/跳过编译”时，才只输出建议不执行。
8. 默认在构建成功后自动安装本次生成的对应 `deb` 包；仅当用户明确要求“跳过安装/只编译不安装”时，才跳过安装。
9. `ugripper` 安装默认执行 `sudo dpkg -i ./ugripper_<VERSION>_arm64.deb`；`updater` 安装默认执行 `sudo dpkg -i ./ugripper-usb-updater_<VERSION>_arm64.deb`。若脚本实际产物命名与此不同，应以实际生成文件为准。
10. 若本 skill 已执行构建或安装，上层调用流程（如 `add-feature`）不得重复执行同一批命令。

## 执行步骤
1. 在仓库根目录执行：
   - `bash .codex/skills/auto-release-deb/scripts/auto_release_deb.sh`
   - 可选：`--ugripper-manifest <PATH>` / `--updater-manifest <PATH>` 覆盖默认 manifest 路径。
2. 根据脚本输出执行建议构建命令（若存在）：
   - `./scripts/build_arm_deb_in_pp_arm_dev.sh` 或 `./scripts/build_arm_deb_in_pp_arm_dev.sh -q`
   - `./usb_updater_build.sh`
3. 构建成功后，默认安装本次生成的 `deb` 包：
   - `sudo dpkg -i ./ugripper_<VERSION>_arm64.deb`
   - `sudo dpkg -i ./ugripper-usb-updater_<VERSION>_arm64.deb`
   - 若只命中其中一个 scope，则只安装对应包。
4. 向用户汇报：
   - 改动范围（ugripper / updater / both / none）。
   - `ugripper` 是否必须全量构建，以及原因。
   - `updater` 是否需要构建，以及原因。
   - 建议构建命令与实际执行结果（成功/失败）。
   - 实际安装命令与结果（成功/失败）。

## 输出约束
- 汇报中必须明确“未改版本号”。
- 若用户明确仅询问判定结果或要求跳过编译，不执行构建命令并显式说明“按用户要求仅判定”。
- 若用户明确要求跳过安装，必须显式说明“按用户要求未自动安装”。
