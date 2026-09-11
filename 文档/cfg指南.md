# cfg指南

本文件是配置项含义、可修改范围和参数联动的唯一详细说明。构建、数据路径与运行操作见
[项目指南](项目指南.md)，指标与性能解读见[输出指南](输出指南.md)，资料可信度见[审计](审计.md)。
以下命令均在仓库根目录执行；配置表使用 canonical key，分节简写见下表。
表中 `mats_per_subarray_x/y` 等是成对字段的缩写，实际写成 `_x` 或 `_y` 两个键；
斜杠和通配符不是配置键的一部分。字段名大小写经解析器归一，推荐沿用模板写法。

<a id="usage"></a>

## 1. 配置使用流程

日常从 `configs/hbm.cfg` 或 `configs/lpddr.cfg` 复制一份自包含配置，再修改副本。
HBM 家族选 hbm3/hbm4，LPDDR 家族选 lpddr5/lpddr6。不需要另加载开发者配置。
`configs/validation/` 用于共同命令面验证，不能当作完整器件组织；
`examples/configs/` 是自包含演示，`experiments/local/` 是研究用例。

```bash
mkdir -p experiments/local outputs/cfg_example
cp configs/hbm.cfg experiments/local/my_model.cfg
# 编辑副本，选择 [model] base_standard 和下面需要的输入项
./build-clang-debug/hbm_sim --config experiments/local/my_model.cfg \
  --check-config --dump-resolved-config outputs/cfg_example/resolved.cfg
./build-clang-debug/hbm_sim --config experiments/local/my_model.cfg \
  --requests 128 --stats-json outputs/cfg_example/result.json \
  --dump-timing-table outputs/cfg_example/timing.csv
```

只有确实要新建副本时才执行 cp；不要覆盖已有实验。LPDDR 同理，替换复制源即可。
`--check-config` 检查实际选中的参数组合，不运行请求；通过后仍需小规模数据和时序回归。
最终默认值来自“内置标准 profile + 当前配置 + 选择的 section”，不要把模板中省略的字段当成零。
`resolved.cfg` 是可重载的生效快照，不是适合继续改少量主参数的简洁模板：
它展开了派生值，再改几何或速率时也需要省略相应派生字段；只有 2.2 列出的字段支持 auto，
普通 Timing 不支持 auto。

### 1.1 分节、选择和优先级

```text
内置标准基准 < 公共 section < family < standard < preset < override < CLI
```

只有所选 standard/preset 的段生效。CLI 优先级与它在 `--config` 前后的位置无关；
多份配置也按全局 layer 合并，并非后一文件所有字段无条件胜出。
`[override]` 是覆盖段名，不是“后面所有文本永远是覆盖项”：读到下一个 section 后作用域会改变。

| 推荐 section | 职责及简写 |
|---|---|
| `[meta]` | schema_version；extends 相对当前配置文件解析，不相对运行目录 |
| `[model]` | name、base_standard、preset |
| `[memory_system]` | stack_count、入口、QoS、响应容量 |
| `[frontend]` | pattern、requests、trace、seed、注入和地址范围 |
| `[controller]` | 读写队列、水位 |
| `[controller.scheduler]` | type → scheduler |
| `[controller.row_policy]` | type → row_policy；cap → row_policy_cap |
| `[controller.addr_mapper]` | address_mapping、channel_mapper |
| `[controller.refresh]` | 刷新、RFM、控制器低功耗 |
| `[phy]` | mode → mem_phy_mode；command_fifo_depth 等省略 phy_ 前缀 |
| `[storage]` | backend → memory_backend；capacity_bytes → memory_capacity_bytes；chunk_size/chunk_cache_entries |
| `[power]`、`[thermal]` | enabled/source/scale、enabled/ambient_c 等组件简写 |
| `[reliability]`、`[reliability.payload]`、`[fault_injection]` | 接口保护开销、payload ECC、注错 |
| `[standard.hbm3.organization]` 等 | 仅该标准的逻辑组织与主参数 |
| `[dram.timing]`、`[timing.<名称>]` | Timing 及本段 source/reference；名称不是自动获得可信度 |
| `[audit]`、`[validation]` | 来源标识、可信度模式与严格门槛 |
| `[outputs]` | summary/diagnostic、结果和各类轨迹路径 |
| `[override]` | 本次研究差异，使用完整 canonical key |

section、别名和重复键规则由 [document.cpp](../src/config/document.cpp) 定义。
不能假定所有配置字段都能通过同名 `--xxx` 传入；CLI 可用项以 `--help` 为准。

<a id="coupling"></a>

## 2. 可修改项与联动

### 2.1 三类输入

| 类别 | 例子 | 修改方式 |
|---|---|---|
| 主参数/独立策略 | 行列、BG、banks、data_rate、请求数、调度器、后端 | 修改需要研究的变量，其他条件固定 |
| 可推导字段 | speed_bin、density、tCK、默认 data_bus_bits | 通常省略；支持 auto 的项可写 auto；显式值会参加一致性检查 |
| 审计标签 | model name、timing_profile、vendor_profile、mode_profile、parameter_reference | 记录身份和依据，不会因换名称自动生成器件或算法 |

算法只能选择已有实现：scheduler 为 fcfs/frfcfs；row policy 为 open_page/closed_page/closed_cap；
backend 为 sparse/mmap_sparse/chunk_file；PHY 为 direct/behavioral。陌生值报错，不回退。
时序 source 不是装饰标签，会影响来源检查；不能任意将研究值改成 vendor 以绕过校准。

### 2.2 明确的推导关系

设 C 为每个 Stack/器件实例的几何容量（Byte），T 为单事务 payload（Byte）：

```text
C = channels × pseudo_channels × sids × ranks
    × bank_groups × banks_per_group × rows × columns × T
系统容量 = C × stack_count
HBM density_gb = C × 8 / 2^30 / stack_height
LPDDR density_gb = C × 8 / 2^30 / (channels × pseudo_channels × ranks)
speed_bin_mbps = data_rate_mbps
HBM tCK_ps = 4,000,000 / data_rate_mbps
LPDDR tCK_ps = 2,000,000 × WCK:CK / data_rate_mbps
tick_duration_ps = tCK_ps / tick_multiplier
默认 data_bus_bits = channels × pseudo_channels × 基准每 PC/SC 的 DQ 位宽
```

这里 Gb 的密度核对口径是 2^30 bit，GiB 是 2^30 Byte；columns 是事务列数，
已经乘了 T，就不要再次乘 prefetch 或 burst。HBM 容量乘积中的 SID 是逻辑寻址维度，
不是“一 SID 就是一 die”；stack_height 用于物理映射和密度分摊，不在 C 外重复乘入。
HBM 的 ranks=1 是中性软件维度，不表示标准定义了可自由堆叠的 Rank 层。
bank_groups/banks_per_group 属于逻辑 PC/SID/rank 范围，不是整 die 的 bank 总数。

`columns` 在所有配置中都表示事务槽，不因验证用途改变单位。HBM4 BL8 的 32 个槽
恰好对应 JESD270-4A Table 4 的 CA[4:0]，不能笼统说它“不是原始列地址数”。
HBM4 8Hi 的每 SID 2 BG × 2 SID = 每 PC 4 BG，与 Table 5 的 PC 口径一致。
4/8/12/16Hi→1/2/3/4 SID 是 HBM4 标准组织关系；12Hi 缺少 Ramulator 预设，
不等于缺少 JEDEC 依据。HBM3/LPDDR5 的本次核对来自外部源码，未取得对应 JEDEC 原文。

