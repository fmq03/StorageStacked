# 异构 XPU 统一 AXI4 HETTrace

本项目把 gem5 中 host CPU、Vortex GPU 和 CoralNPU 的 memory-side 流量放到同一个互连观察点，
写成统一的 **HETTrace v2 AXI4 五通道记录**，再离线投影给外部
`mem_sim/hbm_sim` 做存储控制器与 DRAM 时序仿真。该上游当前匿名不可访问，
请使用[固定版本源码包导入流程](UPSTREAM.md#mem_sim-无登录获取)。

```text
host CPU ─┐
Vortex  ──┼─> gem5 功能数据通路 ─> HetAxiMonitor ─> HETTrace v2/source
CoralNPU ─┘                                      │
                                                 ├─> validate / stats
                                                 └─> convert --preset memsim
                                                        └─> external hbm_sim
```

这里有意分开两种真值：gem5 和真实设备模型负责功能字节与程序执行；外部 `hbm_sim` 负责固定
请求流的 open-loop 存储时序。外部 completion 不反馈给 gem5，因此不能从离线结果声称 IPC、
tokens/s 或应用级闭环加速比。

## 当前实现

- `HetAxiMonitor` 是主配置中唯一的 memory-side 观察点，并按 gem5 requestor 分类三源；
- 三个源在统一观察点都已经是 gem5 packet；monitor 将其确定性分段成合法、地址对齐的
  INCR burst，所有五通道记录都标记 `SYNTH`；
- CoralNPU 原生 timing seam 在当前 16 B 单拍子集中保留地址、ID 与 WSTRB，但
  AW/W/B/AR/R 事件、时序、LEN/SIZE/BURST/USER 及响应仍由统一 monitor 重构；
- 每源独立文件、统一 gem5 tick，`merge` 时稳定排序；
- `convert --preset memsim` 从 W 保留写粒度/WSTRB、从 AR 保留读发射时刻，生成精确
  字节大小的请求和逐行映射 CSV；其中零值 `data=`/`expect=` 只是大小载体，不是 WDATA/RDATA；
- 项目侧旧的在线 Ramulator、UCIe/MC/DFI 占位链和 Python 内置 memsim 已移除。

Vortex 的 `third_party/ramulator` 仍是 SimX 自身依赖，不能删除；它不是本项目的存储时序后端。

## 快速检查

首次安装先读[四棵外部源码的获取、构建与运行](docs/08-build-run.md)。gem5 的公开基线已修正为
官方 `v25.1.0.1` / `c8222cc67a399bfc01e8658dd14b30d5bfd634f9`；原 `2721ed...` 是历史私有
fork 提交。下载与预检统一读取 [upstream.lock.json](upstream.lock.json)。

```bash
source scripts/native_env.sh
make check
make test-storage-chain
make test-memsim-smoke
make preflight
```

完整上游树构建好后：

```bash
gem5int/tests/run_het.sh
gem5int/tests/run_vortex_shared.sh
gem5int/tests/run_three_source.sh
# 可选长跑
make benchmark-llm-memory
```

`run_three_source.sh` 同时验证 workload 数据、统一观察层级、来源分类、AXI 因果、共享区域以及
`mem_sim` 投影守恒。`test-memsim-smoke` 会快速把小型 LLM-like trace 真正送入外部 `hbm_sim`
并按 sidecar 对齐所有 response；`benchmark-llm-memory` 保留较大的默认实验规模，运行更久。

最新回归的逐项数字、执行日期和模型口径统一记录在
[当前版本验证报告](docs/05-validation-report.md)。快速与完整三源 trace 都已实际送入外部
`hbm_sim` 并完成请求、字节、mapping 与 response 守恒检查；这些结果不代表产品性能。

## 目录

| 路径 | 作用 |
|---|---|
| `libhettrace/` | C++17 HETTrace v2 记录与 writer |
| `tools/hettrace/` | 读取、校验、统计、合并、合成和转换 CLI |
| `gem5int/` | 统一 AXI monitor、异构配置及安装/验收脚本 |
| `coralnpuint/` | CoralNPU gem5 设备与原生 AXI seam |
| `vortexint/` | Vortex gem5/trace 集成补丁 |
| `storage_chain/` | 透明 AXI4 边界、协议 checker 和 RTL trace monitor 参考 |
| `workloads/` | 两源、三源及 decoder-LLM 访存 workload |
| `addrmap.json` | 地址空间唯一手写真值源 |

## 文档

- [项目手册](docs/USER_MANUAL.md)：项目边界、环境、构建、运行、维护和结果解释；
- [详细架构](docs/07-architecture.md)：层级、模块、状态所有权、端口交互与事件运行机制；
- [构建运行详解](docs/08-build-run.md)：四棵源码逐项添加命令、工具链、产物、完整重放及排错；
- [实验设计与结果分析](docs/09-experiments.md)：实测参数对照、指标定义、现象分析和优化空间；
- [多设备协同](docs/10-multi-device.md)：Host 编排、Vortex/NPU 控制、数据交接与同步；
- [新增 XPU 接入](docs/11-xpu-integration.md)：契约、ABI、SimObject、地址、分类与逐级验收；
- [地址图](docs/01-address-map.md)：统一物理地址及硬约束；
- [HETTrace v2 格式](docs/02-trace-format.md)：字段、层级和投影；
- [结论边界](docs/03-limitations.md)：open-loop 能与不能说明什么；
- [上游集成](docs/04-integration.md)：安装内容和补丁维护；
- [验证报告](docs/05-validation-report.md)：当前版本的测试矩阵、三源 trace 与 HBM 重放结果；
- [RTL AXI 边界](docs/06-storage-chain-plan.md)：参考模块与限制；
- [固定上游版本](UPSTREAM.md)：公开 gem5 commit、历史基线、mem_sim 访问与离线交付。
