# 统计实现

本目录把内部统计整理为模型、参数、结果三部分，不参与数据传输或命令调度。

- `stats.cpp`：收集类型化内部计数，提供显式 diagnostic 输出所需的字段。
- `result.cpp`：定义精简公共结果集合、物理单位换算、参数变化比较、人读报告及 JSON schema 2。
- JSON 不再反解析终端文本；整数和浮点值按类型保存。
- summary 使用英文精简报告（MODEL / PARAMETERS / RESULTS）；指标和提示使用英文，用户自定义名称原样保留。只有 diagnostic 附加内部字段，不能要求普通结果含全部内部计数。
- `row_hit_pct` 的分母是首次调度分类总数；平均队列诊断统一使用 aggregate_ctrl_cycles 归一化。
- 内部计数被 Controller/MemorySystem/验证测试使用，不等于每一项都应公开。
- 新增公共指标须说明独立用途、单位、范围、分母和无数据时的状态，并更新结果契约测试。

接口声明见 [统计接口](../../include/hbm_sim/stats/README.md)。
完整使用说明和迁移表见 [输出指南](../../文档/输出指南.md)；
全部专项诊断键的解释见 [交付手册附录 B](../../堆叠存储模型交付手册.md#appendix-b)。
