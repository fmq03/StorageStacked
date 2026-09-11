---
title: "异构 XPU 统一 AXI4 HETTrace 项目手册"
date: "2026-09-08"
lang: zh-CN
---

# 1. 项目概览

本手册对应 2026-09-08 文档与可复现性更新。仓库只保存项目增量、测试、workload 与文档；
不保存上游源码树、编译产物、仿真输出或 trace。上游版本统一记录在
[UPSTREAM.md](../UPSTREAM.md)，生成物由 `.gitignore` 排除。

首次使用建议按以下专题阅读：

| 需求 | 完整说明 |
|---|---|
| 理解仿真器层级和运行机制 | [详细架构](07-architecture.md) |
| 添加四棵源码、解决 checkout/登录、完成构建运行 | [构建运行详解](08-build-run.md) |
| 看实验数据、指标定义、对照分析和优化方向 | [实验设计与分析](09-experiments.md) |
| 理解 Host/Vortex/CoralNPU 分工、提交与数据交接 | [多设备协同](10-multi-device.md) |
| 从零适配新的 XPU | [接入流程](11-xpu-integration.md) |

## 1.1 目标与架构

本项目在一个 gem5 SE 进程中运行 host、Vortex 与 CoralNPU，把三者的 memory-side 请求经过
同一个透明 `HetAxiMonitor` 写成 HETTrace v2。gem5 负责功能执行和共享字节；离线转换器将
AXI 事件降级为 GuXing25 `mem_sim/hbm_sim` 的请求流，后者负责 controller/DRAM 时序。

```text
XPU functional execution → unified interconnect HETTrace v2
                         → validate/convert + mapping CSV
                         → external hbm_sim + response CSV
```

这是 open-loop 流程。外部 response 不回灌 gem5，详细结论边界见
[03-limitations.md](03-limitations.md)。

## 1.2 仓库目录

| 路径 | 作用 |
|---|---|
| `libhettrace/` | C++17 HETTrace v2 数据结构与 writer |
| `tools/hettrace/` | 校验、统计、合并、合成与转换 CLI |
| `gem5int/` | 统一 AXI monitor、功能内存、异构配置和验收脚本 |
| `coralnpuint/` | CoralNPU gem5 设备、原生 AXI seam 与补丁 |
| `vortexint/` | Vortex SimX/gem5 集成补丁 |
| `storage_chain/` | 透明 AXI4 边界、协议 checker 与 RTL trace monitor |
| `workloads/` | host/CoralNPU/Vortex 三源及 decoder-LLM 访存 workload |
| `addrmap.json` | 地址空间唯一手写真值源 |
| `scripts/` | 地址生成、环境加载与只读预检 |

# 2. 获取项目与准备环境

## 2.1 获取项目

```bash
git clone git@github.com:hy2581/gem5_new.git zhongxing
cd zhongxing
```

GitHub 仓库不包含四棵外部源码树。按 [UPSTREAM.md](../UPSTREAM.md) 克隆并检出固定 commit，
不要直接用上游最新版本替代已验证基线。

gem5 新安装使用官方 `c8222cc67a399bfc01e8658dd14b30d5bfd634f9`；旧 `2721ed...` 只存在于
历史私有 fork。mem_sim 当前匿名 Git 访问返回 401，使用单独交付的源码包。加载下面的环境后可：

```bash
python3 scripts/upstreams.py fetch \
  --memsim-archive /path/to/mem_sim-7945650579c44713ddec80acc2c82ae34e1f19a5.tar.gz
python3 scripts/upstreams.py check
```

四棵源码的显式逐项命令、Bazel/系统依赖与工具链准备完整列在
[构建运行详解](08-build-run.md)，下面第 3 节是已备齐依赖后的速查。

## 2.2 推荐布局

```text
$HOME/
├── zhongxing/
├── gem5/
├── coralnpu/
├── mem_sim/
└── vortex-gpu/
    ├── vortex/
    └── vxbuild/
```

