# 验证实现

validation 层当前做离线可审计视图：command trace 回放 bank/timing/bus 状态，DFI builder 从 `IssuedCommand` 生成 beat/signal CSV，DFI validator 检查事件结构和数据窗口；有请求 `expect=` 时，它还会用独立期望值检查读 payload。数据 mismatch 报告由 core/controller 提供 payload 结果。validation 层不直接修改仿真状态。

本目录实现命令 trace 导出、DFI beat/signal trace 导出和离线验证。validation 层用于检查
仿真输出命令流是否满足 bus、edge pairing、bank state、timing constraint、tFAW、
WCK window 等规则，也提供面向 DFI 6.x 的 command/data beat 和 signal CSV 视图。

注意当前项目有两条验证线：

- 命令流验证在本目录，关注命令是否合法、是否满足 timing/state。
- DFI beat/signal trace 在本目录，关注 RD/WR 命令如何展开为 controller/PHY 边界上的
  command/data beat 事件，并给出第一版 `dfi_reset_n`、`dfi_cs_n`、`dfi_cke`、`dfi_odt`、
  `dfi_address`、`dfi_bank`、`dfi_rddata_en`、`dfi_wrdata_en`、`dfi_rddata_valid`、
  `dfi_wrdata`、`dfi_wrdata_mask`、`dfi_rddata` CSV 视图。
- 数据正确性验证在 `core/data.cpp` 的 `DataValidator`，关注 RD 返回的 actual payload 是否等于 expected payload。

二者互补：命令合法不代表数据一定正确，数据正确也不代表命令 timing 一定合法。

主要文件：

- `trace.cpp`：CSV command trace recorder。
- `dfi.cpp`：DFI 6.x-oriented beat/signal trace generator 和 validator。
- `validator.cpp`：command trace validator，重放命令状态并生成验证报告。

修改建议：

- 新增协议命令后，必须同步更新 validator 的状态重放逻辑。
- validator 报告字段应保持稳定，便于 smoke test 和未来 golden trace 对比。
- Ramulator2.1 固定配置外部参考由 `tools/ramulator2_differential.py` 驱动，只覆盖共同命令面；项目原生 validator 和数据语义不以其结果为裁决标准。

## `trace.cpp`

`trace.cpp` 把在线仿真发出的 `IssuedCommand` 写成 CSV。CSV 适合三类用途：

- 人工检查命令序列。
- smoke test 检查关键字段是否存在。
- 后续和 Ramulator2.1 或真实器件 golden trace 做 differential compare。

导出字段应保持稳定；新增字段时尽量追加到末尾，减少脚本兼容问题。

## `dfi.cpp`

`dfi.cpp` 把 `IssuedCommand` 后处理成 DFI beat-level CSV 和 DFI-like signal CSV。当前事件类型包括：

- `COMMAND`：每条 issued DRAM command 都生成一条命令事件。
- `READ_DATA`：`RD/RDA` 在 `dfi_read_latency_nck` 或 `nCL` 之后展开为多个 data beat。
- `WRITE_DATA`：`WR/WRA` 在 `dfi_write_latency_nck` 或 `nCWL` 之后展开为多个 data beat。

可配置字段包括 `dfi_phase_count`、`dfi_data_lane_bytes`、`dfi_read_latency_nck`、
`dfi_write_latency_nck`。data beat 数优先按 `IssuedCommand::payload` 的真实字节数展开；
没有 payload 快照时才按 `DramSpec::transaction_bytes()` 生成
`synthetic_fallback`。在线控制器会在
`commit_write_data()` 中把最终写 payload 绑定到 `WR/WRA`，在 `complete_read()` 中把
`MemoryImage::read()` 的 actual payload 绑定到 `RD/RDA`，并把请求携带的 expected payload
单独绑定到读命令。每个 command/data 事件还保存原始命令 `issued_cycle`，避免仅由 data
arrival cycle 反推命令身份。因此 DFI CSV 中的 `dfi_wrdata` 和
`dfi_rddata` 可以和最终 memory image、mismatch report 对照。

DFI 手册定义了 `dfi_wrdata_mask`/DBI 类总线，具体极性取决于目标 memory signal 和模式；
当前项目默认按 project-defined active-high suppress 视图导出：内部 `MemoryImage` byte mask
非零表示写入，导出为 `00`；内部 byte mask 为 0 表示屏蔽，导出为 `ff`。`payload_init_mask`
是项目自定义审计字段，用来标明读回数据是否来自已经初始化的存储 byte，不是 DFI 标准 pin。

signal CSV 使用稳定的 `encode_dfi_address()` 打包 decoded 坐标，后续如果接入真实 PHY pin map，
只需要替换该编码和 lane/phase 数据绑定。当前按 DFI 6.0 公开功能类别实现 MC/PHY 边界思想，
但仍不是完整 DFI pin-level、模拟训练或厂商 lane 协议模型；
完整 HBM4/LPDDR6 pin-level 接口需要对应 DFI 版本与厂商 PHY profile 继续校准。没有厂商
PHY 私有数据的结构暂时保持 project-defined 配置项和审计字段。

`validate_dfi_trace()` 不复用 CSV 文本做字符串判断，而是直接检查结构化 `DfiEvent`：

- 每条 issued command 只有一个 `COMMAND` 事件。
- RD/WR data beat 的数量、序号、宽度、地址和 latency 正确。
- phase 和 cycle 一致，DFI signal 组合符合当前事件类型。
- payload、initialized mask、write mask 的长度和 `payload_source` 合法。
- 有独立 expected payload 时，逐 beat 比较 read data，而不是拿同一份 actual payload
  自己验证自己。

validator 先按 `(request_id, command, issued_cycle)` 建立 command/data 索引，再完成配对
和 beat 检查，避免为每个数据命令反复扫描完整事件数组。完整 trace 本身仍按事件数占用内存，
所以百万级压力测试默认不应启用 trace；需要审计时应截取较小窗口。

`tests/sequence_tests.cpp` 会破坏 read beat 周期、write payload 宽度、同宽 payload
内容、独立 expected payload 和 read command enable，确认内容与信号错误路径都可达。

## `validator.cpp`

validator 离线重放命令流。它不看原始请求，只看已经发出的命令，因此能检查 controller 输出是否自洽。
当前重放内容包括：

- bank open/closed/activating 状态。
- ACT/PRE/RD/WR/RDA/WRA 的基本合法性。
- REFab/REFpb/REFdb 和 RFMab/RFMpb 的维护状态。
- HBM edge pairing 和 falling-edge PRE 约束。
- LPDDR WCK/CAS window、DVFS 后 retrain、power/self-refresh 状态。
- table-driven timing constraint 和 tFAW。

## 新增 validator 规则步骤

1. 明确规则属于 bus、edge、state、timing 还是 WCK/power 特殊状态。
2. 在 validator 内增加状态记录或检查函数。
3. 报告中增加检查计数或错误信息。
4. 在 `tests/sequence_tests.cpp` 中加入正例和反例。
5. 如果 CLI 会暴露结果，同步更新 `src/stats` 或 CLI 输出字段。
