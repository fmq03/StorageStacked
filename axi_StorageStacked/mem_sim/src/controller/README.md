# 控制器实现

controller 层当前只处理请求调度和命令完成语义：写完成时调用 `MemoryImage::write()` commit payload，读完成时调用 `MemoryImage::read()` 取 actual payload，read-forward 从 write buffer overlay pending write，并为每个已接受的读写生成 `TransactionResponse`。不要在这里实现具体 `sparse/mmap/chunk` 后端或 host 多事务重组。

本目录实现 controller 层。它连接 request buffer、scheduler、row policy、
refresh/RFM manager、timing engine 和 command executor，决定每个 cycle 发射什么命令。

主要文件：

- `controller.cpp`：controller 顶层协调、请求生命周期、HBM 双发射、LPDDR split activate。
- `executor.cpp`：命令副作用执行，包括 bank 状态、timing 更新、RFM 计数和命令统计。
- `timing.cpp`：scope-level timing gate、tFAW、WCK window、table-driven constraints。
- `scheduler.cpp`：FRFCFS/FCFS 队列选择语义。
- `row_policy.cpp`：open/closed/ClosedCAP 行策略。
- `refresh.cpp`、`rfm.cpp`：refresh/RFM 维护请求生成与轮转。

修改建议：

- Controller 只负责协调，命令合法性放在 `dram/state.cpp`，命令副作用放在 `executor.cpp`。
- 新增维护类命令时，要同时考虑 priority buffer、timing、stats 和 validator。
- HBM/LPDDR 的差异应尽量由 `DramSpec` 字段驱动，避免在 controller 里散落硬编码。
- 真实存储区副作用放在请求完成和命令事件路径：WR 完成时 commit payload，RD 完成时读取/校验 payload，ACT/PRE/RD/WR/REF/RFM 事件进入 `MemoryImage` 的 row buffer、power 和 thermal 模型。

## 每个文件的职责边界

`controller.cpp` 是协调层，不应该无限膨胀。它可以做这些事：

- 通过 `RequestQueues` 管理 read/write/priority/active buffer；
  队列组件集中处理容量查询、active 提升/移除和每 bank 的 active 所有权计数。
- 处理请求进入、完成、读转发、写合并和 channel-local transaction response。
- 调用 scheduler、row policy、timing engine、executor。
- 维护 HBM row/column bus 发射顺序和 LPDDR split activate 流程。

它不应该做这些事：

- 直接写 JEDEC timing table。
- 在局部硬编码命令类别。
- 直接格式化输出统计文本。
- 把离线 validator 当作在线发射 gate；离线重放保留独立状态与规则检查。

`executor.cpp` 是命令副作用集中地。发出 ACT/PRE/RD/WR/REF/RFM/MR/WCK/DVFS 后，
bank 状态、timing 更新、RFM 计数和命令计数应尽量在这里完成。

`timing.cpp` 是 timing gate。它关注“当前 cycle 能不能发某命令”，包括 table-driven
constraint、scope bucket、tFAW、WCK active window、HBM/LPDDR 相关 bus 约束。

`refresh.cpp` 和 `rfm.cpp` 只负责产生维护目标和维护计数，不直接改普通请求排序。

Scheduler 只对 eligible/ready 候选排序，row policy 决定关页行为，Controller
负责跨队列仲裁及 HBM/LPDDR 协议流程；不能把这些规则塞进队列组件。
CAS_RD/CAS_WR 共用 WCK 状态更新，仅分别增加读/写 CAS 计数。

## 修改流程示例

新增一个维护类命令时，建议按下面顺序修改：

1. 在 `core/common.hpp` 增加命令枚举。
2. 在 `dram/semantics.cpp` 标记命令类别、bus 和 scope。
3. 在 `dram/state.cpp` 定义命令何时合法。
4. 在 `controller/executor.cpp` 实现命令副作用。
5. 在 `controller/timing.cpp` 或 timing table 中补约束。
6. 在 `stats` 中补计数或输出字段。
7. 在 `validation/validator.cpp` 补离线重放规则。
8. 在 `tests/sequence_tests.cpp` 加命令序列测试。

## 常见风险

- 只在在线 controller 中实现规则，但 validator 没同步，导致 trace validation 失真。
- 只更新命令计数，没有更新 interface overhead，导致带宽利用率错误。
- HBM 半周期 tick 和普通 nCK 混用，导致 edge pairing 或 timing 间隔偏移。
- LPDDR WCK/CAS 状态只在 RD 路径更新，WR 路径忘记同步。