源码 commit 与 Vortex submodule 版本见 [UPSTREAM.md](../UPSTREAM.md)。

## 2.3 系统工具

已验证平台为 Ubuntu 24.04 x86-64。需要 gcc/g++、make、git、Python 3、CMake、SCons、Bazel、
Icarus Verilog、Verilator、Clang 和 `liblz4-dev`；系统包完整命令见构建运行详解。
`hbm_sim` 使用 C++20。运行 Vortex `.vxbin` 还需要其 RISC-V LLVM 工具链。

## 2.4 加载路径并运行仓库自检

```bash
cd "$HOME/zhongxing"
source scripts/native_env.sh
export HET_JOBS=8
make check
make test-storage-chain
```

源码不在默认同级目录时，先导出 `GEM5_HOME`、`CORALNPU_HOME`、`VORTEX_HOME`、
`VORTEX_BUILD`、`MEMSIM_HOME`，再 source。需要外部构建产物的 `make test-memsim-smoke` 和
`make preflight` 放到第 4 节执行；`native_preflight.sh` 只读检查，不安装或修改文件。

# 3. 安装与构建

## 3.1 Vortex 配置

```bash
mkdir -p "$VORTEX_BUILD"
cd "$VORTEX_BUILD"
"$VORTEX_HOME/configure" --xlen=32 --tooldir="$HOME/tools"
```

若还没有 Vortex 工具链，使用该 build tree 的 `ci/toolchain_install.sh` 安装 LLVM、libc32、
libcrt32 和 riscv32；不需要 FPGA/OpenCL 的完整工具集。

## 3.2 安装项目增量

```bash
cd "$HET_PROJECT_ROOT"
make install
```

`make install` 严格按 Vortex 补丁、Vortex gem5 SimObject、gem5 项目增量、CoralNPU 增量的
顺序执行。安装器可重复运行；单项安装与补丁维护见 [04-integration.md](04-integration.md)。

## 3.3 构建外部 mem_sim

```bash
cmake -S "$MEMSIM_HOME" -B "$MEMSIM_BUILD" -DCMAKE_BUILD_TYPE=Release
cmake --build "$MEMSIM_BUILD" -j"$HET_JOBS"
"$MEMSIM_BIN" --help
```

外部树固定到 [UPSTREAM.md](../UPSTREAM.md) 中的 commit。它不需要项目补丁。

## 3.4 构建 Vortex

```bash
make -C "$VORTEX_HOME/third_party" -j"$HET_JOBS"
env -u DEBUG make -C "$VORTEX_BUILD/sim/simx" \
    USE_GEM5=1 libvortex-gem5 -j"$HET_JOBS"
make -C "$VORTEX_BUILD/sw/runtime/stub" -j"$HET_JOBS"
make -C "$VORTEX_BUILD/sw/runtime/gem5" HOST_ARCH=x86_64 -j"$HET_JOBS"
make -C "$VORTEX_BUILD/tests/regression/vecadd" -j"$HET_JOBS"
```

`third_party/ramulator` 在这里是 Vortex SimX 的必要依赖；不要把它当作 HETTrace 下游。

## 3.5 构建 CoralNPU

```bash
cd "$CORALNPU_HOME"
bazel build --jobs="$HET_JOBS" //gem5int:libcoralnpu-gem5.so //gem5int:ddr_touch.elf
```

## 3.6 构建 gem5 与 workload

```bash
cd "$GEM5_HOME"
/usr/bin/python3 -m venv .venv
.venv/bin/pip install -r requirements.txt
.venv/bin/pip install scons
.venv/bin/scons build/X86/gem5.opt -j"$HET_JOBS"

cd "$HET_PROJECT_ROOT"
make -C workloads/shared_buffer
make -C workloads/vortex_smoke
VORTEX_HOME="$VORTEX_HOME" VORTEX_BUILD="$VORTEX_BUILD" \
    make -C workloads/three_source
make preflight
```

