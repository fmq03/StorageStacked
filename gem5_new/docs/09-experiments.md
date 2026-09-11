# 实验设计、结果分析与优化方向

实验必须同时回答“功能是否正确”“trace 是否可信”“固定输入下存储行为如何变化”。
[验证报告](05-validation-report.md)保存日期、版本与结果；本页给出复现方法、计算口径、
本轮对照分析和下一步实验设计。

## 1. 实验分层与问题

| 实验 | 输入与目的 | 能验证的结论 |
|---|---|---|
| E0 基础回归 | writer、工具、地址图、RTL boundary | 格式、字段、反压、生成物和工具因果检查 |
| E1 双设备功能 | Host↔NPU 正向/断开共享；Host↔Vortex vecadd | 真实字节、runtime/PIO/DMA 路径，错误能被发现 |
| E2 真实三源 | 同一 gem5 中 4 元素 vecadd + 64 元素 NPU 变换 | 三源分类、两条共享交接、活动区间重叠、投影守恒 |
| E3 存储完整性 | E2 trace→hbm_sim→HostResponse | 每请求 ID/地址/类型/时间一致、全部完成 |
| E4 参数对照 | 固定 LLM-like trace，改变一项 controller/输入时间参数 | 固定请求流下的服务、排队、转发与完成行为 |

E2 使用真实 CPU/设备功能执行；E4 是生成器构造的访存形状。两组不能混写为同一应用实验。
每组记录项目 revision/本地补丁、四棵依赖、编译工具、workload 规模、gem5 配置、trace
文件清单、程序路径、实际存储配置、`ticks_per_cycle`、截止周期与测量区间。

## 2. E2/E3 历史基线的数据解释

2026-09-05 私有 gem5 fork 基线产生 79,699 条 AXI 记录、19,768 笔事务和 50,487 个 W/R
数据拍，覆盖 731,880 B。明细与 HBM4 重放计数见[验证报告](05-validation-report.md)。
这些是不同计数口径：AW/AR/B 不是额外数据请求，50,487 才是转换后的 HostRequest 数。

| 来源 | 数据拍 | 字节 | 字节占比（据历史表计算） | 分析含义 |
|---|---:|---:|---:|---|
| Host | 49,478 | 715,984 | 97.83% | 初始化、动态运行库、线程与数据搬运占主导 |
| Vortex | 881 | 13,848 | 1.89% | core 与 CP DMA 都被观察，需分开分析计算与搬运 |
| CoralNPU | 128 | 2,048 | 0.28% | 64 次读、64 次写；当前 16 B 单拍子集 |

因此这份小功能 workload 不适合评价三设备带宽公平性：Host 初始化流量远大于设备计算流量。
NPU 实际操作 64 个 4 B 输入/输出，但 AXI seam 的拍跨度是 16 B；meta 的 `bytes` 累加拍
`size`，不等于有用的算法字节，也不等于 WSTRB 使能字节。写有效载荷另算
`sum(popcount(WSTRB))`，读拍没有 RSTRB，不能凭空推回全部有效 lane。

完整重放 50,487/50,487 请求完成、剩余为零，证明输入到响应可审计；2,476,809 system cycles
与平均读延迟 166.99 cycles 是该固定输入的存储模型统计。初始化间隔、输入时间跨度、排队和
结束排空都影响 system cycles；不得将其当作 GPU/NPU 推理运行时间。

09-08 使用官方 gem5 与原设备库复核，得到相同计数与时序；但逐响应状态审计发现
39,998 个 `uninitialized_data`、10,489 个 `ok`。初始 ELF/页内容的 functional 预置不在
memory-side timing trace 中，离线模型缺少初始镜像；`data_mismatches=0` 不等于全 status=ok。
因此真实 trace 的命令显式使用 `--allow-uninitialized`，只认证请求/时序完整性，并记录状态
分布。此豁免不适用于应包含全部初始化写的合成 LLM smoke，其他错误状态也不能豁免。

同日进一步重新构建四个外部组件后，完整三源为 79,711 条记录、50,495 个请求、732,008 B；
Host 比旧设备库组合增加 8 个数据拍，Vortex/NPU 计数不变。该组重放 system cycles 为
2,479,105，平均读延迟 168.80，全部请求完成，未初始化状态仍为 39,998 个。完整构建证据与
分源表见验证报告 6.4；本节历史表保留原输入口径，不用最新数据覆盖旧表，也不把跨构建差异
当成单参数性能对照。下面的 E4 则始终使用同一份合成输入。

## 3. E4 的可复制命令

先按[构建指南](08-build-run.md)准备 hbm_sim。生成一次三源 trace，四组共用它：

