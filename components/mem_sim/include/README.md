# 公共头文件

公共头文件当前暴露的是平台级接口：`DramSpec`、`MemorySystem`、`Controller`、`Request`、`MemoryImage`、`MemoryBackend`、DFI/validation/stats 类型。真实存储区相关声明主要在 `include/hbm_sim/core`，控制器只通过接口使用它，不直接依赖具体 backend。

本目录保存项目对外公开的 C++ 头文件。真正的公共 API 位于
`include/hbm_sim/`，外部代码应通过 `-Iinclude` 引入。

当前公共 API 不只包含 controller/timing，也包含真实存储区模型：`hbm_sim/core/data.hpp`
声明 `MemoryImage`、`DataBlock`、`StorageKey`、`PhysicalAddress`、`DataValidator` 和
`StorageModelOptions`。它覆盖稀疏 payload、bank/row/column、subarray/mat/cell/microbump 坐标、SECDED shadow 和 TSV-aware 热模型入口。如果外部实验只想使用堆叠存储模型而不运行完整 CLI，也可以直接包含这些头文件。
DFI beat/signal trace API 位于 `hbm_sim/validation/dfi.hpp`，适合外部脚本把命令流转成
controller/PHY 边界的 command/data beat CSV 和 DFI-like signal CSV；在线仿真生成的
`IssuedCommand` 会携带真实 request/MemoryImage payload 快照，DFI data beat 会优先使用这些真实数据。
同一头文件还提供 `DfiValidationReport` 和 `validate_dfi_trace()`，用于离线检查事件、
phase、latency、signal、payload 和 mask；请求带 `expect=` 时还会检查独立 expected payload。

使用方式：

```cpp
#include "hbm_sim/hbm_sim.hpp"
```

如果只依赖某一层，也可以 include 更窄的模块头文件，例如：

```cpp
#include "hbm_sim/dram/spec.hpp"
#include "hbm_sim/core/system.hpp"
```

修改建议：

- 头文件只声明稳定接口和必要结构，具体算法实现放在 `src/`。
- 新增 public 类型时，同步检查 `include/hbm_sim/hbm_sim.hpp` 是否需要聚合它。
- 内部源码优先 include 窄头文件，外部实验代码可以使用聚合头。

## 目录和实现层的对应关系

`include/` 和 `src/` 是镜像关系：

```text
include/hbm_sim/core        -> src/core
include/hbm_sim/dram        -> src/dram
include/hbm_sim/controller  -> src/controller
include/hbm_sim/frontend    -> src/frontend
include/hbm_sim/stats       -> src/stats
include/hbm_sim/validation  -> src/validation
```

头文件回答“外部能调用什么”，源文件回答“内部怎么实现”。如果某个函数只在一个
`.cpp` 中使用，应优先放在该 `.cpp` 的 anonymous namespace，而不是加入 public header。

## include 选择建议

- 快速实验、单文件测试、外部 demo：使用 `hbm_sim/hbm_sim.hpp`。
- 项目内部源码：使用最窄头文件，例如 `hbm_sim/dram/state.hpp`。
- 第三方工具：只 include 需要的层，减少编译依赖。
- 不要让 `core/` 头文件 include `controller/` 或 `frontend/`，否则依赖方向会倒挂。

## 新增头文件检查清单

- 文件放入正确层级，文件名使用短而清楚的名词。
- 顶部使用 `#pragma once`。
- 只声明需要跨文件共享的类型和函数。
- 同步补充对应目录的 README。
- 如果是外部常用 API，同步加入 `include/hbm_sim/hbm_sim.hpp`。