预检成功应显示 `READY`，并找到 `build/X86/params/HetAxiMonitor.hh`、两套设备库、Vortex runtime/
kernel 和外部 `hbm_sim`。

# 4. 运行验收

按从轻到重执行：

```bash
make check
make test-storage-chain
make test-memsim-smoke
make preflight
gem5int/tests/run_het.sh
gem5int/tests/run_vortex_shared.sh
gem5int/tests/run_three_source.sh
make benchmark-llm-memory
```

| 入口 | 验证内容 |
|---|---|
| `make check` | 地址生成物、Python 工具、C++ writer、源码获取与响应审计回归 |
| `make test-storage-chain` | RTL 透明 AXI 边界、字段与反压 |
| `make test-memsim-smoke` | 16 B 粒度小型 trace→外部 hbm_sim→请求/字节/response 对齐 |
| `make preflight` | 命令、固定上游 revision 和全流程构建产物是否齐备 |
| `run_het.sh` | host↔CoralNPU 功能交接 |
| `run_vortex_shared.sh` | host runtime↔CP DMA↔Vortex core 与统一 trace |
| `run_three_source.sh` | 同一 gem5 中三源功能、分类、共享区、AXI 因果和投影守恒 |
| `benchmark-llm-memory` | 较大默认 decoder-LLM trace→外部 hbm_sim；属于长跑实验 |

gem5 脚本会输出保留的 trace 目录和 m5out。正式三源验收应看到三源 `level=interconnect`、
三源 `SYNTH=true`，且 `unmapped=non_monotonic=0`。CoralNPU 原生 seam 能保留当前 16 B 单拍
子集的地址、ID 与 WSTRB，但统一点的五通道事件和其余属性仍由 monitor 从 packet 重构。

# 5. 手工处理 trace

```bash
export PYTHONPATH="$HET_PROJECT_ROOT/tools${PYTHONPATH:+:$PYTHONPATH}"
TRACE_DIR=/absolute/path/to/traces

python3 -m hettrace validate "$TRACE_DIR"
python3 -m hettrace stats "$TRACE_DIR"
python3 -m hettrace convert "$TRACE_DIR" --preset memsim \
    --ticks-per-cycle 1000 -o "$TRACE_DIR/mem_sim.trace"
```

转换默认生成 `mem_sim.trace.map.csv`。运行外部模型：

```bash
"$MEMSIM_BIN" \
  --config "$MEMSIM_HOME/configs/hbm.cfg" \
  --standard hbm4 \
  --trace "$TRACE_DIR/mem_sim.trace" \
  --requests 0 --max-cycles 100000000 \
  --progress-interval 0 --stats-view summary \
  --response-delivery-mode host \
  --response-trace "$TRACE_DIR/hbm_sim.responses.csv"
```

需要自动保留配置、输入清单、执行命令并逐请求审计时，推荐使用：

```bash
python3 scripts/replay_trace.py "$TRACE_DIR" \
  --out "$HET_PROJECT_ROOT/build/replay-$(date +%Y%m%d-%H%M%S)" \
  --ticks-per-cycle 1000 --allow-uninitialized
```

它会重新 validate/convert，运行外部模型，生成 `summary.md`、`run.json` 和 `resolved.cfg`。
真实 trace 无初始镜像，`--allow-uninitialized` 只容忍并报告未初始化状态；合成 smoke 仍要求全 ok。
单独调用 hbm_sim 后仍须核对 response，不应只看进程退出码。

`--ticks-per-cycle` 是实验参数；所有对比必须固定。HETTrace 的数据请求、mapping 和 response
应逐级保持请求数，manifest/mapping 还必须保持字节数。输入中的零值 `data=`/`expect=` 只是让
`mem_sim` 精确识别请求大小，不是原始 WDATA/RDATA；模型会真实写入/比较这些零值，因此其
status/data mismatch 也只能验证零值替身。格式与字段见
[02-trace-format.md](02-trace-format.md)。