```bash
source scripts/native_env.sh
export HET_EXP_ROOT="$HET_PROJECT_ROOT/build/memory-sweep-$(date +%Y%m%d-%H%M%S)"
python3 workloads/llm_memory/generate_trace.py \
  --output "$HET_EXP_ROOT/input" \
  --hidden-size 16 --layers 1 --context-tokens 8 --decode-tokens 2 \
  --ffn-multiplier 2 --request-bytes 16

python3 scripts/replay_trace.py "$HET_EXP_ROOT/input" --out "$HET_EXP_ROOT/baseline"
python3 scripts/replay_trace.py "$HET_EXP_ROOT/input" --out "$HET_EXP_ROOT/fcfs" \
  --scheduler fcfs
python3 scripts/replay_trace.py "$HET_EXP_ROOT/input" --out "$HET_EXP_ROOT/closed" \
  --row-policy closed_page
python3 scripts/replay_trace.py "$HET_EXP_ROOT/input" --out "$HET_EXP_ROOT/dense" \
  --ticks-per-cycle 4000
```

默认组为 HBM4、`configs/hbm.cfg`、FRFCFS、open_page、1000 tick/cycle、最大 100,000,000
cycles。第四组单独压缩注入时间，1000→4000 是 `tick // ticks_per_cycle` 的除数变大，
所以请求在更少的模型周期中到达；它不是把真实芯片时钟提速四倍。

四组都是 596 请求（390 读 / 206 写）、9,536 B。分源为 Host 196/3,136 B、Vortex 76/1,216 B、
CoralNPU 324/5,184 B。生成器按每层 `(4H²+3HI)*weight_bytes` 构造权重访问，KV 随上下文与
decode 增长；不执行 Q/K/V 矩阵乘法的数值计算。

## 4. 2026-09-08 对照结果

可机读的实测数字、实验参数及输出路径保存于
[20260908-memory-sweep.json](results/20260908-memory-sweep.json)。

以下使用从固定离线源码包**重新编译**的 hbm_sim。所有组 ID/地址/类型/status/时间与逐源
请求/字节校验通过，`remaining_requests=remaining_pending=0`，`hit_cycle_limit=false`。
延迟单位为 hbm_sim cycle，p95 采用 nearest-rank。

| 组别 | system cycles | 全部响应 mean | p95 | 提交等待 mean / max | forwarded 响应 |
|---|---:|---:|---:|---:|---:|
| baseline | 2,023 | 270.20 | 1,465 | 0 / 0 | 5 |
| FCFS | 2,023 | 270.97 | 1,465 | 0 / 0 | 5 |
| closed_page | 3,141 | 585.52 | 2,133 | 0 / 0 | 70 |
| dense（4000 tick/cycle） | 1,985 | 113.62 | 274 | 20.27 / 50 | 170 |

四组使用同一输入目录 `build/repro-20260908/experiment-input/`，生成一次后不再改动。
每组 `run.json` 记录实际输入文件路径；未来修改生成器后应重新生成全部对照。模型解析后的参数
保存在 `resolved.cfg`；同一组的原始响应、summary 和命令分别在 CSV、Markdown 和 run.json。

### 4.1 调度器差异很小

FCFS 相比 baseline 平均延迟只增约 0.29%，末周期和 p95 不变。这说明当前小输入下调度器
切换不是总完成时间的主导因素，不能据此概括“FRFCFS 总是更快”或“两种调度完全等价”。
下一步应增加同 bank 的行冲突、混合读写和持续 outstanding，并观察 row hit/miss/conflict
及命令队列，而不是只扩大文档表格中的样本数。

### 4.2 行策略改变总体开销，也改变来源分布

closed_page 的 system cycles 增约 55.26%，mean 增约 116.70%，p95 增约 45.60%。
减少行保持可能导致更多 PRE/ACT，这是与实现相符的机制解释；本轮 summary 未逐命令证明
增加了多少 PRE/ACT，不能将推断写成已测的命令计数。

平均数还掩盖了来源变化：Vortex mean 从 1345.25 降至 204.76，而 CoralNPU 从 120.69 升至
721.98。response CSV 显示 Vortex forwarded 读从 3 次增至 68 次，说明写缓冲命中的返回路径
发生显著变化。不能把 Vortex 的低延迟单独解释成对 GPU 有利的 DRAM 行策略。

### 4.3 更密的输入为何响应 mean 下降

dense 的请求计划区间从 `[1,539]` 压缩为 `[0,134]` cycles，实际接受区间为 `[0,174]`，
出现 20.27 cycles 平均提交等待。同时 forwarded 从 5 增至 170，其中 Vortex 68、CoralNPU
100、Host 2。固定 mem_sim 实现会让命中待写缓冲的读走 forwarding，形成一周期等短延迟。
这是 CSV 与 `src/controller/controller.cpp` 能直接交叉检查的机制。