| 修改项 | 自动变化 | 仍需自己确认 |
|---|---|---|
| rows、columns、BG、bank、PC、SID、rank、channel | 几何容量、折算 density；channel/PC 可影响默认总位宽 | 地址映射、热位置、刷新分支；不自动补成厂商组织 |
| stack_count | 实例和控制器数量、总容量、理论总带宽 | 每实例文件、工作集分布、注入压力 |
| stack_height | HBM 在 SID 为 auto/未指定时先推导 SID，再计算容量、density 和物理 layer | 显式整数 SID 保持不变；非支持层数需显式研究组织 |
| data_rate_mbps | speed_bin、tCK、profile 中时间项展开 | 用户显式 nCK 不会自动保持原 ns；RL/WL 需目标模式表 |
| LPDDR5 lpddr_wck_ratio | CK 换算；允许 2 或 4 | 速率与模式有效性 |
| LPDDR6 lpddr_wck_ratio | 当前仅实现 2:1 | 其他比例会拒绝，不能视为自由连续参数 |
| LPDDR6 low DVFS | 选低速分支；data_rate 必须与 lpddr_low_data_rate_mbps 一致 | 模式恢复/训练约束 |
| nRAS 或 nRP | 未显式覆盖 nRC 时推导 nRC=nRAS+nRP | 显式 nRC 必须满足校验，不能小于和 |
| nRFC/nRFCpb | 非 LPDDR6 且未显式覆盖时，nRFMab/nRFMpb 跟随 | LPDDR6 RFM 时长独立，不能套相同规则 |
| memory_capacity_bytes | 设置后端地址上限；0 使用几何容量 | 不改 DRAM 几何；输入必须同时落在几何与后端范围内 |
| thermal 网格、chunk 缓存 | 热离散或宿主缓存成本 | 不改变 DRAM 逻辑容量 |

仅 speed_bin_mbps、data_rate_mbps、density_gb、tck_ps、data_bus_bits、sids 支持此处的 auto
输入机制。不要给任意 timing 或算法写 auto。`dram_transaction_bytes=0`、
DFI 中约定的 0 是各自的默认推导约定，不等于通用 auto。

显式 density/speed/tCK 与推导值冲突会报错；tCK 允许整数 ps 表的有限舍入容差。
显式 data_bus_bits 可作为独立研究位宽，不强制等于默认位宽公式，但仍受合法性校验。
HBM 的 SID 自动规则只覆盖本模型支持的 4/8/12/16Hi 组织；其他层数必须填写显式正整数
SID，不能凭取整获得“标准组织”。显式 SID 不随层数变化，也不表示厂商组织已认证。
例如 HBM4 主模板从 8Hi 改为 16Hi、保持 `sids=auto` 和其他几何不变，SID 2→4、
每 Stack 容量 32→64 GiB、折算每 die 密度仍为 32 Gibit；若同时显式 `sids=2`，
则容量仍为 32 GiB、密度变为 16 Gibit。刷新参数按现有 Timing 规则使用最终密度/层数。
新建模型省略 SID 可随指定层数推导；库对已有模型仅覆盖无关字段时保留现有 SID，
需要重新按当前层数选择时显式传入 `sids=auto`。配置快照导出具体整数以固定重放组织。
联动不是“所有看起来有关系的参数都会自动改”：实际逻辑见
[resolve_coupled_inputs / apply_coupled_model](../src/config/model.cpp)。

### 2.3 小容量自定义例子

在复制的 HBM 配置末尾已有的 `[override]` 段内写入（不要再创建同名重复段）：

```ini
rows = 1024
columns = 16
density_gb = auto
```

选择 HBM3 后，这会缩小逻辑容量并重算 density，不会固定打印原来的标称密度。
保持原 data_rate、事务宽度和调度策略，先执行第 1 节 check-config，再做小规模读写。
若副本或快照显式写过 density/tCK/speed，应删除对应冗余输入或改成允许的 auto；
有意固定不一致值不会被静默接受。

<a id="reference"></a>

## 3. 详细参数字典

默认数值以当前主配置叠加内置 profile 的解析结果为准；以下说明单位、作用域和可修改条件，
不再复制四套易漂移的默认数字表。参数标签的证据要求集中见[审计](审计.md)。

<a id="appendix-a-1"></a>

### 3.1 实验和输入

| 配置键/CLI | 含义 | 可否自定义/注意事项 |
|---|---|---|
| `standard` / `--standard` | hbm3/hbm4/lpddr5/lpddr6 | 可；会改变默认 traits/profile |
| `pattern` | stream/random | 可；trace 存在时由 trace 驱动 |
| `trace` / `trace_path` | trace 文件 | 可；相对路径以运行目录解析 |
| `requests` | host 请求上限；trace 中 0 为全部 | 可 |
| `read_ratio` | 合成读比例，百分数 | 可，0–100 |
| `seed` | 随机种子 | 可；可复现必须保存 |
| `random_address_space_bytes` | random 聚合地址空间上限；0 为模型总容量 | 可；须按 line_size 对齐且至少容纳一个 HostRequest，小型文件后端建议显式设置 |
| `addr_stride` | 合成地址步长，byte | 可；建议 transaction/line 对齐 |
| `inject_interval` | 前端注入间隔，controller tick | 可；性能曲线的主自变量 |
| `init_sequence` | none/auto/hbm/hbm3/hbm4/lpddr/lpddr5/lpddr6/lpddr6_full | 可；必须与标准族匹配；`lpddr6_full` 额外覆盖低功耗往返序列 |
| `init_sequence_interval` | 控制序列命令间隔 | 可 |
| `max_cycles` | 系统周期上限 | 可；命中上限不代表正常完成 |
| `progress_interval` | 每 N 个 system cycle 向 stderr 打印进度 | 0 关闭；不改变仿真结果 |
| `stats_view` | summary / diagnostic | summary 日常精简报告；diagnostic 按需查看内部字段 |

<a id="appendix-a-2"></a>

### 3.2 Profile、组织与时钟

| 配置键 | 含义 | 替换规则 |
|---|---|---|
| `timing_profile` | Timing 来源身份标签 | 自定义名称不生成时序；基准由协议与参数展开 |
| `vendor_profile` | 厂家/器件身份标签 | 名称不代表已校准；必须有真实参数和来源证据 |
| `mode_profile` | 模式审计名称 | 不按名称子串切换功能；显式设置保护、效率、DVFS 等字段 |
| `timing_override_source` | vendor/jedec/external_reference/research_default/derived | 按所在段绑定；顺序不影响同段来源，默认 research_default |
| `speed_bin_mbps` | Timing 速率选择，Mb/s/pin | schema 3 通常省略，跟随 data_rate_mbps；冲突时报错 |
| `density_gb` | Gibit；HBM 按平均每 die，LPDDR 按每子通道/rank | 所有版本均由几何推导，可为小数；显式值须与推导值一致 |
| `stack_height` | 物理 die 层数 | HBM 自动 SID 的主输入；影响物理映射和密度分摊，不在逻辑容量乘积外再次乘入 |
| `strict_timing_table` | 禁止未校准的必要默认值 | 目标器件参数齐备后应设为 true |
| `data_rate_mbps` | Mb/s/pin，外部每 pin 数据率 | 主输入；触发时钟和速度档联动，见第 2 节 |
| `data_bus_bits` | 每实例接口总位宽，bit | 省略时派生；显式研究位宽须符合模型合法性 |
| `prefetch_size` | 保留的内部预取/BL 相关身份字段 | 当前不自动推导 transaction、columns 或 nBL；不要把改它当作切换突发模式 |
| `line_size` | host line byte | 可；可拆 transaction |
| `dram_transaction_bytes` | 单 RD/WR payload byte | 可；必须满足 burst/总线组织 |
| `tck_ps` | 时钟周期 ps | 谨慎；应由速度档推导 |
| `tick_multiplier` | 模拟 tick 与 CK 比例 | 谨慎；影响所有 nCK 解释 |
| `channels` | 每 stack channel 数 | 可；需与 profile/容量一致 |
| `pseudo_channels` | 每 channel 的 PC/SC 数 | HBM 称 PC，LPDDR 称 subchannel |
| `sids` | 每 PC 下独立寻址的 SID 数量 | auto 时 HBM 4/8/12/16Hi→1/2/3/4；LPDDR→1；显式正整数固定研究组织 |
| `ranks` | 每 PC/SC/SID 下的软件 rank 维度 | HBM 通常为 1；非 1 是研究扩展，影响容量 |
| `bank_groups` | 每 PC/SC、SID、rank 下的 BG 数 | 标准相关；REFdb 要求可配对 |
| `banks_per_group` | 每 BG bank 数 | 标准相关 |
| `rows`、`columns` | 每 Bank 行数、每行事务槽数 | columns × transaction_bytes 是行容量；外部原始 column 需先换算 |
| `full_stack_model` | 使用完整 stack 组织口径 | 可；不等于 stack_count>1 |

