# Camera Config Samples

本目录存放 `camera_recorder` 配置契约样例。

当前样例分成 3 类：

- `legacy_flat_valid.yaml`
  - 当前 parser 已支持的旧格式可执行样例
- `schema_v1_valid.yaml`
  - `Step 4` 冻结的 `schema_version: 1` 合同样例
  - 当前 parser 尚未支持；实际兼容验证后移到 `Step 6`
- `legacy_flat_invalid_missing_output_files.yaml`
  - 非法旧格式样例
  - 用于验证缺失关键字段时的稳定报错

当前目录先作为测试资产真源落地，不在 `Step 4` 引入新的 `CTest` 目标。
