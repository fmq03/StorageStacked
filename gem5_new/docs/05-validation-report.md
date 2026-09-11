---
title: "异构 XPU 统一 AXI4 HETTrace 验证报告"
date: "2026-09-08"
status: "PASS"
---

# 当前版本验证报告

本报告区分 2026-09-05 历史私有 fork 结果与 2026-09-08 公开基线/可复现性验证。
第 1～5 节保留历史基线，第 6 节记录本轮验证；不能将旧表自动归属于新 gem5 commit。
固定源码版本及获取方式见 [UPSTREAM.md](../UPSTREAM.md)。
验证在 Ubuntu 24.04.4 LTS x86-64 服务器上完成。它是当前持久化的结果真值来源；各次运行产生的
`validate.txt`、`stats.txt`、`hbm_sim.txt`、`summary.md`、trace 和 CSV 均为可再生产物，不提交
到仓库。

## 1. 当前架构口径

```text
host ───────┬─ shared_buffer ─ CoralNPU
            └─ vortex_bar ──── Vortex
                    │
                    v
       unified memory-side HetAxiMonitor
                    │ HETTrace v2
                    v
       convert --preset memsim → hbm_sim
```

CoralNPU 的 AXI 地址只有 32 位，而 Vortex BAR 位于 4 GiB 以上，因此不存在三方以同一物理地址
直连共享的区域。三源协同由 host 分别完成两条真实字节交接。统一 monitor 从 gem5 packet 重构
五通道 AXI 事件，三源记录均为 `level=interconnect`、`SYNTH=true`；外部 completion 不回灌 gem5。

## 2. 回归矩阵

| 检查 | 2026-09-05 结果 | 覆盖范围 |
|---|---:|---|
| `make check` | PASS：Python 317/317（37 用例）；C++ writer 171/171 | 地址/文档同步、格式、校验、统计、转换、writer |
| `make test-storage-chain` | PASS：6 transactions / 15 events / 3 sources；lint PASS | 五通道透明边界、WSTRB、ID、USER、RESP、LAST、反压 |
| `make test-memsim-smoke` | PASS：596/596 requests；9,536 B 守恒 | 合成 LLM-like trace→mapping→真实外部 HBM4→response |
| `make preflight` | `READY` | 工具、固定 revision、gem5/设备库/runtime/kernel/`hbm_sim` 产物 |
| `run_het.sh` | PASS | host↔CoralNPU 正向共享及 `--npu-no-share` 反向对照 |
| `run_vortex_shared.sh` | PASS | host↔Vortex runtime、CP DMA、core、BAR 共享与 AXI 因果 |
| `run_three_source.sh` | PASS | 同一 gem5 进程中的三源功能、自检、分类、并发与投影守恒 |

Vortex shared 回归中，19,292/19,339 个 host 事务具有非零响应延迟，且
`non_monotonic=0`，覆盖 atomic fast-forward 的延迟完成时间戳路径。

## 3. 最新三源 trace

| 源 | AXI 记录 | 事务 | 数据拍 | 读 | 写 | 字节 |
|---|---:|---:|---:|---:|---:|---:|
| host | 78,127 | 19,391 | 49,478 | 40,124 | 9,354 | 715,984 |
| Vortex | 1,252 | 249 | 881 | 497 | 384 | 13,848 |
| CoralNPU | 320 | 128 | 128 | 64 | 64 | 2,048 |
| **合计** | **79,699** | **19,768** | **50,487** | **40,685** | **9,802** | **731,880** |

三份 meta 均满足 `unmapped=0`、`non_monotonic=0`、`level=3`、`synth=true`。校验器实际观察到：

- `shared_buffer` 被 host 与 CoralNPU 共同访问；
- `vortex_bar` 被 host 与 Vortex 共同访问；
- Vortex core 与 CP DMA 均被正确分类；
- CoralNPU 地址、ID 与部分 WSTRB 保留到统一 packet 投影；
- 50,487 个 AXI 数据拍逐行映射为 50,487 个外部请求。

## 4. 外部 HBM4 重放

同一份三源 trace 使用固定的 `mem_sim` commit 和 `configs/hbm.cfg`，以 `--standard hbm4`、
`--response-delivery-mode host` 完整运行：

| 指标 | 结果 |
|---|---:|
| host requests / DRAM transactions | 50,487 / 50,487 |
| completed reads / writes | 40,685 / 9,802 |
| consumed responses | 50,487 |
| remaining requests / pending | 0 / 0 |
| mapping 与 response ID 集合差异 | 0 |
| response 时序关系错误 | 0 |
| hit cycle limit | false |
| system cycles | 2,476,809 |
| average read latency | 166.99 hbm_sim cycles |