<a id="appendix-a-3"></a>

### 3.3 Controller、映射和多 Stack

| 配置键 | 含义 | 可否自定义/注意事项 |
|---|---|---|
| `read_buffer_size`、`write_buffer_size` | 读写队列深度 | 可；影响反压/排队 |
| `priority_buffer_size` | 维护/控制队列深度 | 可 |
| `write_high_watermark`、`write_low_watermark` | 写模式切换水位 | 可；必须满足 `0 <= low < high <= 1` |
| `scheduler` | fcfs/frfcfs | 可 |
| `row_policy` | open_page/closed_page/closed_cap | 可 |
| `row_policy_cap` | closed_cap 列访问上限 | 可 |
| `addr_mapping` | default/RoBaRaCoCh/ChRaBaRoCo/RoCoRaBaCh | 可；改变局部性 |
| `channel_mapper` | decoded/round_robin/xor | 可；公平比较需固定 |
| `single_controller` | 单控制器独立实验 | 局部控制器对照实验 |
| `stack_count` | 独立 stack 数 | 可；主动模型真正复制端点和 image |
| `stack_mapping` | interleaved/blocked | 可 |
| `stack_interleave_bytes` | stack 条带大小 | 可；必须为 line_size 整数倍 |
| `stack_ingress_buffer_size` | 每 stack 系统入口深度 | 可 |
| `stack_dispatch_width` | 每 stack 每拍分发上限 | 可 |
| `stack_qos_policy` | fcfs/strict_priority | 可；高数字为高优先级 |
| `response_delivery_mode` | disabled/host/transaction/both | CLI trace 可自动推导；single_controller 不支持 |
| `host_response_queue_capacity` | HostResponse 队列容量 | 0 无限；满时反压 completion |
| `transaction_response_queue_capacity` | TransactionResponse 队列容量 | 0 无限；满时反压 completion |

<a id="appendix-a-4"></a>

### 3.4 PHY/DFI

| 配置键 | 含义 | 可否自定义/注意事项 |
|---|---|---|
| `mem_phy_mode` / `phy_mode` | direct/behavioral | 可；正式 full-stack 用 behavioral |
| `phy_protocol` / `[phy] protocol` | auto/hbm/lpddr | 必须与选定协议族匹配；不是自定义适配器名称 |
| `dfi_version` | DFI 目标标签 | 可；不自动获得合规能力 |
| `phy_command_fifo_depth` | 命令 FIFO 深度 | 可；与 command pipeline/吞吐联合解释 |
| `phy_write_fifo_depth` | 写数据 FIFO 深度 | 可 |
| `phy_read_fifo_depth` | 读返回 FIFO 深度 | 可 |
| `[phy] command_pipeline_cycles` | 命令流水延迟，nCK | 可；平面 canonical key 为 `phy_command_pipeline_cycles` |
| `[phy] write_data_pipeline_cycles` | 写数据流水延迟，nCK | 可；canonical key 为 `phy_write_data_pipeline_cycles` |
| `[phy] read_return_pipeline_cycles` | 读返回流水延迟，nCK | 可；canonical key 为 `phy_read_return_pipeline_cycles` |
| `[phy] reset_cycles` | reset 持续 nCK | 可，需校准；canonical key 为 `phy_reset_cycles` |
| `[phy] initialization_cycles` | init 持续 nCK | 可，需校准；canonical key 为 `phy_initialization_cycles` |
| `[phy] training_cycles` | training 持续 nCK | 可，需校准；canonical key 为 `phy_training_cycles` |
| `[phy] auto_train` | true 经过训练；false 跳过训练但仍经过复位和初始化 | 可；canonical key 为 `phy_auto_train` |
| `dfi_phase_count` | DFI beat phase 数 | 0 表示派生 |
| `dfi_data_lane_bytes` | 每 beat byte | 0 表示派生 |
| `dfi_read_latency_nck`、`dfi_write_latency_nck` | DFI 读写延迟 | 可，需 PHY/profile 依据 |

上表已经注明 canonical key；长短名称指向同一字段，配置中保持一种写法即可。

配置与默认 PHY 统一记录 `dfi_version=6.0.1`；这是来源标签，不是版本功能开关。
`dfi_phase_count` 仍用于事件 phase 的取模；已移除的适配器内部 `dfi_phases` 没有消费者，
不能将它的删除理解为取消相位配置。signal-like CSV 保留项目历史名称，
`dfi_cs_n/dfi_reset_n/dfi_wrdata_mask` 等不等同于规范的
`dfi_cs/dfi_reset/dfi_wrdata_dbi_mask` 端口；address/bank/cke/odt 也不是规范 CA packing。

<a id="appendix-a-5"></a>

### 3.5 Refresh、RFM、低功耗和协议模式

| 配置键 | 含义 | 说明 |
|---|---|---|
| `refresh_policy` | per_bank/all_bank | 项目枚举；LPDDR6 的 per_bank 实际选择 REFdb 双 Bank 刷新，标准没有 REFpb 命令 |
| `refresh_temperature_mode` / `--refresh-temperature` | normal/high/extended | 刷新温度模式 |
| `refresh_high_temp_multiplier` | 高温刷新频率倍率，正整数 | 越大则刷新间隔越短；与器件温度档核对 |
| `refresh_postpone_limit` | 最大延后 | 策略参数 |
| `refresh_pullin_limit` | 最大提前 | 策略参数 |
| `refresh_credit_limit` | refresh credit 上限 | 策略参数 |
| `rfm_policy` | per_bank/all_bank | RFM 作用域 |
| `rfm_act_threshold` | ACT 累计触发阈值 | 研究/器件参数，需校准 |
| `rfm_decrement` | RFM 后计数下降量 | 研究/器件参数，需校准 |
| `low_power_mode` / `--low-power` | off/power_down/self_refresh | 低功耗策略 |
| `low_power_entry_cycles` | 空闲进入阈值，nCK | 控制器策略，不等于向 PHY 发 PDE/SREFEN |
| `low_power_exit_cycles` | power-down 退出延迟，nCK | 需 profile 依据 |
| `self_refresh_exit_cycles` | self-refresh 退出延迟，nCK | 需 profile 依据 |
| `lpddr_efficiency_mode` | normal/static/dynamic | LPDDR 模式 |
| `lpddr_dvfs_mode` | nominal/low/disabled | LPDDR DVFS |
| `lpddr_wck_mode` | `cas_sync`/`always_on` | LPDDR WCK；`burst_sync` 仅保留枚举占位，解析器会拒绝，不能作为实验配置 |
| `hbm_link_crc_mode` / `--hbm-link-crc`、`hbm_link_crc_bits_per_request` / `--hbm-link-crc-bits` | HBM CRC 模式/开销 | HBM 专用 |
| `hbm_ras_metadata_bits_per_request` / `--hbm-ras-metadata-bits`、`hbm_ecc_bits_per_request` / `--hbm-ecc-bits` | HBM 接口 metadata | HBM 专用 |
| `hbm_link_retry_enabled` / `--hbm-link-retry` | HBM retry 入口 | 行为级 |
| `lpddr_link_protection` | LPDDR link protection | LPDDR 专用 |
| `lpddr_dbi_enabled` / `--lpddr-dbi`、`lpddr_dbi_bits_per_request` / `--lpddr-dbi-bits` | DBI 及开销 | LPDDR 专用 |
| `lpddr_link_ecc_enabled` / `--lpddr-link-ecc`、`lpddr_link_ecc_bits_per_request` / `--lpddr-link-ecc-bits` | link ECC | LPDDR 专用 |
| `lpddr_ca_parity_enabled` / `--lpddr-ca-parity`、`lpddr_ca_parity_bits_per_command` / `--lpddr-ca-parity-bits` | CA parity | LPDDR 专用 |
| `metadata_bits_per_request`、`ecc_bits_per_request` | 通用接口开销 | 影响 interface bandwidth |

