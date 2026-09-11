# DRAM 模型实现

dram 层当前负责把 HBM3/HBM4/LPDDR5/LPDDR6 标准语义转成可运行 `DramSpec`、timing table、命令语义、状态机和接口开销。真实 payload 存储、文件后端和 CSV/bin checkpoint 不属于 dram 层。

本目录实现 DRAM 标准和器件描述层。它负责把 HBM/LPDDR 的组织结构、timing、
命令语义、状态合法性和接口开销表达成 controller 可消费的数据。

主要文件：

- `standard_traits.cpp`：标准身份、别名、协议能力和默认 profile 选择。
- `profiles.cpp`：按 speed-bin、density、stack-height、mode、vendor 展开完整组织和 timing。
- `spec.cpp`：统一 `make_spec()`、timing table、constraint 派生和 finalize。
- `jedec.cpp`：JEDEC 单位换算和常用公式。
- `semantics.cpp`：命令分类、bus/scope 元数据。
- `state.cpp`：ACT/PRE/RD/WR/REF/RFM/MR/WCK/DVFS/低功耗等合法性检查。
- `interface.cpp`：payload、ECC、DBI、CRC、metadata lane 的接口占用计算。
- `mem_phy.cpp`：在线 MC-to-stack 数据路径。Controller 仍负责 JEDEC 命令
  合法性和调度；本文件负责行为级 PHY 生命周期、FIFO 反压、HBM/LPDDR 命令编码摘要
  及异步 payload 完成。

修改建议：

- 跨器件公式优先落在 `profiles.cpp`；具体器件/模式数值落在
  `configs/hbm.cfg` 或 `configs/lpddr.cfg` 的具名 preset。
- 如果某个行为会影响命令能不能发，改 `state.cpp`；如果影响命令发出后的状态，改 `controller/executor.cpp`。
- 带宽、接口传输率和 payload efficiency 相关开销统一从 `interface.cpp` 进入统计。
- dram 层不保存真实 payload，也不直接计算热图；真实存储区、floorplan、power 和 thermal 由 `core/data.cpp` 根据 `DramSpec` 的组织结构消费。

## `spec.cpp`

`make_spec(name)` 是完整默认模型的便利入口，内部执行
`traits -> default profile -> finalize`。CLI 为了让配置优先级唯一，使用
`make_spec_draft(name) -> profile selectors -> profile -> overrides -> finalize`。

`StandardTraits` 不保存 `Organization` 或 `Timing`，只承载标准身份、别名、协议
能力、scope 和默认 profile 选择。四个私有 `apply_*_profile()` 是标准表/公式的数值
策略，不是四套公开模型构造器。命令是否合法、发出后如何迁移状态仍由
`semantics.cpp`、`state.cpp` 和 controller 实现。因此：

- 新 speed bin、density、stack height、mode 或 vendor 数据优先进入
  `profiles.cpp`/家族主配置 preset。
- 新标准若仍属于现有 HBM/LPDDR 协议族，先增加 traits/profile 数据项。
- 只有出现新的命令语义或状态迁移时，才修改协议实现。

这里需要特别注意 timing 来源：

- JEDEC 固定值或公式换算应标为 `JEDEC` 或 `derived`。
- 厂商表应标为 `vendor`。
- 临时研究值应标为 `research_default`，并在 strict timing table 中可见。

LPDDR6 当前按用户提供的 JESD209-6 路线补入了 REFdb 相关字段：

- `nREFDB2ACT`：REFdb 后到 ACT 的间隔。
- `nREFDB2REFDBS`：dual-bank refresh 到短间隔下一次 REFdb。
- `nREFDB2REFDBL`：dual-bank refresh 到长间隔下一次 REFdb。

