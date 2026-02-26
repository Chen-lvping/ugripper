---
name: auto-release-deb
description: 在代码修改完成后，自动执行发布收尾：升级 build_deb.sh 版本号（默认 +.z，用户指定才 +.x/+ .y），并自动判断是否使用 build_deb.sh -q。
---

# auto-release-deb

## 适用场景
- 用户要求“修改完代码后自动打包 deb”。
- 用户要求“自动更新 build_deb 版本号”。

## 默认规则
1. 版本号默认只升级 `.z`（patch）。
2. 仅当用户明确指定时，才升级 `.x` 或 `.y`。
3. 自动判定是否使用 `-q`：
   - 若改动包含 C/C++/CMake 文件，则使用标准模式 `./build_deb.sh`。
   - 若不包含上述改动且关键二进制存在，则使用快速模式 `./build_deb.sh -q`。
   - 若关键二进制缺失，则回退标准模式。
4. `docs/CHANGELOG.md` 不由本 skill 维护；功能改动日志由 `add-feature` 流程维护。

## 执行步骤
1. 在仓库根目录执行：
   - 默认（`.z`）：
     - `bash .codex/skills/auto-release-deb/scripts/auto_release_deb.sh`
   - 指定升级 `.y`：
     - `bash .codex/skills/auto-release-deb/scripts/auto_release_deb.sh --bump .y`
   - 指定升级 `.x`：
     - `bash .codex/skills/auto-release-deb/scripts/auto_release_deb.sh --bump .x`
2. 向用户汇报：
   - 旧版本 -> 新版本
   - build_deb 实际执行模式（`-q` 或标准模式）及原因

## 输出约束
- 汇报中必须明确版本规则（默认 +.z）。
- 若用户未指定 `.x/.y`，不得升级 `.x/.y`。
