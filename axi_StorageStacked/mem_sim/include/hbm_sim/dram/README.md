# DRAM 模型接口

dram 头文件描述器件规则而不是存储 payload。当前 `DramSpec` 负责 HBM/LPDDR 组织、timing、DFI、ECC/link metadata、refresh/RFM、power source 字段；真实数据保存由 core 层处理，dram 层只提供地址、命令和 timing 语义。

本目录声明 DRAM 标准、器件组织、timing、命令语义和接口模型。它描述“器件是什么样”，
controller 层再消费这些描述做调度和发射。

主要头文件：

- `spec.hpp`：`DramSpec`、组织结构、timing table、profile 字段和协议开关。
- `profiles.hpp`：按 speed-bin/density/stack-height/mode/vendor 展开 profile 的接口。
- `jedec.hpp`：ps/ns/us 到 nCK 的 JEDEC 单位换算工具。
- `semantics.hpp`：命令类别、bus 类型、scope 等命令元数据。
- `state.hpp`：命令合法性状态机接口。
- `interface.hpp`：payload、ECC、DBI、CRC、metadata 等接口开销计算接口。
- `bank_state.hpp`：bank-local 状态结构。
- `mem_phy.hpp`：MC 与 stack 之间的行为级 PHY。它定义 Direct/Behavioral
  模式、DFI 生命周期、FIFO、完成队列及 HBM/LPDDR 协议适配器；实现位于
  `src/dram/mem_phy.cpp`。它不替代 JEDEC 调度器，也不声称是厂商 pin-accurate
  PHY，建模边界见 `堆叠存储模型交付手册.md` 的 2.5 节。

修改建议：

- 标准或厂商表数值优先进入 `spec/profiles`，不要硬编码在 controller。
- 命令是否合法由 `state` 判断，命令造成什么副作用由 controller executor 执行。
- 接口带宽和 payload 带宽的差异统一通过 `interface` 计算。

## dram 层的职责

`dram/` 描述“器件和协议是什么”，不描述“controller 如何排队”。它主要回答：

- 当前是 HBM4、HBM3、LPDDR6 还是 LPDDR5。
- 有多少 channel、pseudo-channel、SID、bank group、bank、row、column。
- 每条命令属于哪类 bus、scope 和状态转换。
- timing 参数来自 JEDEC、vendor、derived 还是 research default。
- 读写 payload 之外还有多少 CRC/ECC/DBI/metadata 接口开销。

## 核心数据结构

- `DramSpec`：单次仿真的标准、组织、timing 和协议开关快照。
- `density_reference_channels`：仅内部单 Channel 视图记录父 Channel 数；用户完整模型保持 0，不通过 cfg 设置。
- `TimingTable`：带来源标注的 timing 条目集合。
- `BankState`：bank-local open row、状态、last command 等信息。
- `CommandMetadata`：命令类别、bus、scope、是否 row/column/maintenance 命令。
- `CommandState` 相关接口：判断命令在当前 bank/system 状态下是否合法。

## 关键新增字段

LPDDR6 REFdb timing 字段位于 `Timing`：

- `nREFDB2ACT`
- `nREFDB2REFDBS`
- `nREFDB2REFDBL`

它们来自 LPDDR6 dual-bank refresh 约束，用于避免把 REFdb 简化成普通 per-bank
refresh 后丢失关键间隔。当前调度策略保守使用 long 间隔，后续可按 bank-pair 拓扑细化。

DFI trace 字段位于 `DramSpec`：

- `dfi_phase_count`
- `dfi_data_lane_bytes`
- `dfi_read_latency_nck`
- `dfi_write_latency_nck`

这些字段同时服务于在线 `MemPhy` 和 `validation/dfi.hpp`。Behavioral PHY 把真实完成拍与 payload 记录到 `IssuedCommand`，DFI builder 再按 `dfi_data_lane_bytes` 切分 beat；Direct/离线命令仍可按配置 latency 推导。完整 DFI CA bit placement、模拟训练和厂商私有 lane 结构不在 dram header 内实现。

## 修改边界

- 数值表：优先改 `spec.hpp/cpp`、`profiles.hpp/cpp` 或配置 profile。
- 命令类别：改 `semantics.hpp/cpp`。
- 命令合法性：改 `state.hpp/cpp`。
- 接口占用：改 `interface.hpp/cpp`。
- JEDEC 单位换算：改 `jedec.hpp/cpp`。

不要在 `dram/` 里直接操作 request buffer；它不知道请求排队策略。
也不要在 `dram/` 里保存真实数据内容；payload、storage key、floorplan、power 和 thermal 状态属于 `core/data.hpp`。
