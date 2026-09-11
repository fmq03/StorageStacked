# 命令行实现

CLI 现在是所有模型能力的总装入口：标准/profile/config、streaming workload、memory backend、真实存储区加载/导出、golden 验证、command trace、DFI trace、thermal map 和统计输出都在这里接线。新增参数时必须同步 `help.cpp`、配置解析和 README。

本目录实现命令行入口。CLI 层负责读取配置、解析参数、装配 `DramSpec`、
生成 workload、运行仿真并打印统计结果。

主要文件：

- `main.cpp`：CLI 与运行选项解析、调用配置库构建模型、仿真流程装配和结果输出。
- `help.cpp`、`help.hpp`：用户可见的命令行帮助文本。

修改建议：

- CLI 不实现协议状态机，协议行为应下沉到 `dram/` 或 `controller/`。
- 新增命令行参数时，同时更新 `help.cpp`、配置解析、README 和 smoke test。
- 输出统计字段应调用 `stats` 层，不要在 CLI 中重复计算核心指标。

## CLI 处理流程

命令行入口按下面顺序工作：

```text
argv
  -> parse_args()
  -> load_config()
  -> apply_option() 与 config::build_model()
  -> 主输入联动、Timing 换算和 DramSpec 检查
  -> build StorageModelOptions
  -> build traffic/control sequence
  -> run Controller 或 MemorySystem
  -> optional memory image / command trace / thermal map dump
  -> optional command/DFI trace validation / data mismatch report
  -> print Stats
```

这个流程让配置文件和 CLI 参数可以复用同一套字段解析逻辑。命令行参数拥有最终优先级。

## `main.cpp` 中主要区域

- 参数解析：把 `--key value` 转换成 `Cli` 结构或 `DramSpec` 覆盖项。
- 配置加载：读取 `key = value` 文件，跳过空行和注释。
- profile 展开：调用 `config/model`，统一主输入推导与 `dram/profiles`、`dram/spec` 的最终模型描述。
- workload 生成：调用 `frontend/traffic` 创建 synthetic 或 trace 请求。
- 存储模型装配：解析 `--memory-image`、`--dump-memory-image`、`--dump-memory-csv`、`--verify-golden`、`--mismatch-report`、`--dump-thermal-map`、`--power-scale`、`--thermal-*`、物理几何和 ECC shadow 参数，创建共享 `MemoryImage` 与 `DataValidator`。
- 仿真执行：选择单 controller 或多 controller memory system；多 controller 的
  流式运行交给 `MemorySystem::run(RequestSource&, ..., RunOptions)`，
  CLI 仅提供响应 CSV 和进度回调，不重复维护注入/重试/step/收尾循环。
- 输出与验证：打印配置、统计、timing table dump、command trace / DFI beat/signal trace、command/DFI validation report。

默认 `summary` 输出 MODEL—PARAMETERS—RESULTS 英文精简报告；`--stats-json` 输出分区 schema 2。
只有 diagnostic 才附加内部审计字段。CLI 收集类型化值并直接序列化 JSON；
参数变化比较使用实际 resolved 值与程序内置基准，不局限于 override 段。
模型字段的白名单、单位和相互推导在 `src/config/model.cpp`，不再在 CLI 维护第二套模型覆盖表。

## 存储模型相关 CLI

常用参数：

- `--memory-image PATH`：加载稀疏真实存储区初始内容。
- `--dump-memory-image PATH`：导出仿真结束后的真实存储区和物理坐标；`.bin` 路径会写稀疏二进制 checkpoint，其余默认写文本。
- `--dump-memory-csv PATH`：导出仿真结束后的真实存储区 CSV，适合 Excel/WPS 查看 address、data、mask、version、bank/row/tile 等列。
- `--verify-golden PATH`：仿真结束后加载 golden memory image，逐 line 比较 payload 和逐 byte initialized mask。
- `--mismatch-report PATH`：导出数据正确性错误报告。
- `--dump-thermal-map PATH`：导出访问过的 floorplan tile、能量和温度。
- `--floorplan true|false`：控制 bank 到 layer/tile 的物理放置。
- `--power-model true|false`：控制命令能量统计。
- `--thermal-model true|false`：控制 sparse 3D RC 热模型。
- `--power-scale FLOAT`：统一缩放命令能量。
- `--thermal-ambient-c FLOAT`、`--thermal-cooling-per-cycle FLOAT`、`--thermal-rise-c-per-pj FLOAT`：热模型参数。
- `--thermal-coupling true|false`：控制 tile 间横向/纵向热耦合。
- `--thermal-lateral-coupling FLOAT`、`--thermal-vertical-coupling FLOAT`：设置稀疏 RC 横向/纵向耦合系数。
- `--thermal-tsvs-per-grid N`、`--thermal-tsv-coupling-scale FLOAT`：设置 TSV-aware 垂直耦合入口。
- `--subarrays-per-bank N`、`--mats-per-subarray-x N`、`--mats-per-subarray-y N`、`--cells-per-mat-x N`、`--cells-per-mat-y N`、`--microbumps-x N`、`--microbumps-y N`：设置从 row/column/tile 派生的细粒度物理坐标。
- `--ecc-shadow true|false`、`--ecc-check-on-read true|false`、`--ecc-correct-single-bit true|false`、`--ecc-inject-period N`：控制 cache-line SECDED shadow、读校验、单 bit 纠正和错误注入。

