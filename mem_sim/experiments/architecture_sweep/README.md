# 定制化 HBM/LPDDR 架构实验

该实验只通过两份权威 master 配置加临时 `[override]` 改变模型，不维护第三份入口
配置。预置三组 bank 组织、三组 row/column 几何和 `per_bank`/`all_bank` 两种刷新
策略。它不是把同一份随机流量套到所有 case，而是按目标维度生成可审计的定向 trace：

- bank：请求同时到达并轮转目标 bank；同一 bank 每轮使用一个不重复 row，避免
  FR-FCFS 借助 row wrap 重排出额外命中，用于隔离 bank-level parallelism；
- geometry：在固定 transaction footprint 上等距扫描单 bank，比较不同 column 宽度
  带来的 row hit/conflict 变化；
- refresh：在相同研究型 `nREFI/nREFIpb` 与 `nRFC/nRFCpb` 下保持持续流量，隔离
  per-bank/dual-bank 与 all-bank scope。压力 timing 明确标记为 `research`，不是 JEDEC
  baseline 或器件值。

定向地址按照主配置当前的 32 B DRAM transaction 粒度编码；不要把 64 B host line
当成 column 步长。每个 host request 仍可在 frontend 中拆成两个 32 B transaction。

```bash
python3 experiments/architecture_sweep/run.py
```

默认扫描当前两种新标准 HBM4 和 LPDDR6。工具也支持四标准统一复跑：

```bash
python3 experiments/architecture_sweep/run.py \
  --standards hbm3,hbm4,lpddr5,lpddr6
```

LPDDR6 的 bank/geometry 组已经关闭 refresh，因此在缩减为单 BG 的 geometry
case 中会同步关闭不会执行的 REFdb 能力；refresh 组保留 REFdb，并继续要求偶数 BG
成对。这是组织参数与协议能力的联动，不是绕过配置校验。

快速检查单一标准：

```bash
python3 experiments/architecture_sweep/run.py --standards hbm4 --requests 512 \
  --out outputs/experiments/architecture_sweep_quick
```

运行时逐 case 显示进度，单 case 默认 120 秒超时，可用 `--case-timeout` 调整。输出包含
`results.csv`、`checks.csv`、`summary.md`、离线 `trends.html`，以及每个 case 的
`workload.trace`、`resolved.cfg` 与 `stats.txt`。自动检查包括：无 cycle-limit/数据错误、
bank scaling、geometry hit/conflict 趋势，以及 REFpb/REFdb/REFab scope 是否实际分离。
bank scaling 的硬门禁比较最大与最小 bank 组织，不要求三个中间点逐点严格单调，因为
命令/数据总线饱和、映射和调度可能造成小幅回落；全部点仍保存在结果中。任一检查失败时
脚本返回非零。结果属于模型参数敏感性研究，不代表某款器件。