# 6. 常用配置

查看完整 gem5 参数：

```bash
"$GEM5_HOME/build/X86/gem5.opt" \
  "$GEM5_HOME/configs/het/het_system.py" --help
```

常用项包括 `--num-cpus`、`--max-ticks`、`--mem-latency`、`--mem-bandwidth`、
`--axi-data-bytes`、`--axi-id-bits`、`--host-trace-inst` 和 `--no-axi-trace`。前两个 memory
参数只改变 gem5 功能 responder，不应作为最终 DRAM 结果；存储实验参数应在外部
`hbm_sim` config/standard 中修改。

地址区域从 `addrmap.json` 修改，随后运行 `make addrmap` 并同步 gem5 常量。Vortex cluster/
core/warp/thread 是编译期配置，应使用独立 `VORTEX_BUILD`，重新构建设备库、runtime 和 kernel。

LLM-like benchmark 可直接调整规模：

```bash
workloads/llm_memory/run.sh \
  --hidden-size 256 --layers 6 \
  --context-tokens 256 --decode-tokens 16
```

# 7. 故障排查

- `HetAxiMonitor` 参数不存在：重新运行 `gem5int/install.sh` 并重编 gem5；
- Vortex 报 Ramulator undefined symbol：把 `$VORTEX_HOME/third_party/ramulator` 放到
  `LD_LIBRARY_PATH` 最前；验收脚本已自动处理；
- vecadd 找不到 kernel：必须在其 regression 目录运行，验收脚本已处理相对路径；
- trace 为空：确认设置 `HETTRACE_DIR`、未传 `--no-axi-trace`，查看 monitor close 统计；
- `unmapped` 非零：检查 `addrmap.json` 生成物与 gem5 地址常量；
- `hbm_sim` request 未完成：检查最大周期、config、输入 cycle 单调性及 mapping/response ID；
- CoralNPU/Vortex 功能错：先看 workload 自检和 gem5 responder，不能用离线时序结果诊断字节值。

# 8. 已验证基线

当前测试矩阵、精确计数、三源 trace 和真实外部 HBM4 重放结果统一维护在
[05-validation-report.md](05-validation-report.md)，并区分 09-05 历史 fork 与 09-08 公开基线。
2026-09-05 基线的工具、writer、RTL、
两源、三源、smoke 与完整 50,487 请求重放均通过。报告中的时序只适用于固定请求流，
不构成硬件绝对性能或应用闭环性能结论。

# 9. 清理与日常维护

提交代码前建议执行：

```bash
make check
make test-storage-chain
git diff --check
```

外部环境齐全时再执行 `make test-memsim-smoke`、`make preflight` 和相应的 gem5 三源回归。
修改 `addrmap.json` 后必须先运行 `make addrmap`，并提交所有同步生成的地址常量。

清理仓库内已知生成物与 Python 缓存：

```bash
make clean
```

依赖源码快照、命名的重放实验目录和 `build/delivery/` 交付包会保留，便于复查与再次交付；
需要释放空间时，先确定具体运行目录及其备份，不要直接删除整个包含外部源码的 `build/`。

该目标不会清理四棵上游源码树的 build 目录，也不会删除验收脚本明确保留在仓库外的 trace/
`m5out`。复现实验时应同时保存 HETTrace manifest、转换参数、mapping CSV、`hbm_sim` 配置和
response CSV；只保存最终统计不足以重放结果。

仓库持久化[当前版本验证报告](05-validation-report.md)与 [实测对照数据](results/20260908-memory-sweep.json)。benchmark 每次运行生成的
`validate.txt`、`stats.txt`、`hbm_sim.txt` 和 `summary.md` 必须与同目录 trace/CSV 一起看，
不应脱离输入另存为“最新结果”。
