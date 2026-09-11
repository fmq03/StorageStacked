# HETTrace v2：AXI4 五通道格式

HETTrace v2 的一条记录是一场 AXI 通道握手，不是抽象的内存访问。写事务为
`AW + W... + B`，读事务为 `AR + R...`；同一事务由 `txn` 关联。只有 W/R 搬运字节，带宽、
footprint 和外部请求数必须以数据通道或明确的地址通道投影为准。

## 文件组织

默认每个源两个文件：

```text
host.hettrace
host.hettrace.meta.json
vortex.hettrace
vortex.hettrace.meta.json
coralnpu.hettrace
coralnpu.hettrace.meta.json
```

未启用的源不创建空文件。各文件内部 `tick`、`seq` 单调；多源需要通过 `hettrace merge` 做
稳定归并，不允许多个时钟域直接交织写同一个文件。

## 二进制布局

所有整数均为小端。文件头 64 B，记录 56 B；C++ 真值定义在
`libhettrace/include/hettrace/record.h`，Python 解码在 `tools/hettrace/reader.py`。

文件头关键字段：

| 字段 | 含义 |
|---|---|
| `version` / `record_size` | 当前固定为 2 / 56 |
| `ticks_per_second` | 全局时间基准，gem5 为 `10^12` |
| `clock_period_ticks` | 该源一个周期对应多少全局 tick |
| `src_id`, `name` | 来源身份 |
| `level` | 观察层级；主配置统一为 `interconnect`（数值 3） |
| `axi_data_bytes`, `axi_addr_bits` | AXI 总线契约 |

记录字段：

| 字段 | 含义 |
|---|---|
| `tick` | 握手发生的全局时间 |
| `addr`, `size` | 首地址/拍地址与覆盖字节数 |
| `strb` | WSTRB；只对 W 有意义 |
| `ctx`, `src_id` | 源内上下文与来源 |
| `seq`, `txn` | 每源顺序号与事务关联号 |
| `axi_id`, `axi_len`, `axi_size` | AXI ID、拍数减一、每拍字节的 log2 |
| `op`, `chan`, `burst` | 读写、AW/W/B/AR/R、burst 类型 |
| `resp`, `user` | B/R 响应与来源 USER |
| `flags` | beat、prefetch、unmapped、instruction、DMA、LAST/事务完成、SYNTH |

`SYNTH` 是关键证据边界：统一观察点收到的三个源都已经是 gem5 packet。monitor 根据包大小、
byte enable、4 KiB 边界和最多 256 拍的 AXI 约束进行确定性投影，因此三源五通道记录全部带
`SYNTH`，不可当作真实 pin-level 协议信号。CoralNPU 的原生 timing seam 在当前 16 B 单拍
子集中保留地址、ID 与 WSTRB；独立设备诊断 trace 也可只含 W/R，但统一 monitor 中的通道事件、
时序和其余 AXI 属性仍是重构值。

HETTrace 不保存 WDATA/RDATA。`WSTRB` 和地址能说明哪些字节 lane 被访问，不能证明数据值正确；
功能正确性由 workload、gem5 数据通路和 RTL scoreboard 验证。

## 元数据

`*.meta.json` 至少记录 `emitted`、`data_records`、`transactions`、`bytes`、`filtered`、
`unmapped`、`non_monotonic`、时间范围和 AXI 契约。正式分析前应要求：

- `unmapped == 0`、`non_monotonic == 0`；
- 主配置三源 `level == 3`；
- 主配置三源 `synth == true`；
- 记录数、事务数和 workload 规模相符。

## CLI

```bash
export PYTHONPATH="$PWD/tools${PYTHONPATH:+:$PYTHONPATH}"

python3 -m hettrace validate TRACE_DIR
python3 -m hettrace validate TRACE_DIR --allow-single-source  # 仅设备 bring-up
python3 -m hettrace stats TRACE_DIR
python3 -m hettrace merge TRACE_DIR -o merged.hettrace
python3 -m hettrace convert TRACE_DIR --preset timed -o timed.txt
python3 -m hettrace convert TRACE_DIR --preset memsim \
    --ticks-per-cycle 1000 -o mem_sim.trace
```

`readwrite`、`dec_readwrite` 和 `timed` 默认只取 W/R。`memsim` 对完整五通道源从 W 投影写
请求以保留 WSTRB，从 AR 展开读请求以保留发射时刻而不是较晚的 R 返回时刻；只有 W/R 的
诊断 trace 才逐数据拍转换。转换支持合法的 FIXED/INCR/WRAP，但当前可转换契约要求 AW/AR
首地址按 AxSIZE 对齐：格式没有 RSTRB，无法无损表达 unaligned 读首拍的有效 lane。非法 burst
或未映射地址默认报错。

外部 `mem_sim` 会把只有 `R/W address` 的行默认为整条 cache line。转换器因此给每行附加与
请求大小严格等长的零值 `data=`/`expect=`，写请求再附加从 WSTRB 得到的 `mask=`。这些零值
只承载大小，不是 HETTrace 未保存的真实 WDATA/RDATA。`mem_sim` 内部仍会真的写入/比较这些
零值，所以它报告的 response status 与 data mismatch 只属于零值替身，不能用于原 trace 的
功能数据校验。

转换会同时写 `mem_sim.trace.map.csv`。其中 `host_request_id` 与外部 `hbm_sim`
`--response-trace` 的 ID 对齐，保留原始源、`seq`、`txn`、通道、AXI ID、beat 和 flags。这个
sidecar 是 AXI 降级后回溯原事件的唯一依据，不应丢弃。

`--ticks-per-cycle` 是显式实验时间量化参数，不是从任意单源时钟自动推断的全局真值。比较实验
必须保持它一致并记录在结果中。
