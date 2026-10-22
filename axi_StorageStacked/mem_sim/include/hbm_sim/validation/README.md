# 验证接口

validation 公共接口当前分三类：command trace validator、DFI beat/signal trace builder、数据正确性报告。数据 payload 的 expected/actual 比较在 controller 完成读请求时触发，command/DFI validator 则是离线后处理视图。

本目录声明命令 trace、DFI beat/signal trace 和离线验证接口，用于向 Ramulator2.1/golden trace
对比靠齐。

当前验证职责分三类：本目录负责 command trace validation 和 DFI beat/signal trace；真实
payload 的 read/write correctness 由 `core/data.hpp` 中的 `DataValidator` 负责。

主要头文件：

- `trace.hpp`：CSV command trace recorder 接口。
- `dfi.hpp`：DFI 6.x-oriented command/data beat trace、DFI-like signal trace 和 `DfiValidationReport` 接口。
- `validator.hpp`：离线 command trace validator 接口和验证报告结构。

修改建议：

- 新增命令语义或 timing scope 时，应同步更新 validator。
- validator 应重放 bank/power/WCK/refresh/RFM 状态，尽量不要只做字符串检查。
- 与 Ramulator2.1 做 differential compare 时，优先从本层扩展输入/报告格式。
- DFI 当前已有 `dfi_reset_n`、`dfi_cs_n`、`dfi_cke`、`dfi_odt`、`dfi_address`、
  `dfi_bank`、`dfi_rddata_en`、`dfi_wrdata_en`、`dfi_rddata_valid`、`dfi_wrdata`、
  `dfi_wrdata_mask`、`dfi_rddata` 的 CSV 视图。在线 controller 会把真实写 payload
  和真实读回 payload 回填到 `IssuedCommand`，因此 DFI data beat 优先来自真实存储模型；
  离线手工构造命令没有 payload 时才使用 `synthetic_fallback`。当前仍不是完整
  pin-level/training/update/low-power DFI 协议；新增信号级模型时应从 `DfiEvent`
  扩展，而不是把信号散落到 CLI。

## validation 层目标

validation 层用于回答“仿真器发出的命令流是否自洽”。它不是性能模型的一部分，
而是回归和对比工具。当前重点检查：

- 命令是否走了正确 bus。
- HBM rising/falling edge pairing 是否满足规则。
- bank 状态是否允许 ACT/PRE/RD/WR/REF/RFM。
- timing table constraint 是否被违反。
- tFAW、WCK window、power/self-refresh 状态是否合理。
- RD/WR 是否能被转换成可审计的 DFI command/data beat 和 signal 事件。
- DFI command/data 事件的数量、beat、phase、latency、signal 和 payload 是否自洽。
- 请求提供 `expect=` 时，read data beat 是否与独立 expected payload 一致。

## 报告字段

`CommandValidationReport` 中的计数字段用于说明 validator 实际检查了什么。比如：

- `checked_commands`：检查过的命令总数。
- `bus_checks`：bus 类型检查次数。
- `edge_checks` / `edge_pairing_checks`：HBM edge 相关检查次数。
- `state_checks`：命令状态机检查次数。
- `timing_checks`：timing constraint 检查次数。

新增检查项时，应补报告字段或错误信息，方便 CLI 和 smoke test 观察。

`DfiValidationReport` 对应字段为 `checked_events`、`command_checks`、
`data_beat_checks`、`latency_checks`、`phase_checks`、`signal_checks` 和
`payload_checks`、`expected_payload_checks`。事件用
`(request_id, command, issued_cycle)` 建立配对索引。`errors` 非空时 `ok()` 返回 false，CLI 的
`--validate-dfi-trace` 会把它转为非零退出。