因此 mean 下降既涉及返回路径比例，又涉及到达形状与排队；单一平均值无法表示 DRAM 阵列
速度。dense 的“计划到完成”mean 为 `113.62+20.27=133.89`，必须与提交后 response mean 分列。
进一步对照应分别统计 forwarded 与未转发读、分源、读写、地址区域、请求年龄和 tail latency。

### 4.4 末次响应不一定等于模拟结束

baseline 末次 completion 为 2023，system cycles 也是 2023；closed_page 末次 completion
为 3083，系统继续排空到 3141。应把 `max(completion_cycle)` 与最终 `system_cycles` 分开记录，
后者还要求内存系统达到 idle/drained。不能把所有尾部周期都归为一笔请求的响应延迟。

## 5. 指标的精确定义

设 mapping 行的 `cycle=q`，HostResponse 的 `arrival_cycle=a`、`completion_cycle=c`：

| 指标 | 计算 / 口径 | 易错点 |
|---|---|---|
| 请求数 | mapping 行数 = 唯一 HostRequest ID 数 = response 行数 | 不用 AXI 五通道总记录数代替 |
| 拍跨度字节 | `sum(mapping.size)`，与源 trace 的 W/R `size` 求和相等 | 部分 WSTRB 时不等于使能字节 |
| 提交等待 | `a-q` | 前端/入口接受等待；同周期可接收多笔，不是固定每周期一笔 |
| response latency | `c-a`，必须与 CSV latency 相等且非负 | 包含被模型接受后的服务/排队/返回，不包含 `a-q` |
| 计划到完成 | `c-q=(a-q)+(c-a)` | 仍是离线请求指标，不是应用端到端时间 |
| 均值 / p50 / p95 / max | 按请求样本统计，可分源、读写、转发、区域 | 全局 mean 易掩盖不公平和长尾 |
| 提供负载 | 输入字节 / 输入时间窗口 | 与完成吞吐、物理接口字节口径不同 |
| 应用有用字节 | 由 workload 语义独立计算 | 不由 bus 拍数自动推导 |
| 存储完成吞吐 | 明确的完成字节 / 明确的完成区间 | 周期转秒须使用模型真实 tick 口径 |
| hbm_sim achieved/peak 带宽 | 外部模型按自身 payload/接口/时钟计算 | 不是 HETTrace `sum(size)` 的保证等价量 |

本轮 baseline 自报 achieved 18.634 GB/s、peak 2048 GB/s、利用率 0.91%，但 input 只有 9,536 B、
大部分请求为 16 B，而 DRAM transaction payload 配置为 32 B。若用 trace 字节除模型总时长
自行计算，其含义是另一种有效载荷吞吐。报告必须写清字节层级、窗口和时钟，不能混称同一带宽。

## 6. 现存问题与可验收的优化实验

| 优先级 | 已知问题 | 下一步工作与验收方式 |
|---|---|---|
| P0 | 依赖获取不一致、私有 URL 阻止复现 | 官方 gem5 pin、共同 lock、mem_sim 校验源码包；干净构建与完整回归 |
| P0 | 功能输入规模小，Host 初始化占主导 | 保存切换 tick/ROI；增加 kernel 规模；完整捕获与计算 ROI 分别分析，保持事务完整 |
| P1 | 单一均值掩盖来源和 forwarding | 分源 p95/p99、forwarded/非 forwarded、core/DMA、排队与提交等待分组 |
| P1 | 单个 LLM-like 形状不足以评价控制器 | 扫描 context/decode/hidden、读写比例、同 bank 行冲突、随机与流式地址；固定种子 |
| P1 | 高并发会受 queue/dispatch 影响 | 单独改变 read/write buffer、stack ingress、dispatch width；记录拒收次数与尾延迟 |
| P1 | gem5 功能时序影响原 trace | 固定 memory latency/bandwidth；另做敏感性组，区分输入流改变与外部模型改变 |
| P2 | 原生 AXI 证据不足 | 为目标设备增加独立 pin-level seam/VIP 覆盖，明确与 SYNTH 投影的对应关系 |
| P2 | 模型校准缺口 | 当前 exploratory/standard_default，补厂商参数与命令级参考验证后才作器件精度结论 |
| P2 | 缺少闭环反馈、动态任务分配和设备直接共享 | 单独设计同步接口/调度/一致性扩展并重做功能与因果验证；当前不声称已实现 |

每个新对照先固定一个 trace，只改变一个外部参数；涉及 trace 生成参数时另列实验组。
确定性实验重复用于检查稳定性，不应伪造随机误差条；若测宿主机仿真速度，另记录真实 wall time、
机器负载和多次运行统计，不与模拟器 cycle 混用。

当前 HETTrace 没有 WDATA/RDATA，零值 `data=/expect=` 仅承载请求大小，`data_mismatches=0`
只证明零值替身一致。所有存储结论都属于固定到达流的 open-loop 结果，不能推出 IPC、tokens/s、
应用闭环加速比或未经校准的硬件绝对性能。