该运行自报 `validation_mode=exploratory`、`model_conformance=standard_default`。输入中的
`data=`/`expect=` 是只承载请求大小的零值替身，因此 `data_mismatches=0` 只验证替身一致性，
不能证明原始 WDATA/RDATA 正确。`system cycles` 和 latency 是固定到达流的 open-loop 存储时序，
不能解释成应用执行时间、IPC、tokens/s 或闭环加速比。

## 5. 文档一致性审计

本轮同时完成以下检查：

- `addrmap.json`、C++/Python 生成物和实际两条交接路径一致；
- 项目手册、README、格式、限制、集成、RTL 边界与 benchmark 文档均描述当前架构；
- 不再引用已删除的旧 trace probe、在线内存后端、协议占位实现或过期报告资产；
- 仓库内 Markdown 相对链接均解析到现存文件；
- `git diff --check` 与所有 shell 脚本语法检查通过。

复现命令、环境变量和故障排查见[项目手册](USER_MANUAL.md)。结论使用前仍须遵守
[结果边界](03-limitations.md)。

## 6. 2026-09-08：公开依赖与实验复现

### 6.1 获取问题与版本来源

官方 gem5 `v25.1.0.1` 为 `c8222cc67a399bfc01e8658dd14b30d5bfd634f9`。旧表将官方 remote
配上私有 fork 的 `2721ed...`，这是 checkout 失败的根因。本轮核查官方 release/tag，并在
独立 Git clone 中检出官方 commit、安装项目增量、构建 X86 gem5。最初复用本机已构建设备库，
三源验证得到与历史表完全相同的 79,699 records / 50,487 requests / 731,880 B。

另外用 `scripts/upstreams.py fetch --only gem5` 从官方 HTTPS remote 实际下载并检出同一 commit，
日志保存在 `build/repro-20260908/public-fetch-gem5.log`；公开可获取性不是仅根据本机对象推断。

mem_sim 匿名 API/网页 404，Git info/refs 401。改为固定提交的源码快照交付。
最初导入完整 161 文件并执行 CMake Release 重编译、14/14 CTest，全部通过。
最终交付按用户要求排除上游旧长手册，剩余 160 文件，构建源码和许可证未变；再次导入及回归通过。
源包没有 `.git` 历史，预检读取版本记录与文件清单，不检查文件内容修改。

新 `upstream.lock.json` 由下载与预检共用；原私有 fork 仍可运行但显示 WARN。
源码获取脚本测试覆盖新 clone、保留脏工作树、错误 revision 拒绝、失败不留半成品、
父仓库误识别拒绝、错误源包版本拒绝、导入后文件缺失检测。

### 6.2 真实三源重放的进一步审计

官方 gem5 + 原设备库生成的三源 trace，用本轮重新编译的 hbm_sim 重放，全部 50,487 个
请求完成、剩余为 0、system cycles 2,476,809、平均读延迟 166.99，与历史值一致。
新增的逐响应核对发现：`ok=10,489`，`uninitialized_data=39,998`，`data_mismatches=0`。

原报告只记录零替身 mismatch 与请求完整性，未记录这项状态分布。实际原因是原始 trace 没有
加载器的初始功能内存镜像。严格的数据状态审计会失败；通过 `--allow-uninitialized` 显式选择
“仅请求/时序”审计后，ID、地址、类型、非负延迟与逐源请求/字节均通过，其他错误仍不允许。
这不代表原始功能数据重放通过，功能结果以 Host/NPU/Vortex 自检为准。

### 6.3 固定 trace 的四组对照

使用同一份小型 LLM-like 三源 trace，固定 596 请求、9,536 B；四组逐请求审计全部通过，
且所有 status=ok，不使用未初始化豁免。

| 参数变化 | system cycles | response mean / p95 | 提交等待 mean / max | forwarded |
|---|---:|---:|---:|---:|
| FRFCFS/open_page/1000 tick | 2023 | 270.20 / 1465 | 0 / 0 | 5 |
| 仅 scheduler=FCFS | 2023 | 270.97 / 1465 | 0 / 0 | 5 |
| 仅 row_policy=closed_page | 3141 | 585.52 / 2133 | 0 / 0 | 70 |
| 仅时间量化=4000 tick | 1985 | 113.62 / 274 | 20.27 / 50 | 170 |

