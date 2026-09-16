# 定制化 HBM/LPDDR 架构实验

该实验只通过两份权威 master 配置加临时 `[override]` 改变模型，不维护第三份入口配置。
每个 case 以**该标准自己的完整标准组织**为基线，只做一处改动：

- **密度组**（3 点）：按各标准的真实 JEDEC 规格取密度档位。真实器件变密度时
  **不变**的是 `channels`、`banks_per_group`、`columns`；**变**的是 `rows`，
  以及（HBM4 的堆叠配置）`stack_height` 与随之确定的 SID 域数量 `sids`。
- **refresh 组**（2 点）：只改 `refresh_policy`（`per_bank` / `all_bank`）。
- **组织扰动组**（4 点）：三个单键 ×2 的点（`banks_per_group`、`bank_groups`、
  `columns` 各放大一倍），加一个 `mixed_shift` ——同时挪动
  `banks_per_group`/`bank_groups`/`rows`/`columns` 四个键、有增有减。**这些键在真实
  器件里是随密度档固定的**，所以本组是有意偏离 JEDEC 的研究型扰动，不是器件规格。

基线不是硬编码的，而是每个标准跑一次 `--check-config --dump-resolved-config`
（约 7 ms，不跑仿真）解析出来，存为 `<out>/<standard>/baseline.cfg`。这一点是刻意的：
容量门禁要拿 Python 按容量公式算出的容量与引擎报告的 `aggregate_capacity_bytes`
比对，如果两边都来自同一张硬编码表，检查就成了循环论证。

## 密度档位与容量

| 标准 | case | 变化的组织键 | 容量 |
|---|---|---|---|
| HBM3 | `8gb_8h` | rows 8192 | 8 GiB |
| | `16gb_8h` | 基线对照 | 16 GiB |
| | `32gb_8h` | rows 32768 | 32 GiB |
| HBM4 | `32gb_8h` | 基线对照 | 32 GiB |
| | `24gb_12h` | stack_height 12、sids 3、rows 12288 | 36 GiB |
| | `32gb_16h` | stack_height 16、sids 4 | 64 GiB |
| LPDDR5 | `12gb_x16` | rows 49152 | 1.5 GiB |
| | `16gb_x16` | 基线对照 | 2 GiB |
| LPDDR6 | `8gb_sc` | rows 32768 | 2 GiB |
| | `16gb_sc` | 基线对照 | 4 GiB |
| | `24gb_sc` | rows 98304 | 6 GiB |

容量 = `channels × pseudo_channels × sids × ranks × bank_groups × banks_per_group ×
rows × columns × dram_transaction_bytes`，**不含 `stack_height`**。

取值依据：HBM3 的 `SID, BA[3:0]` 恒为 32 banks/PC、`4 banks/BG`，密度只由 rows 区分
（8Gb RA[12:0]、16Gb RA[13:0]、32Gb RA[14:0]）。HBM4 的 `8 banks/BG` 固定，
8Hi/12Hi/16Hi 分别给出 2/3/4 个 SID 域，即 32/48/64 banks/PC；24Gb die 因
RA[13:12]=11 非法，rows 为 3×2¹² = 12288。LPDDR6 每 SC 恒 4 BG × 4 = 16 banks、
page 2 KB，24Gb/SC 的 rows 为 2¹⁶+2¹⁵ = 98304。LPDDR5 恒 16 banks，
12Gb 的 rows 为 2¹⁵+2¹⁴ = 49152。

**LPDDR5 的 x8 模式档位没有收录。** x8（128Mb × 8DQ）在同密度下把接口位宽减半，
但引擎的 `achieved_bw_GBps` 只按 payload ÷ 仿真时间计算，**不随 `data_bus_bits` 变化**
（`src/controller/controller.cpp`）；实测把 `data_bus_bits` 设成 8 会让
`peak_bandwidth_GBps` 减半（12.8→6.4 GB/s）而 `achieved_bw_GBps` 纹丝不动（9.526），
即 achieved > peak、利用率 >100%，物理上不可能。在“半宽接口下相同 payload 耗时翻倍”
这层耦合明确之前，该档位无法被忠实建模，因此只保留严格同宽度的 x16 两点。

`stack_height` 与 `sids` 都显式写在 override 里，不依赖引擎从 `stack_height`
自动推导 `sids`：隐式推导会让 trace 生成器按旧 `sids` 编码地址、被新 `sids` 的引擎
解码，落点与预期不符。

## 组织扰动组

