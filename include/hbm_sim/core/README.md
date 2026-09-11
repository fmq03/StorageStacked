# 核心接口

core 头文件是当前真实堆叠存储模型的主要公共入口：`data.hpp` 定义 `MemoryImage`、`DataBlock`、物理坐标、row buffer 相关接口；`stack_model.hpp` 定义可被外部 MC slice 驱动的被动 `StackModel` / `MultiStackMemoryModel`；`memory_backend.hpp` 定义 `sparse/mmap_sparse/chunk_file` 后端抽象；`system.hpp` 定义带 stack 路由、入口 QoS 和每 stack 多 channel controller 的主动运行入口。

本目录声明跨模块基础类型。这里的内容应尽量小而稳定，被 controller、dram、
frontend、stats、validation 多层共同依赖。

主要头文件：

- `common.hpp`：标准枚举、命令枚举、地址字段、timing scope、公共工具函数声明。
- `request.hpp`：读写请求、维护请求和 decoded address 的请求封装。
- `response.hpp`：Controller transaction completion、MemorySystem host response、状态、ECC 结果和读数据/初始化掩码。
- `addr_map.hpp`：单 stack DRAM 地址映射和系统地址到 stack-local 地址映射接口。
- `data.hpp`：真实存储区、payload、SECDED shadow、数据校验、bank/row/column/subarray/mat/cell/microbump 存储键、floorplan、tile 内 thermal grid、DRAMsim3-style IDD 功耗参数、TSV-aware 行为级稀疏三维热模型接口。`ecc_status_counters()` 提供不扫描存储区的 ECC 状态快照，完整统计由 `storage_stats()` 提供。
- `stack_model.hpp`：被动多 stack 器件数组接口。当前默认规模为 6 个 stack。`StackModel` 接受 transaction-level 读写或 command-level `ACT/PRE/RD/WR/REF` 事件，`MultiStackMemoryModel` 只按 `stack_id` 分发并汇总 per-stack 统计；它不实现 UCIe、Bridge 或 MC frontend 调度。
- `system.hpp`：主动多 stack / 多 channel controller system 顶层接口；负责 stack ingress、反压、QoS、并行 tick、异步响应重组和聚合统计。

`MemorySystem::RunOptions` 为流式 `run()` 提供 Host/transaction 消费回调、
进度回调和 `drain_responses`。CLI 与库共用注入、重试、step 和收尾驱动。
回调模式假设消费者 always-ready；需要响应反压时调用逐拍接口，并自行控制 pop。

修改建议：

- 只有被多层共享的类型才放入 `core/`。
- 新增命令或枚举后，要同步检查 `dram/semantics`、`dram/state`、`controller/executor`、`validation/validator`。
- 地址映射相关变化优先放在 `addr_map.hpp/cpp`，避免散落到 frontend 或 controller。
- 堆叠存储相关变化优先放在 `data.hpp/cpp`，保持 `Request` 和 `Controller` 只携带轻量引用、payload id 或 decoded 坐标。功耗/热模型若来自 DRAMsim3 或标准/手册，应在 `StorageModelOptions` 中保留来源字段和可配置参数；ECC/RAS 的第一版 payload shadow 也放在这里，避免 controller 热路径膨胀。
- `MemoryImage::load_text()`、`dump_text()`、`dump_csv()`、`load_binary()`、`dump_binary()`、`read_initialized_mask()` 和 mismatch report 是 checkpoint/report 接口；RD/WR 热路径操作内存中的稀疏结构，不应改成每次访问 txt/CSV/bin 文件。
- 参考闪存的完整数组文件模型时，必须保留 HBM/LPDDR 的容量差异：
  HBM/LPDDR 默认应使用稀疏 `MemoryImage`，最后再导出文本或 CSV。

## core 层为什么要小

`core/` 是所有层都会接触的基础。如果它变大，整个项目都会被迫重新编译，也更容易产生循环依赖。
因此这里主要放三类东西：

- 所有模块都需要理解的枚举，例如 `Command`、`Standard`、`SchedulerKind`。
- 请求和地址的通用结构，例如 `Request`、`DecodedAddress`。
- 系统级入口，例如地址映射和多 controller system。

不应放入 `core/` 的内容：

- JEDEC timing 表和器件 preset，这些属于 `dram/`。
- 调度算法细节，这些属于 `controller/`。
- trace 文件解析，这些属于 `frontend/`。
- 输出格式，这些属于 `stats/`。

## 常见修改路径

新增一个命令枚举时：

1. 修改 `common.hpp`。
2. 在 `dram/semantics.hpp/cpp` 中定义命令分类。
3. 在 `dram/state.hpp/cpp` 中定义合法状态。
4. 在 `controller/executor.hpp/cpp` 中定义副作用。
5. 在 `validation/validator.hpp/cpp` 中定义离线重放。

新增地址映射时：

1. 修改 `AddressMappingKind`。
2. 扩展 `addr_map.hpp` 接口和 `src/core/addr_map.cpp` 实现。
3. 补 sequence test 检查字段分布。
4. 如果会影响 channel 分布，补 smoke 或 compare stats。