源码生成命令、输入文件清单、来源分布、指标公式，以及 forwarding 导致平均延迟反直觉变化的
解释见[实验设计与分析](09-experiments.md)。本轮实验目录为
`build/repro-20260908/exp-{baseline,fcfs,closed,dense}/`；每组保留 run.json、resolved.cfg、
mapping、responses、summary 与日志。它们是确定性小模型对照，不是应用加速比或器件精度认证。

### 6.4 四棵固定源码重新构建后的完整验收

进一步在 `build/repro-20260908/` 下准备独立源码与构建目录，未改动原来的四棵工作树。
gem5、Vortex 和 CoralNPU 从固定 Git 对象建立新树；Vortex 三个 submodule 同样使用固定版本，
mem_sim 从交付源码包导入。安装当前项目增量后，四个外部组件均重新编译并用于以下回归。
复用了本机 gem5 的 Python/SCons 环境、Vortex LLVM 工具链及 Bazel 下载缓存；这不是全新操作
系统的工具链安装测试。CoralNPU 缺少 LZ4 开发文件时使用局部 sysroot，没有修改系统 dpkg 状态。

| 构建项 | 结果与本轮修正 | 日志（相对上述目录） |
|---|---|---|
| 官方 gem5 X86 | 编译、链接通过；最后安装全新 Vortex SimObject 后再次链接 | `gem5-build.log`、`gem5-relink.log` |
| Vortex SimX/runtime/vecadd | 全部通过；明确构建 `runtime/stub` 与 `runtime/gem5`，不使用漏掉 gem5 driver 的顶层 runtime all | `vortex-simx-build.log`、`vortex-runtime-gem5.log`、`vortex-vecadd-build.log` |
| CoralNPU 库与 ELF | 全部通过；修正共享库缺 `libatomic` 导致 dlopen 失败，并启用链接期未解析符号检查 | `coralnpu-build-final.log`、`coralnpu-smoke-clean.log` |
| mem_sim Release | 构建通过，14/14 CTest；最终无登录源码包重新导入 160 文件通过 | `memsim-build.log`、`memsim-ctest.log` |

最终 160 文件附件另外在 `mem_sim-delivery-final/` 中重新构建，再次通过 14/14 CTest、
596 请求的小型端到端 smoke 和全组件 preflight。日志为 `memsim-delivery-final-build.log`、
`memsim-delivery-final-ctest.log`、`llm-smoke-delivery-final.log`、`preflight-delivery-final.log`。

全新组件组合的 `make preflight` 为 READY；CoralNPU 独立 smoke、Host↔NPU 正向及断开共享
反向对照、Host↔Vortex vecadd、真实三源功能/trace/投影全部通过。Host 输出 NPU
`tag=0x600d sum=0x17e0`，Vortex `dst[0]=4187.0 dst[3]=16477.0`。

| 来源 | records | transactions | 数据拍 / 投影请求 | bytes |
|---|---:|---:|---:|---:|
| Host | 78,139 | 19,393 | 49,486 | 716,112 |
| Vortex | 1,252 | 249 | 881 | 13,848 |
| CoralNPU | 320 | 128 | 128 | 2,048 |
| 总计 | 79,711 | 19,770 | 50,495 | 732,008 |

两设备 trace 活动区间有交集，unmapped/filtered 均为 0。相对 6.1 的旧设备库组合，差异仅在
Host 的 12 条记录 / 2 笔事务 / 8 个数据拍 / 128 B；不能混用两组 trace 的固定计数。
本轮未用单变量对照定位这项差异的具体成因，因此不将它归因为 gem5 基线变化或性能改善。

真实三源再送入新编译的 hbm_sim：**50,495 / 50,495 请求完成，732,008 B 守恒**，
`remaining_requests=remaining_pending=0`、`hit_cycle_limit=false`，system cycles 为
2,479,105，平均读延迟 168.80 cycles。逐响应审计记录 `ok=10,497`、
`uninitialized_data=39,998`，仍使用 6.2 所述的显式请求/时序口径，而不是原始数据复现认证。
全部响应平均延迟 168.4、p95 522 cycles，写缓冲转发 5 笔。

证据入口为 `three-source-clean.log`、`three-source-clean/traces/`、
`three-source-clean/m5out/`、`three-source-clean/hbm4/summary.md` 与原始 response CSV。
源码自检另有 Python 工具 317/317（37 用例）、C++ writer 171/171、工作流 13 个测试；
RTL 边界 6 事务 / 15 事件 / 3 来源及 lint 通过。以上报告区分功能正确性、记录完整性和
离线时序结果；没有运行 FPGA/ASIC 综合，也没有测量应用级闭环加速比。