## DFI trace 相关 CLI

DFI 6.x-oriented 输出有两层抽象：beat CSV 用于检查 command/data beat、phase、latency、真实 payload 和 payload accounting；signal-like CSV 用于把同一批事件展开成后续 MC/PHY 联调更容易消费的 `dfi_*` 字段。它仍不是完整 pin-level DFI 协议。Behavioral PHY 会把真实完成拍回填到 `IssuedCommand`；Direct 模式继续按配置的 DFI latency 推导。启用 `--dfi-trace` 或 `--dfi-signal-trace` 时，CLI 会保留 command trace，因此适合可审计的调试窗口，不适合默认保留百万级请求的完整事件。

- `--mem-phy direct|behavioral`：选择直接完成或在线行为级 PHY。
- `--dfi-version VER`：记录目标 DFI 版本标签。

- `--dfi-trace PATH` 或 `--dump-dfi-trace PATH`：导出 DFI beat trace。
- `--dfi-signal-trace PATH` 或 `--dump-dfi-signal-trace PATH`：导出 DFI-like signal trace。
- `--dfi-phase-count N`：设置 DFI phase 数；未设置时默认使用 `tick_multiplier`。
- `--dram-transaction-bytes N`：设置一条 DRAM RD/WR 命令的 payload 字节数；HBM4 x32/BL8 和 LPDDR6 x12/BL24 默认均为 `32`。
- `--dfi-data-lane-bytes N`：设置每个 DFI data beat 的 payload 字节数；未设置时由 `dram_transaction_bytes / nBL` 推导。
- `--dfi-read-latency-nck N`：设置 RD 命令到 read data beat 的延迟；未设置时使用 `nCL`。
- `--dfi-write-latency-nck N`：设置 WR 命令到 write data beat 的延迟；未设置时使用 `nCWL`。
- `--validate-dfi-trace`：生成结构化 DFI 事件并验证 command/data 配对、beat、phase、latency、signal、payload/source/mask；有 `expect=` 时还会独立比较读 data beat。

示例：

```bash
./build-clang-debug/hbm_sim --standard hbm4 --requests 8 \
  --dfi-trace outputs/hbm4_dfi.csv \
  --dfi-signal-trace outputs/hbm4_dfi_signal.csv \
  --validate-dfi-trace \
  --dfi-phase-count 2 \
  --dfi-data-lane-bytes 16
```

beat CSV 字段包括 `cycle`、`phase`、`kind`、`request_id`、`command`、`bus`、
`beat`、`beat_count`、`beat_bytes`、decoded channel/pseudo-channel/SID/bank/row/column、
`address`、`payload_source`、`payload_initialized`、`payload_init_mask` 和原始命令
`issued_cycle`。signal CSV 额外包含
`dfi_reset_n`、`dfi_cs_n`、`dfi_cke`、`dfi_odt`、`dfi_address`、`dfi_bank`、
`dfi_rddata_en`、`dfi_wrdata_en`、`dfi_rddata_valid`、`dfi_wrdata`、`dfi_wrdata_mask` 和
`dfi_rddata`。写数据的 `payload_source` 通常是 `request_payload`，读数据通常是
`memory_image`；`synthetic_fallback` 只表示离线命令没有真实 payload 快照。DFI 手册定义了
`dfi_wrdata_mask`/DBI 类总线，具体极性取决于目标 memory signal 和模式；当前项目默认导出
project-defined active-high suppress 视图，不是内部 write-enable mask。这一步的目标是建立稳定可验证的联调接口；完整 CA bit placement、training、frequency ratio change 和 low-power handshaking 仍属于后续阶段。

验证通过时 CLI 输出 `dfi_validation : PASS`，以及 `dfi_validation_events`、
`dfi_validation_command_checks`、`dfi_validation_data_checks`、
`dfi_validation_latency_checks`、`dfi_validation_phase_checks`、
`dfi_validation_signal_checks`、`dfi_validation_payload_checks` 和
`dfi_validation_expected_checks`。失败时会打印首批结构化错误并返回非零。

数据 mismatch 默认使 CLI 在完成报告导出后返回退出码 `3`；调试故障注入时可用
`--allow-data-mismatch` 只记录错误而不使进程失败。

## 新增 CLI 参数步骤

1. 在 `help.cpp` 增加帮助说明。
2. 在 `parse_args()` 中解析命令行参数。
3. 如果参数也应支持配置文件，更新 `apply_option()` 或 spec override 逻辑。
4. 如果参数属于器件/协议，确认 `DramSpec` 有字段和默认值。
5. 在 README 的配置字段说明中补充该参数。
6. 在 `tests/smoke.sh` 增加至少一个覆盖路径。

## 常见误区

- 不要在 CLI 中直接改 bank state；CLI 只装配仿真。
- 不要让 CLI 单独计算带宽；带宽口径集中在 `stats`。
- 不要只支持命令行、不支持配置文件，除非这是纯调试参数。
- 不要把长帮助文本塞回 `main.cpp`，用户可见说明放在 `help.cpp`。