TimingEngine 与离线 validator 分别重放每 Subchannel/SID/Rank 的刷新计数器：
同一刷新行内用短间隔，完成所有 Bank 对后到下一行用长间隔（JESD209-6 §7.6.1/7.6.2）。
一轮完成前禁止重复 Bank；REFab 和自刷新退出重置计数。两个目标 Bank 都保留 nRFCpb 恢复。
当前只表达相同 BA、相邻 BG 的固定配对（0↔1、2↔3），不表达标准允许的全部 dBG 组合；
非标准组织按实际 Bank 数/2 推进，是研究扩展。短/长选择本身不再无条件使用长间隔。

## `profiles.cpp`

`profiles.cpp` 将速率、几何折算密度、堆叠高度和实际功能参数展开成具体时序。
mode_profile/vendor_profile 是名称标签，不会自动生成对应功能或厂商校准证据。
实验复制 `configs/hbm.cfg` 或 `configs/lpddr.cfg` 后在配置内覆盖时序。
`spec.cpp` 共用密度公式并兜底校验几何/时钟/nRC 一致性，
内部单 Channel 视图通过父 Channel 数保留完整器件密度，而不重查刷新表。

## `state.cpp`

`state.cpp` 判断命令是否合法，例如：

- bank 是否 closed/opened/activating。
- REF/RFM 是否要求 all-bank idle。
- LPDDR CAS/RD/WR 是否需要 WCK ready。
- power-down/self-refresh 状态是否阻塞普通命令。
- mode register、training、DVFS、RAS/ECC 命令是否满足状态条件。

它不执行状态变化。状态变化在 `controller/executor.cpp`。

## `interface.cpp`

`interface.cpp` 负责把 payload byte 和协议开销分开。这里会影响：

- `interface_read_bytes` / `interface_write_bytes`。
- `interface_overhead_bits`。
- `achieved_if_bw_GBps`。
- `payload_efficiency_pct`。

HBM CRC/ECC/RAS metadata、LPDDR DBI/link ECC/CA parity 都应通过这里进入接口统计。
这些 bit 当前只做接口流量和效率记账，不会自动增加 RD/WR 占用周期。因此
`achieved_if_bw_GBps` 是等效需求带宽，不能解释为已经受链路容量约束后的实测带宽；
需要研究保护开销对吞吐量的影响时，还要给数据通路加入对应的序列化/重放时延。

## DFI 字段

`DramSpec` 包含 DFI 6.x-oriented PHY/trace 抽象字段：

- `dfi_phase_count`
- `dfi_data_lane_bytes`
- `dfi_read_latency_nck`
- `dfi_write_latency_nck`

这些字段描述 Behavioral PHY 和 DFI 输出所需的相位、data beat 粒度及读写延迟。
版本标签统一为 `6.0.1`，但不按字符串切换实现。`dfi_phase_count` 仍用于事件相位；
适配器中无消费者的 `dfi_phases` 已删除，二者不是同一个字段。
`validation/dfi.cpp` 会基于它们生成两类输出：

- beat CSV：`COMMAND`、`READ_DATA`、`WRITE_DATA` 事件。
- signal-like CSV：`dfi_reset_n`、`dfi_cs_n`、`dfi_cke`、`dfi_odt`、`dfi_address`、
  `dfi_bank`、`dfi_rddata_en`、`dfi_wrdata_en`、`dfi_rddata_valid`、`dfi_wrdata_mask`、
  `dfi_wrdata`、`dfi_rddata` 等字段。

signal-like CSV 保留上述历史字段供现有工具读取，并非 DFI 6.0.1 端口原名。
规范使用 `dfi_cmdaddr`、`dfi_cs`、`dfi_reset`、`dfi_wrdata_dbi_mask`；项目的
address/bank/cke/odt 等是解码后的行为摘要，不能通过简单改名获得规范 packing 或极性语义。

真实 write/read payload 与 Behavioral PHY completion cycle 由 controller 回填到
`IssuedCommand`，DFI builder 按真实长度切分 beat；没有 payload 快照时才使用
`synthetic_fallback`。它们仍不代表完整 DFI pin-level 协议；DRAM bank 状态合法性仍由
`state.cpp` 和 controller executor 负责。没有厂商 PHY
资料支撑的 lane 拓扑、训练流程和精确 pin packing 暂时保持 project-defined。