| case | 变化的键 | 容量 |
|---|---|---|
| `bpg_double` | `banks_per_group` ×2 | 基线 ×2 |
| `bg_double` | `bank_groups` ×2 | 基线 ×2 |
| `cols_double` | `columns` ×2 | 基线 ×2 |
| `mixed_shift` | `banks_per_group`/`bank_groups`/`rows`/`columns` 四键，有增有减 | 基线 1.5～2.25 倍 |

前三个是单键 ×2，容量都恰好是基线的 2 倍，因此彼此可直接比较——差别只在**这 2 倍
容量加在哪个维度上**。例如 HBM4 的 `bpg_double`（2bg × 16bpg = 4096 banks）与
`bg_double`（4bg × 8bpg = 4096 banks）bank 总数相同，但 bank group 的划分不同，
因而走不同的时序作用域（`nRRDL` 按 BG、`nCCDL` 按 BG）。

`mixed_shift` 则一次性挪动四个键、方向不一致，用来观察模型在"多个维度同时偏离"
时是否仍然自洽。取值由固定种子 `20260915` 的伪随机序列生成后**冻结成表**
（见 `run.py` 的 `MIXED_SHIFT`），不在运行时随机——否则实验不可复现，
`resolved.cfg` 也无法作为审计依据。生成时施加三条约束：`bank_groups` 取偶数且 ≥2
（LPDDR REFdb 相邻 BG 配对校验要求）、至少一个键变小且至少一个变大、容量比落在
[0.4, 4] 以免退化成容量实验。

各标准的 `mixed_shift` 取值：

| 标准 | banks_per_group | bank_groups | rows | columns |
|---|---|---|---|---|
| HBM3 | 4→**2** | 4→**12** | 16384→**32768** | 32→**24** |
| HBM4 | 8→**6** | 2→**4** | 16384→**8192** | 32→**64** |
| LPDDR5 | 4→**2** | 4→**12** | 65536→**131072** | 64→**48** |
| LPDDR6 | 4→**2** | 4→**8** | 65536→**196608** | 64→**48** |

注意 LPDDR6 的 bank 总数**不变**（2×8 = 4×4 = 16 banks/PC），变的是行数与列宽，
因此它在几何上是另一个器件配置，而不是 bank 数不同的配置。

**本组的请求数与基线对照相同**，即只有组织变、负载不变。这是唯一不混淆变量的比较
方式；若按各自的 bank 数放大请求数，每 lane 负载会跟着变，就分不清差异来自组织还是
来自负载。代价是**bank 数翻倍时覆盖率必然降到约一半**（固定请求数下新增的 bank
到不了）。因此在 HBM3/HBM4 上，三个 case 实际访问的物理 bank 集合与基线一致，
带宽逐位相同——这是预期行为，不是缺陷。

脚本因此**只对本组做容量核算与单键漂移校验，不给带宽趋势结论**：在未覆盖到的
bank 上，带宽差异既可能来自并行度，也可能只是负载被摊薄。`summary.md` 会把三个
case 的 bank 数/覆盖率/带宽与基线对照并列出来供人工判读。

## 定向 trace

- **密度组**：请求同时到达并轮转全部 `(lane, bank)`；同一 bank 每轮访问一个从未使用过的
  row，避免 row wrap 产生的 FR-FCFS 命中把"可并行 bank 数"与"调度器重排行命中"
  混在一起。`lane = index % lanes` 与被扫的组织无关，保证各档呈现给机器的 lane/bank
  分解方式一致。
- **refresh 组**：跨 lane 铺开、行数受限以保持页打开、持续到达；两个 case 使用**完全相同**
  的 trace，使 `refresh_policy` 成为唯一变量。

定向地址按 `AddressMapper::Default` 的完整 8 级顺序编码，低位到高位为
`column, bank, bank_group, pseudo_channel, sid, rank, channel, row`，byte address 首先
除以 `dram_transaction_bytes`（32 B）得到 line。**不要把 64 B host line 当成列步长**；
每个 host request 仍可在 frontend 中拆成两个 32 B transaction。

## channel_mapper 为什么改成 decoded

`configs/hbm.cfg` 默认 `channel_mapper = xor`，请求进哪个 channel 由
`(line ^ (line>>6) ^ (line>>12)) % channels` 决定，会**覆盖**地址解出的 `channel` 字段。
该哈希多对一、没有闭式反解，实验无法控制落点；在 Python 里复刻它还会与 C++ 内部实现
静默耦合。因此本实验显式写 `channel_mapper = decoded`（LPDDR 本来就是 `decoded`，
不受影响）。代价是结果不再代表 HBM 主配置默认的 xor 路由行为，这是为定向隔离而改的
口径，已在 `resolved.cfg` 与 `result.json` 中逐 case 记录。

## 请求数

