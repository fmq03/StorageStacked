# 配置目录

普通模型从两份自包含主模板复制并修改，不需要额外的开发者配置。
HBM 主模板使用 `sids=auto`：修改支持的层数会先联动 SID，再计算容量和密度；
需要固定逻辑组织时填写显式正整数。完整规则见 cfg 指南，不通过层数猜测厂商 Timing。

| 文件 | 用途 |
|---|---|
| [hbm.cfg](hbm.cfg) | HBM3/HBM4 家族主模板 |
| [lpddr.cfg](lpddr.cfg) | LPDDR5/LPDDR6 家族主模板 |
| [profile_index.csv](profile_index.csv) | 参数集位置和来源索引，不是运行配置 |
| [validation/](validation/README.md) | 共同命令面对齐配置与审计元数据，不等于完整器件模板 |
| [examples/configs](../examples/configs/) | 单实例/多实例的自包含 Demo |
| [experiments/local](../experiments/local/) | 自定义研究用例 |

详细字段、分节用法、主输入与派生关系统一见[cfg指南](../文档/cfg指南.md)；
启动命令见[项目指南](../文档/项目指南.md#usage)，机器结果见[输出指南](../文档/输出指南.md#json)。

主模板省略的完整协议/Timing 基准由 src/dram/profiles.cpp 展开。
每次实验保存 --dump-resolved-config，核对实际生效值；来源与外部参考的声明要求见
[审计](../文档/审计.md)。这里不另维护一套字段表或默认数值表。

`profile_index.csv` 的 `speed_bin_mbps`、`density_gb`、`stack_height` 记录该行
`config + standard + preset` 解析后的实际模型值，回归测试会与 resolved 配置逐项比较。
`reference_model` 单独记录外部参考型号；其名称中的密度、层数和速率不替代执行参数。
单通道共同面不代表完整参考器件，具体比较边界见该行 `claim_boundary`。
