# 公共头文件分层

当前公共 API 已覆盖真实 payload 仿真：frontend 产生带 payload/expected/mask 的 `Request`，core 提供 `MemoryImage + MemoryBackend`，controller 完成 commit/read-forward/read-check，validation 和 stats 输出可审计结果。新增接口时先判断它属于平台 API、存储模型 API 还是内部实现细节。

`include/hbm_sim/` 和 `src/` 使用同一套职责分层。这样做的目的不是制造目录深度，而是让“接口定义在哪里”和“实现代码在哪里”一一对应，后续继续扩展 JEDEC/vendor timing、HBM4 细协议或 LPDDR6 链路行为时可以快速找到入口。

- `core/`：项目通用基础类型、请求结构、地址映射和多 controller memory system。这里是其他模块共享的最小核心。
- `core/data.hpp`：真实堆叠存储模型接口。命名保持短而具体：`MemoryImage` 表示运行时稀疏镜像，`DataBlock` 表示一个 payload block，`StorageKey` 表示 bank/row/column 坐标，`PhysicalAddress` 表示 floorplan/subarray/mat/cell/microbump 坐标；同层还承载 SECDED shadow、错误注入统计和 TSV-aware 行为级稀疏三维热模型入口。
- `dram/`：DRAM 标准、器件组织、timing table、命令语义、命令合法性和接口开销模型。新增标准字段或校准 JEDEC/vendor 表时优先看这里。
- `controller/`：控制器侧策略和执行路径，包括 scheduler、row policy、refresh/RFM manager、timing engine、命令执行器和统一命令事件。
- `frontend/`：synthetic traffic、trace 和初始化/训练控制序列入口。
- `stats/`：稳定统计结构。输出格式在 `src/stats/stats.cpp` 中维护。
- `validation/`：命令 trace recorder、DFI beat/signal trace generator、离线 command validator 和 DFI validator；项目原生规则在这里自验证，也可导出共同命令面供外部参考对照。

依赖方向尽量保持为：`core` 提供基础，`dram` 描述器件，`controller` 消费 `core/dram`，`frontend` 产生请求，`validation` 重放命令流，`stats` 只保存和输出指标。

命名约定：

- 对外完整入口是 `hbm_sim.hpp`，适合外部实验代码一次性引入整个公共 API。
- 目录已经表达领域时，文件名保持短而清楚，例如 `dram/state.hpp`、`dram/profiles.hpp`、`controller/executor.hpp`、`controller/timing.hpp`、`validation/validator.hpp`。
- 类名和结构名仍保持完整语义，例如 `CommandExecutor`、`TimingEngine`、`CommandTraceValidator`，避免把代码里的核心概念缩写到难以搜索。

## 公共 API 使用方式

外部小实验可以直接使用聚合头：

```cpp
#include "hbm_sim/hbm_sim.hpp"
```

如果只需要某一层能力，建议 include 窄头文件：

```cpp
#include "hbm_sim/dram/spec.hpp"
#include "hbm_sim/core/system.hpp"
```

这种方式更适合长期维护的外部工具，因为依赖关系更明确。

## 放置新接口的判断方法

- 请求、地址、枚举、全局基础类型：放 `core/`。
- 标准、timing、命令语义、器件状态：放 `dram/`。
- 调度、队列、发射、refresh/RFM manager：放 `controller/`。
- synthetic workload、trace、初始化序列：放 `frontend/`。
- 统计结构和稳定输出字段：放 `stats/`。
- 命令 trace、DFI beat/signal trace、离线验证、golden 对比：放 `validation/`。

## 分层约束

- `core` 不依赖上层。
- `dram` 可以依赖 `core`，但不应知道 controller 队列。
- `controller` 消费 `core` 和 `dram`，并更新 `stats`。
- `frontend` 产生 `Request`，不直接发 DRAM 命令。
- `validation` 可以重放命令流，但不参与在线仿真调度。