能力开关和 profile 专用字段如下。它们可以自定义，但多数是标准/profile 身份的一部分，
用于对照实验时也必须记录，不能只记录 timing：

| 配置键 | 含义/允许口径 |
|---|---|
| `supports_refresh`、`supports_rfm`、`supports_ecc` | refresh、RFM/PRAC、ECC 能力总开关 |
| `hbm_full_32_channel_stack` | 是否采用 HBM4 32-channel full-stack 组织 |
| `hbm_sid_interleave`、`hbm_pc_interleave` | SID、pseudo-channel 是否参与地址 interleave |
| `hbm_edge_pairing`、`hbm_strict_edge_pairing` | 是否启用普通/严格 HBM 边沿配对 |
| `hbm_edge_pairing_matrix` | pairing 规则标签；真正约束由 Controller/validator 执行 |
| `hbm_sid_mapping` | SID 映射策略标签 |
| `hbm_ras_policy` | HBM RAS 行为策略标签 |
| `hbm_ecc_scheme` | HBM ECC 方案标签 |
| `lpddr_dual_bank_refresh` | 是否启用 LPDDR6 REFdb dual-bank refresh |
| `lpddr_wck_ratio` | WCK:CK 项目内部比率；须按目标标准模式校准 |
| `lpddr_wck_training_mode` | startup/dvfs/cas-sync 等训练策略标签 |
| `lpddr_wck_training_required` | 启动或 DVFS 后是否要求 WCK training |
| `lpddr_dvfs_transition_policy` | DVFS 转换许可策略标签 |
| `lpddr_low_data_rate_mbps` | 低速 DVFS 分支数据率 |
| `lpddr_low_power_state_policy` | 低功耗状态策略标签 |
| `lpddr_mode_register_profile` | mode register profile 标签 |
| `lpddr_link_protection_mode` | link protection 模式长名称 |

`address_mapping` 是 `addr_mapping` 的配置文件别名。
主配置或 `[override]` 中写裸 `note=` 会按未知键拒绝。自包含配置应使用
`parameter_reference`，并同时保留 `timing_override_source` 的机器可读来源标签。

<a id="appendix-a-6"></a>

### 3.6 Timing 参数

