# 统计接口

- `stats.hpp`：仿真模块共享的原始计数、累计量及必要派生辅助函数。
- `result.hpp`：`ResultValue`、`ResultFields`、`ResultReport` 及序列化/格式化接口。

`Stats` 是内部测量状态，`ResultReport` 是经过取舍的公开报告，二者不能等同。
新增内部计数不要求自动加入公共 metrics；先证明有独立分析或验证用途。
单位与范围必须明确，尤其是 Host/事务、system tick/Controller 累计 tick、
控制器行命中/后端行缓冲命中、有效数据/接口记账带宽。

summary 使用英文精简人读报告；机器接口使用分区 JSON schema 2，键名不随终端标签变化。
无完成读时延迟为 null；未启用的功耗/热结果不生成。不要用数值零冒充未收集数据。
整数不能先转 double；JSON 不沿用终端舍入精度。

实现见 [src/stats](../../../src/stats/README.md)，
契约见 [输出指南](../../../文档/输出指南.md)。