refresh 组用 `--requests`（默认 512）。密度组取 `max(--requests, 该档 bank 数)`，
使各档**每 lane 负载相等**——lane 数随密度档变化，若把请求数固定成常数，档位越高
每 lane 分到的请求越少，带宽下降只是负载摊薄而非并行度变差。可用 `--density-requests`
固定该值（smoke 用它保持快），此时脚本会显式报告"不可比较"而不是给出误导性趋势。

组织扰动组则相反，取**与基线对照相同的请求数**（同样是 `max(--requests, 基线 bank 数)`），
因为本组 lane 数不变，按各自 bank 数放大只会让每 lane 负载跟着变。两种取法的取舍见
上一节。

## 自动门禁

| 检查 | 判定 |
|---|---|
| `completion` | cycles>0、未撞 cycle limit、`data_mismatches=0` |
| `capacity` | Python 按容量公式算出的组织容量 **严格等于** 引擎报告的 `aggregate_capacity_bytes`（全为整数，无浮点容差） |
| `organization drift` | 每个 case 的 `resolved.cfg` 只在它声明的字段上偏离基线——把"单点改动"变成可执行断言 |
| `baseline stability` | 覆盖为空的那个对照 case 逐字段复现解析出的基线 |
| `builtin org fixture` | 模块内 `STANDARD_ORGS`（smoke fixture）与解析出的基线一致 |
| `density basis` | `density_gb` 与容量口径的关系固定下来，防止被当容量读 |
| `address encoding` | 用 `--cmd-trace` 导出的显式 DRAM 坐标校验 Python 侧地址编码与引擎解码一致 |
| `density behavior invariance` | 组织形状相同的密度档（只差 rows）之间，行为指标必须逐位相同 |
| `density scaling` | 只在各档 bank 数不同**且**每 lane 负载相等时比较带宽；否则显式说明无可分辨的并行度轴或不可比较 |
| `refresh scope` | per-bank case 必须有 PB 批且 AB 批为 0，all-bank case 反之 |

`capacity` 与 `organization drift` 对**全部三个组**逐 case 生效，组织扰动组也受这两条
覆盖（每个 case 仍必须只偏离基线一个键、且容量算术成立）。`density behavior invariance`
与 `density scaling` **只作用于密度组**——它们的前提是该组是 JEDEC 密度轴，混入
bank/bank_group/columns 扰动会让结论失真，因此代码里按 `group == "density"` 过滤。

**`density behavior invariance` 为什么是正向断言。** 按 JEDEC，HBM3 与 LPDDR 的 bank
组织与列宽在全密度档固定，因此这些标准的密度轴只改变容量与地址范围，行为指标本就
应当逐位相同。写成断言是把"三点没差异"变成一个被检验的事实，而不是让它伪装成
空洞的通过。

**`density scaling` 只在 HBM4 上有效。** 只有 HBM4 的 bank 数随密度档变化
（SID 域 2/3/4 → 2048/3072/4096 banks）；HBM3 恒 1024、LPDDR5 恒 16、LPDDR6 恒 32，
这些标准没有可分辨的并行度轴，脚本会明说而不是编造趋势。允许 1% 输出精度余量。

`density_gb` 是 Gibit 口径（HBM 按每 die、LPDDR 按每通道子通道），**不是容量**；
容量一律读 `aggregate_capacity_bytes`。任一检查失败脚本返回非零。

## 用法

```bash
# 默认扫描当前两种新标准 HBM4 和 LPDDR6
python3 experiments/architecture_sweep/run.py

# 统一复跑四标准
python3 experiments/architecture_sweep/run.py \
  --standards hbm3,hbm4,lpddr5,lpddr6

# 快速查看 HBM4 工具链
python3 experiments/architecture_sweep/run.py --standards hbm4 \
  --out outputs/experiments/architecture_sweep_quick
```

单 case 默认 120 秒超时，可用 `--case-timeout` 调整。输出包含 `results.csv`、
`checks.csv`、`summary.md`、离线 `trends.html`，以及每个 case 的 `workload.trace`、
`resolved.cfg`、`stats.txt` 与每个标准的 `baseline.cfg`、`probe/cmd_trace.csv`。

刷新组覆盖的短 `nREFI/nREFIpb/nRFC/nRFCpb` 标记为 `research`：其中
`nREFI/nREFIpb` 取 32 是为了让维护批次数在短窗口内可观测，**不是物理间隔**，
只用于压力验证，不替代 master 中的 JEDEC/reference timing。

注意：聚合文件（`results.csv` 等）按本次运行的标准重写，但各标准的 case 子目录**不会
被清理**。先跑四标准再跑单标准，旧标准的目录会留在磁盘上而聚合表里没有它们，
容易误读成"结果还在"。

结果属于模型参数敏感性研究，不代表某款器件。