以下 canonical key 均可由 profile 或配置覆盖。这里的解释同时给出 JEDEC 风格含义和
项目当前真正使用它的方式；两者不完全相同时，以“项目实现”列为准。
首次修改请先看 [3.6.12：Timing 修改操作清单](#timing-editing)，再查下面的参数字典。

#### 3.6.1 参考依据与声明边界

标准文献范围及数值校准限制见[审计：参考材料](审计.md#references)。
以下“项目实现”说明实际建模语义，不能仅凭名称认定数值已经厂家校准。

#### 3.6.2 单位、换算和读取规则

- `n*` 字段均以 nCK 保存；Controller 内部等待 tick 数为 `nCK × tick_multiplier`；
- `tCK_ps` 是一个 CK 周期的皮秒数；`ns` 输入换算为
  `ceil(ns × 1000 / tCK_ps)`，`us` 输入换算为 `ceil(us × 1,000,000 / tCK_ps)`；
- “最短间隔”表示前一命令在 tick `t` 发出后，后一命令最早在 `t + nCK参数 × tick_multiplier` 发出；
- `nREFI/nREFIpb` 是周期性维护调度间隔，其他大多数参数是命令合法性/完成延迟；
- profile、配置和 CLI 全部覆盖结束后才执行 `finalize_spec()`；最终值必须通过
  `--dump-timing-table` 查看，不能只读某一份 profile；
- 来源应按依据标为 `jedec`、`vendor`、`external_reference`、`derived` 或 `research_default`。只要目标器件依赖项仍是
  research default，就不能声称器件级校准完成。

#### 3.6.3 时钟、Burst 和 CAS 延迟

| 参数 | JEDEC 风格含义 | 项目当前实现/公式 | 适用与校准影响 |
|---|---|---|---|
| `tCK_ps` | CK 周期，所有基于时间的约束换算基准 | ns/us 覆盖先按该值向上换算；调度再乘 `tick_multiplier` | 改速度档时由主输入推导它，再展开时间项；错误会成比例影响所有延迟和带宽时间基准 |
| `nBL` | burst 占用的 CK 数/列总线数据窗口 | 参与 `read_latency=nCL+nBL`、写恢复 `nCWL+nBL+nWR`、同类 RD/WR 间隔和 DFI beat 完成；不逐 beat 模拟 DQ 电气波形 | LPDDR6 BL24、WCK:CK=2 时为 6 CK，不再随低速档变为 4；不等于 BL 编码值 |
| `nCL` | Read Latency/CAS read latency：读命令到首批有效读数据 | 读完成简化为 `nCL+nBL`；LPDDR RD→WR 还使用 `nCL+nBL+2-nCWL` | speed-bin/mode/vendor 相关；增大通常提高读延迟并拉长读写换向 |
| `nCWL` | Write Latency/CAS write latency：写命令到写数据 | WR→PRE 使用 `nCWL+nBL+nWR`；WR→RD 使用 `nCWL+nBL+nWTRS`；PHY 默认写延迟也可由它派生 | speed-bin/mode/vendor 相关；影响写完成后可预充和总线换向 |

#### 3.6.4 激活、保持和预充

| 参数 | JEDEC 风格含义 | 项目当前实现/作用域 | 校准影响 |
|---|---|---|---|
| `nRCDRD` | ACT 到 RD 的最短 RAS-to-CAS delay | 同一 bank 的 ACT/ACT1→RD/RDA；LPDDR 从 ACT1 序列起点设置绝对 gate，ACT2 无论在 `nAADMin..nAADMax` 哪一拍发出都不会重置或扣减该 gate | 过小会允许尚未完成行激活就读；过大会增加 row miss 读延迟 |
| `nRCDWR` | ACT 到 WR 的最短 delay | 同一 bank 的 ACT/ACT1→WR/WRA；LPDDR 同样从 ACT1 起点设置绝对 gate | 主要影响 row miss 写延迟和写队列排空 |
| `nRP` | 单 bank PRE 到下一次 ACT 的 row precharge time；LPDDR6 文本对应 `tRPpb` | Bank scope PREpb→ACT/ACT1；bank 的 `next_act` gate 也由它更新 | 影响 row conflict 代价；不能用平均值替代目标密度/速度档表 |
| `nRPab` | all-bank PRE 到下一次行访问的恢复时间；JESD209-6 第 213 页明确 `tRPab > tRPpb` | PREab 后所有 bank 的 `next_act`；为 0 时回退到 `nRP` | 影响 all-bank refresh/模式切换前后的全局停顿 |
| `nRAS` | ACT 后该行必须保持激活的最短时间 | Bank scope ACT/ACT1→PREpb，阻止过早关闭行 | 必须与 `nRC/nRP` 一致；过小破坏行恢复，过大降低 closed-page 灵活性 |
| `nRC` | 同一 bank 两次 ACT 的最短 row cycle | Bank scope ACT/ACT1→下一次 ACT/ACT1；必须满足 `nRC >= nRAS+nRP`，未显式指定时可按依赖推导 | row cycle 的最小间隔；配置后应检查派生一致性 |
| `nRTP` | RD 到 PRE 的最短 read-to-precharge | Bank scope RD→PREpb；自动预充也受该 gate 控制 | 影响 RDA/closed-page 和读后快速换行 |
| `nWR` | 最后写数据后到 PRE 的 write recovery | 项目组合约束为 `nCWL+nBL+nWR`，不是从 WR 命令单独等待 `nWR` | 写路径关键参数；错误会造成写数据未恢复即预充或不必要停顿 |

#### 3.6.5 列命令间隔和读写换向

| 参数 | JEDEC 风格含义 | 项目当前实现/作用域 | 校准影响 |
|---|---|---|---|
| `nCCDS` | different bank group 的 short tCCD；JESD270-4A 表 6 用于跨 BG 的 WR→WR 和通常的 RD→RD | HBM Sid、LPDDR Rank scope 的 RD→RD、WR→WR 最短间隔 | 决定跨 BG 列并行度；数值越大，可持续列吞吐越低 |
| `nCCDL` | same bank group 的 long tCCD；JESD270-4A 表 6 用于同 BG 的 RD→RD/WR→WR | BankGroup scope 的 RD→RD、WR→WR | same-BG 压力场景的关键瓶颈，应与 `nCCDS` 分开验证 |
| `nCCDR` | HBM 跨 SID 连续读的特殊间隔；JESD270-4A 表 108 注 17 规定它在该场景替代 `tCCDS` | `TimingConstraint::sibling` 只对同一 PC 的 different SID 施加 `nCCDR`；same SID 不受该项约束，但两类都仍受 PC 的 `nBL` 总线占用约束 | 数值与频率为 vendor-specific；已有 same/different SID 边界测试锁定作用域 |
| `nRTW` | Read-to-Write bus turnaround | HBM PseudoChannel RD/RDA→WR/WRA 直接使用；LPDDR 使用 `nCL+nBL+2-nCWL` 的组合式 | 影响读写混合流量；纯读/纯写场景不敏感 |
| `nWTRS` | Write-to-Read short turnaround | HBM/LPDDR 当前主要组合为 `nCWL+nBL+nWTRS` | different-BG/短换向基线；增大会增加写切读空窗 |
| `nWTRL` | Write-to-Read long turnaround | HBM 和 LPDDR 均在 BankGroup scope 对 same-BG WR/WRA→RD/RDA 施加 `nCWL+nBL+nWTRL`；更松 scope 仍使用 `nWTRS` | same/different BG 必须分场景验证；不能用一个平均 WTR 值代替 |

#### 3.6.6 激活间隔和窗口

| 参数 | JEDEC 风格含义 | 项目当前实现/作用域 | 校准影响 |
|---|---|---|---|
| `nRRDS` | different BG 的短 ACT-to-ACT 间隔 | HBM PseudoChannel、LPDDR Rank scope 的连续 ACT/ACT1 | 控制跨 BG bank-level parallelism |
| `nRRDL` | same BG 的长 ACT-to-ACT 间隔 | BankGroup scope 的连续 ACT/ACT1；部分 RFM/REFdb fallback 也引用它 | same-BG 激活压力越高越敏感 |
| `nFAW` | Four Activate Window：滚动窗口内最多四个占用事件；HBM4 包含 ACT/PER BANK REFRESH，LPDDR6 包含 ACT/REFdb | TimingEngine 在 activation scope 记录 ACT/ACT1/REFpb/REFdb，第 5 个事件必须等最早事件离开窗口 | JESD270-4A 表 108 注 10 和 JESD209-6 第 212 页均明确“最多四个”；必须用窗口测试，不可化为普通相邻间隔 |
| `nAADMin` | ACT1→ACT2 最早可发边界 | Bank 进入 activating 后，ACT2 在 `ACT1+nAADMin` 前会被拒绝；当前默认 1 nCK 是项目命令执行粒度 | 这不冒充厂商引脚参数；如有更精确命令总线规则应据此替换 |
| `nAADMax` | LPDDR6 ACT1→ACT2 最晚 deadline；ACT2 须在 tAAD 窗口内发出 | ACT2 相对 ACT1 的偏移窗口为 `[nAADMin,nAADMax]` nCK，换成 tick 后包含两端点；deadline 到达时优先服务，超时显式报错；同一 rank 的下一个 ACT1 要等待 ACT2 | 别名 `nAAD`/`tAAD_ns` 映射到该最大值；Ramulator 差分也用该项对齐 |

#### 3.6.7 LPDDR WCK、CAS 和命令间隔

LPDDR6 BL24 的不同 BG 列间隔 `nCCDS=6`；同 BG 的 `nCCDL` 按 Table 382
的速率上界分档：≤6400→6、≤8533→8、≤10667→10、≤12800→12 CK。
超过 12800 Mb/s 保留 12 CK 研究回退并标为 research_default，不声称已有标准依据。
此规则不表示支持 BL48 全部模式，也不自动校准 RL/WL、WCK training 或 DVFS。

| 参数 | JEDEC 风格含义 | 项目当前实现 | 适用与边界 |
|---|---|---|---|
| `nWCK2CK` | WCK 与 CK 同步后到可使用 WCK/数据命令的等待 | CAS_RD/CAS_WR/WCK sync 后设置 `wck_ready_at`；always-on 模式通常为 0 | LPDDR5/6；影响按需 WCK 的首访问延迟 |
| `nWCKPST` | WCK postamble/同步有效窗口尾部 | 决定 `wck_active_until`，读写完成路径也把它叠加到 WCK 活跃窗口 | 当前是行为级窗口，不模拟真实波形 |
| `nCAS` | CAS_RD/CAS_WR 到实际 RD/WR 的间隔 | Bank scope CASRD→RD/RDA、CASWR→WR/WRA；并与 `nWCK2CK` 共同确定最早数据命令 | `cas_sync` 模式关键；`always_on` 可为 0；`burst_sync` 未实现且会 fail-fast |
| `nCS` | chip-select/命令选择相关最短间隔 | 当前保留在 TimingTable、配置和审计输出中，尚未形成独立 command constraint | 若目标研究需要 CS bus 精度，应增加对应 scope/命令对测试，不能仅改数值 |
| `nPPD` | precharge-to-precharge/相关命令最小间隔的标准字段 | HBM 在 PseudoChannel scope、LPDDR 在 Channel scope 对 PRE/PREpb/PREab 连续命令施加独立 gate；`nRP/nRPab` 仍负责 PRE→ACT | 已可执行；需与 PRE 命令变体、scope 边界一起校准 |
| `nWCKSYNC` | WCK sync 后到普通 RD/WR 的恢复 | PseudoChannel scope WCKSYNC→RD/WR | LPDDR 行为级同步检查，与 WCK ready window 双重约束 |
| `nWCKTRAIN` | WCK training 占用时间 | Channel scope WCKTRAIN→ACT1/CAS_RD/CAS_WR | 训练为行为级固定时延，真实训练算法/眼图不在模型内 |
| `nDVFS` | DVFS 转换完成时间 | Channel scope DVFS→ACT1/CAS/MRW | 改频后模式恢复/重训练入口；需要与 low/nominal profile 联合校准 |

#### 3.6.8 Refresh、dual-bank refresh 和 RFM

| 参数 | JEDEC 风格含义 | 项目当前实现/作用域 | 校准影响 |
|---|---|---|---|
| `nRFC` | all-bank refresh cycle time | HBM PseudoChannel 或 LPDDR Rank 的 REFab→ACT/PRE/REF 恢复；RFMab 为 0 时也可回退使用 | 密度/温度相关；决定 all-bank 维护停顿 |
| `nRFCpb` | HBM/LPDDR5 per-bank、LPDDR6 dual-bank 恢复时间 | REFdb 对两个目标 Bank 都约束 ACT1/后续 REFdb；RFMpb 为 0 时回退使用 | LPDDR6 字段沿用内部名称，不表示存在 REFpb 命令 |
| `nRFMab` | all-bank RFM 占用/恢复时间 | RFMab 后在 PseudoChannel/Rank scope 阻塞 ACT/PRE/RFMpb；为 0 时回退 `nRFC` | 仅启用 RFM 时有意义；阈值策略与时序应分开校准 |
| `nRFMpb` | per-bank RFM 占用/恢复时间 | Bank scope RFMpb→ACT；为 0 时回退 `nRFCpb` | 影响高 ACT 压力场景的维护损失 |
| `nRREFD` | refresh 到后续 ACT/refresh 的附加间隔 | HBM REFpb 在 PseudoChannel scope 到 ACT；LPDDR6 可作为 REFdb→ACT fallback | 不等同 `nRFCpb`；一个是共享 scope 间隔，一个是目标 bank 占用 |
| `nREFDB2ACT` | LPDDR6 dual-bank refresh 后到 ACT 的恢复 | PseudoChannel scope REFdb/RFMpb→ACT1；为 0 时依次回退 `nRREFD/nRRDS` | LPDDR6 专用，应从对应 REFdb 表获取 |
| `nREFDB2REFDBS` | 同一刷新行/计数器内、不同 Bank 对的间隔 | 每 Subchannel/SID/Rank 重放计数；本轮尚未覆盖全部 Bank 时，下一次 REFdb 用 S | JESD209-6 §7.6.1/7.6.2；一轮完成前不得重复 Bank |
| `nREFDB2REFDBL` | 不同刷新行/计数器之间的间隔 | 完成 Bank 数/2 次 REFdb 后，下一次用 L；REFab、自刷新退出同步计数 | 标准 16 Bank 为 8 次一轮；其他几何是研究推广，不再无条件施加 L |
| `nREFI` | all-bank refresh 的平均调度周期 | RefreshManager 在 all-bank policy 下生成 refresh 到期点，并受温度倍率影响 | 它不是 refresh 占用时长；减半会提高维护频率 |
| `nREFIpb` | per-bank/轮转 refresh 的调度周期 | per-bank policy 优先使用；为 0 时回退 `nREFI` | 与 bank rotation、stack height、温度模式相关 |

#### 3.6.9 Mode、低功耗、ECC 和 RAS

REFdb 当前仅支持相同 BA、相邻 BG（0↔1、2↔3）的固定配对，不支持任意 dBG 编码。
显式维护 trace 也应完整轮转或用 REFab/自刷新同步；单纯等待不能使同一轮重复 Bank 合法。
计数在命令实际发出时推进，而不是在维护请求入队时推进；在线与离线分别维护状态。

| 参数 | JEDEC 风格含义 | 项目当前实现/作用域 | 校准边界 |
|---|---|---|---|
| `nMRW` | Mode Register Write 后的命令恢复 | Channel scope MRW→ACT/RD/WR/MRW/MRR 或 LPDDR CAS 命令 | 模式编程序列固定时延；具体 MR 内容不由该值表达 |
| `nMRR` | Mode Register Read 后的命令恢复 | Channel scope MRR→普通命令/MR 命令 | 读取返回数据的 pin 行为不在当前模型内 |
| `nPDEX` | power-down exit 到普通命令可用时间 | Channel scope PDE/PDX→ACT1/CAS；executor 在 PDX 后设置 row gate | Behavioral PHY 还要求显式 PDX；不能只改 Controller 空闲阈值 |
| `nSREFEX` | self-refresh exit 到普通访问/维护可用时间 | Channel scope SREFEN/SREFEX→ACT1/REFdb/RFMpb；通常不小于 `nPDEX` | 温度/器件相关，影响深度低功耗恢复 |
| `nECCSCRUB` | ECC scrub/repair 行为占用时间 | ECCSCRUB/RASERR 后与 `nRASERR` 取最大值，阻塞 Channel 内 ACT/refresh/RFM/MR | 厂商/RAS policy 相关；当前是行为级固定时间 |
| `nRASERR` | RAS error handling/recovery 占用时间 | 与 ECCSCRUB 共同形成 Channel control gate；启用 retry 时还关联读写恢复 | 不是错误发生概率，只是恢复路径时延 |
| `nLINKRETRY` | link retry/replay 后到普通数据命令恢复 | HBM RASERR→RD/WR 使用；配置 link protection/retry 时有效 | 厂商链路和训练相关；当前不模拟实际 flit 重放次数 |

#### 3.6.10 参数之间的组合关系

项目实际检查的不只是单个字段，还包括组合式：

```text
读完成                  = nCL + nBL
WR → PRE               = nCWL + nBL + nWR
WR → RD                = nCWL + nBL + nWTRS
LPDDR RD → WR          = nCL + nBL + 2 - nCWL
HBM same-SID RD → RD   = max(nBL, nCCDS, same-BG ? nCCDL : 0)
HBM different-SID RD → RD = max(nBL, nCCDR)
ECC/RAS control gate   = max(nECCSCRUB, nRASERR)
LPDDR ACT1 → ACT2     = [nAADMin, nAADMax]
LPDDR ACT1 → RD/WR    = nRCDRD / nRCDWR（绝对 gate）
```

以上主要是运行时等待/完成条件，不是配置字段的自动赋值表。例如改 nCWL 会改变
`nCWL+nBL+nWR` 的结果，但不会因此自动改写 nWR。配置阶段的补推规则见 3.6.12；
不能把 nRPab 当成必然跟随 nRP 的派生项。参数敏感性实验需同时记录发生变化的组合约束。

#### 3.6.11 单位别名和逐项替换方法

当前解析器支持的常用别名包括：`tCK_ps`、`tRCD_RD_ns`、`tRCD_WR_ns`、`tRP_ns`、
`tRPab_ns`、`tRAS_ns`、`tRC_ns`、`tRTP_ns`、`tWTP_ns/tWR_ns`、`tCCDS_ns`、
`tCCDL_ns`、`tRRD_S_ns`、`tRRD_L_ns`、`tFAW_ns`、`tWTR_S_ns`、`tWTR_L_ns`、
`tRTW_ns`、`tRFCab_ns`、`tRFCpb_ns`、`tRFMab_ns`、`tRFMpb_ns`、`tRREFD_ns`、
`tREFI_us`、`tREFIpb_ns/us`、`tREFIdb_ns`、`tREFDB2ACT_ns`、
`tREFDB2REFDB_S_ns`、`tREFDB2REFDB_L_ns`、`tWCKTRAIN_ns` 和 `tDVFS_ns`。

替换目标器件 Timing 的推荐流程：

1. 先确定 speed-bin、`tCK_ps`、density、stack height 和模式；
2. 复制对应家族的完整 `configs/hbm.cfg` 或 `configs/lpddr.cfg`，在 `[model]`
   选择基准标准，在 `[override]` 逐项记录目标型号差异，用 `timing_override_source`
   和 `parameter_reference` 记录来源；
3. 先填 JEDEC 固定项和公式项，再填 vendor-dependent RL/WL/row/refresh/RAS；
4. 同一参数只使用 nCK 或一个时间单位版本，避免后写字段无意覆盖；
5. 运行 `--dump-timing-table` 检查值、来源、required/provisional；
6. 对每个被 Controller 使用的核心约束执行 `t-1` 禁止、`t` 允许；
7. 运行 same/different BG、读写换向、tFAW、refresh/RFM 和低功耗序列；
8. 最后才用 `--strict-timing-table` 作为器件数值配置门槛。

<a id="timing-editing"></a>

#### 3.6.12 Timing 修改操作清单

本节描述当前实现的使用契约，不是尚未实现的自动推导方案。普通用户不需要填写整张
Timing 表：复制主模板，先选协议、组织、速率和模式，再覆盖本次实验确实要改变的项。
HX 等研究用例已有大量显式 Timing，不能把它们当作“只改速率、其余均自动匹配”的模板。

**先分清四件事**

- 基准展开：未指定的 Timing 来自所选协议及其工作点；可能是表值、公式值或研究默认值，
  不保证每个速率/组织组合都有对应器件资料。
- 单位换算：`nCL=36` 固定 CK 数，`tRCD_RD_ns=15` 固定时间；改变 CK 后，前者实际时间改变，
  后者重新换算周期。二者不是相同含义的“锁定”。
- 配置补推：仅对实现明确支持、且没有显式输入的目标字段执行，例如修改 nRAS/nRP 后补推 nRC。
- 运行时组合：控制器使用最终参数计算等待，例如 `nCWL+nBL+nWR`；不是自动改写三个输入。

`[dram.timing] source=research_default` 只声明本段自定义 Timing 的来源，不开启自动推导，
也不会把所有未填写的基准项统一改成该来源。派生值若依赖研究值，仍可能标为
research_default，而不是仅凭 source 是否为 derived 判断有没有经过计算。

**按修改目标选择输入**

下表的“显式”包含所选基准段、preset、覆盖段和 CLI 的有效输入，不限于正在编辑的这一段。

| 要修改的内容 | 建议填写 | 当前自动处理 | 不会自动替你完成的事 |
|---|---|---|---|
| 速率、组织、SID、模式 | 修改相应主输入，省略不需要固定的 Timing | 重选/展开未覆盖的基准 Timing，换算时间项；容量和密度按最终组织计算 | 不生成缺失的厂商 RL/WL；已有显式 nCK 不会自动保持原 ns |
| 读写延迟与 burst 占用 | nCL、nCWL、nBL 中需要研究的项 | 运行时读完成、写恢复、换向组合使用最终值 | 不据此自动重填 nWR、nWTRS、nRTW；nBL 也不是任意的 Host burst 字节数 |
| 行激活/保持/预充 | nRCDRD、nRCDWR、nRAS、nRP，或对应时间别名 | 显式修改 nRAS 或 nRP，且 nRC 未显式指定时，补推 nRC=nRAS+nRP | 改 RCD 不会自动推导 RAS；nRPab 不会因覆盖 nRP 而自动同步 |
| 独立 row-cycle / all-bank PRE | nRC、nRPab 或各自时间别名 | 保留显式值；当前校验要求 nRC≥nRAS+nRP | 不会把较大的合法 nRC 强改成等式，也不推断 nRPab 的器件值 |
| 列间隔、换向、激活窗口 | nCCDS/nCCDL/nCCDR、nWTRS/nWTRL/nRTW、nRRDS/nRRDL/nFAW | 各命令 scope 和组合约束使用最终值 | 改短间隔不会自动改长间隔；不根据 nRRD 自动构造 nFAW |
| 刷新恢复 | nRFC/nRFCpb 或 tRFCab_ns/tRFCpb_ns | 非 LPDDR6：显式改 RFC 且对应 RFM 未显式指定时，按项目默认跟随；LPDDR6 RFM 保持独立 | 改 RFC 不会自动重算刷新间隔 REFI；RFC=RFM 不是通用标准定律 |
| 刷新周期、轮转间隔 | nREFI/nREFIpb 或相应时间别名 | 使用最终输入进行维护调度 | 显式改 REFI 后不会自动按比例改 REFIpb；需结合实际刷新策略核对 |
| ACT 两阶段窗口 | nAADMin、nAADMax | split-ACT 模式检查最早/最晚窗口，拒绝倒置窗口 | 不会将最晚 deadline 当成最小等待；不会据此自动修改 RCD |
| WCK、DVFS、低功耗、MR、RAS 等 | 字典中对应的独立 Timing 项 | 对已实现并启用的命令/模式使用这些延迟 | 不推导真实训练结果，也不保证仅修改数值就启用了该功能 |

DFI 延迟和 PHY pipeline 属于 3.4 的另一组配置。它们的默认/0 值语义不能套到普通
Timing：`nRC=auto`、`nCL=auto` 当前均不支持。要让未指定的 Timing 使用基准或补推，
应删除/注释其所有有效显式输入，而不是填 0。支持 auto 的字段清单见 2.2。

**修改位置与冲突处理**

1. 普通模型在已有 `[dram.timing]` 内修改；集中实验差异也可写在已有 `[override]`。
   不要重复新建同名 section。前者的 source 不代替后者的来源声明；后者用
   `timing_override_source=research_default` 说明其 Timing 来源。
2. 检查后面的覆盖段、所选 preset 和命令行是否又给了同一项。相同字段的优先级覆盖，
   与“同一 Timing 同时用 nCK 和 ns 输入”不同：后者会因单位歧义被拒绝，不能靠书写顺序解决。
3. nRC 等目标字段只要仍有显式值，就不触发省略式补推。尤其不要从 resolved 快照开始
   猜哪些值会自动变化：快照已经写出了具体值，适合重放，不适合保持精简联动输入。
4. 校验失败时先看是哪条输入或约束冲突，不要通过修改 source 标签绕过。
   校验通过也不等于所有物理关系均已覆盖，更不等于器件校准通过。

**两个可操作的修改例子**

例一：复制 HBM 主模板后，在已有 `[dram.timing]` 中设置以下研究值，确认其他激活段
没有 nRC/tRC_ns 覆盖：

```ini
[dram.timing]
source = research_default
nRAS = 80
nRP = 40
# 省略 nRC：当前实现得到 120 nCK。
# 若另有显式 nRC=125，则保留 125；若为 100，则校验失败。
```

这不是建议真实器件采用 80/40，而是演示输入契约。nRPab 不会因为这个例子自动变成 40；
实验涉及 all-bank PRE 时应单独检查它。

例二：固定 ACT 到 RD 的时间，在同一段使用下面的时间输入，不再写 nRCDRD：

```ini
tRCD_RD_ns = 15
```

以 HBM4 主输入为例，8000 Mb/s 时 CK=500 ps，得到 nRCDRD=30；9000 Mb/s 时
CK≈444.444 ps，得到 nRCDRD=34。这里仅说明时间换算，不证明 15 ns 对两种器件都适用。

**修改后怎么确认**

假定副本已保存为 `experiments/local/my_model.cfg`，在仓库根目录执行：

```bash
mkdir -p outputs/timing_check
./build-clang-debug/hbm_sim --config experiments/local/my_model.cfg \
  --check-config --dump-resolved-config outputs/timing_check/resolved.cfg
./build-clang-debug/hbm_sim --config experiments/local/my_model.cfg \
  --requests 128 --validate-cmd-trace --validate-dfi-trace \
  --dump-timing-table outputs/timing_check/timing.csv \
  --stats-json outputs/timing_check/result.json
```

第一条只做参数检查和快照导出；当前 `--check-config` 会提前返回，不能靠给它附加
`--dump-timing-table` 生成 Timing CSV。第二条小规模运行才生成该表及检查结果。
查看 resolved 中最终值/派生说明、timing.csv 中的 nCK/来源/依据，再检查完成状态与
命令/DFI 结果。小规模通过后再做目标场景；它不替代数据期望值验证或全部 Timing 边界测试。

<a id="appendix-a-7"></a>

### 3.7 存储、物理、功耗、热和 ECC

| 配置键 | 含义 | 说明 |
|---|---|---|
| `memory_backend` | sparse/mmap_sparse/chunk_file | 可替换后端 |
| `memory_capacity_bytes` | 后端逻辑容量 | 0 可按 spec 派生；文件后端建议显式 |
| `memory_data_file` | payload 文件 | 多 stack 支持 `{stack}` |
| `memory_init_file` | init bitmap 文件 | 可自定义路径 |
| `memory_meta_file` | metadata 文件 | 可自定义路径 |
| `memory_presence_file` | presence bitmap | 可自定义路径 |
| `memory_chunk_size` | chunk_file 块大小 | 性能参数，不改语义 |
| `memory_chunk_cache_entries` | chunk cache 数 | 控制宿主 RAM/I/O |
| `sparse_density_warning_pct` | 稀疏后端密度告警 | 只告警 |
| `topology_stats_scan_limit` | 详细坐标扫描上限 | 0 为无限制 |
| `floorplan` | 物理研究放置开关 | 关闭时 tile_x/y 为 0；不是热模型总开关 |
| `subarrays_per_bank` | 每 bank subarray 数 | 器件参数 |
| `mats_per_subarray_x/y` | mat 网格 | 器件参数 |
| `cells_per_mat_x/y` | cell 网格 | 器件参数 |
| `microbumps_x/y` | microbump 网格 | 封装参数 |
| `power_model` | 命令能量开关 | 可 |
| `power_source` | configured_pj/dramsim3_idd | 选择公式来源 |
| `power_scale` | 全部命令能量缩放 | 敏感性/校准参数 |
| `power_act_pj`、`power_act1_pj`、`power_act2_pj` | ACT/ACT1/ACT2 单事件成本，pJ | 非负，按协议启用对应事件 |
| `power_pre_pj`、`power_preab_pj`、`power_cas_pj` | PRE/PREab/CAS 单事件成本，pJ | 不要将单事件参数当累计能量 |
| `power_read_pj`、`power_write_pj` | 读/写固定事件成本，pJ | 与 per_byte 项共同记账 |
| `power_read_per_byte_pj`、`power_write_per_byte_pj` | 读/写每 Byte 成本，pJ/Byte | 按 payload 字节数缩放 |
| `power_refpb_pj`、`power_refdb_pj`、`power_refab_pj` | 各刷新事件成本，pJ | 对应维护命令范围 |
| `power_rfmpb_pj`、`power_rfmab_pj`、`power_control_pj` | RFM/其他控制事件成本，pJ | 研究输入，需校准 |
| `power_vdd`、`idd0`、`idd2n`、`idd3n`、`idd4r`、`idd4w`、`idd5ab`、`idd5pb`、`idd6x` | 电压/IDD 输入 | 必须来自目标器件才有绝对意义 |
| `idd_devices_per_rank` | 每 rank 器件数 | 与组织匹配 |
| `idd_burst_cycles` | IDD burst 公式的持续周期 | DRAMsim3 辅助验证/校准参数 |
| `thermal_model` | 行为级稀疏耦合热模型开关 | 可；不是完整 RC/有限元求解器 |
| `thermal_ambient_c` | 环境温度 | 边界条件 |
| `thermal_rise_c_per_pj` | 能量到温升比例 | 必须校准 |
| `thermal_cooling_per_cycle` | 每 controller tick 冷却比例 | 必须校准 |
| `thermal_grid_cols_per_tile`、`thermal_grid_rows_per_tile` | tile 内热网格 | 仅改变热离散 |
| `thermal_coupling` | 邻域耦合 | 可 |
| `thermal_lateral_coupling` | 横向耦合系数 | 校准参数 |
| `thermal_vertical_coupling` | 纵向耦合系数 | 校准参数 |
| `thermal_tsv_coupling_scale` | TSV 纵向缩放 | 校准参数 |
| `thermal_tsvs_per_grid` | 每 grid TSV 数 | 封装参数 |
| `thermal_chip_dim_x_m`、`thermal_chip_dim_y_m` | 芯片平面尺寸（米） | 热网格几何输入，须来自 floorplan/封装 |
| `thermal_tsv_radius_m` | TSV 半径，米 | 影响研究耦合的 TSV 面积比 |
| `thermal_k_silicon`、`thermal_k_copper` | 材料导热系数，W/(m·K) | 用于简化耦合，不代表已求解完整材料热传导 |
| `ecc_shadow` | SECDED shadow 开关 | 数据正确性模型 |
| `ecc_check_on_read` | 读时检查 | 可 |
| `ecc_correct_single_bit` | 单比特纠正 | 可 |
| `ecc_inject_period` | 周期性注错间隔 | 0 为关闭 |

<a id="appendix-a-8"></a>

### 3.8 输出和验证参数

| 配置键/CLI | 含义 |
|---|---|
| `stats_json` | 机器结果 JSON 路径；summary 也可以导出 |
| `dump_resolved_config` | 生效配置快照，包含派生值和来源 |
| `dump_config_diff` | 配置覆盖差异导出路径；不是终端内置基准变化清单 |
| `explain_config` | 需要解释覆盖链的配置键 |
| `cmd_trace` | command CSV 路径 |
| `response_trace` | HostResponse CSV 路径 |
| `transaction_response_trace` | DRAM 子事务响应 CSV 路径 |
| `dfi_trace` | DFI beat CSV 路径 |
| `dfi_signal_trace` | DFI-like signal CSV 路径 |
| `dump_timing_table` | 最终 timing CSV |
| `memory_image` | 初始 image |
| `dump_memory_image` | 最终镜像按扩展名选择格式：`.csv` 为明细表，`.bin` 为可重载二进制 checkpoint，其他扩展名为文本镜像；与 `memory_data_file` 指定的持久后端文件不是同一种格式 |
| `dump_memory_csv` | 最终 CSV image |
| `verify_golden` | 仿真后 golden 校验 |
| `mismatch_report` | 不匹配报告 |
| `dump_thermal_map` | 热 map 输出 |
| `validate_cmd_trace` | 运行 command validator |
| `validate_dfi_trace` | 运行 DFI validator |
| `fail_on_data_mismatch` | mismatch 时退出 3 |
| `--allow-data-mismatch`（仅 CLI） | mismatch 只记录不失败；配置文件中应写 `fail_on_data_mismatch=false` |

### 3.9 身份与配置检查工具

| 输入 | 含义与边界 |
|---|---|
| `[meta] schema_version` | 配置语法版本；复制主模板保留 3，不等于 DRAM 协议版本 |
| `[meta] extends` | 可选单个父配置路径，相对当前文件；自包含模板通常不需要 |
| `[model] name / base_standard / preset` | 模型名、基础标准、已有 preset 选择；名称不创造算法或时序 |
| `parameter_reference` / `[audit] reference` | 本次参数依据的可读记录；Timing 段 reference 绑定到该段 |
| `validation_mode` / `[validation] mode` | exploratory/standard/device；自由研究采用 exploratory |
| `--check-config`（CLI） | 只做解析、构造和一致性检查，不运行负载 |
| `--compare-preset`（CLI） | 输出相对配置基线的覆盖差异，与结果 changes 的内置基准比较不同 |

standard/device 模式拒绝相对所选 preset 的模型覆盖差异；standard 还要求必要 Timing
没有 research_default/external_reference，device 要求 provisional 项已处理、非 generic
vendor_profile 和 vendor 来源数值。它们只实施程序门槛，不验证资料真实性；证据要求见审计。

查看单项覆盖链时，可在第 1 节检查命令中附加 `--explain-config nCL`；对比选定 preset
则附加 `--compare-preset`。别把最终 resolved 快照、配置覆盖差异、结果 changes 当成同一种文件。
