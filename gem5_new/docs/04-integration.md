# 上游源码集成

本项目在四棵外部树旁工作：gem5、Vortex、CoralNPU 和 GuXing25 `mem_sim`。固定 commit 见
[UPSTREAM.md](../UPSTREAM.md)。前三棵提供功能执行和 trace 产生端；`mem_sim/hbm_sim` 是独立、
只读消费 trace 的存储时序端，项目不会给它打在线回调补丁。

从零获取四棵源码、公开 gem5 SHA 修正与 mem_sim 离线包导入见
[构建运行详解](08-build-run.md)。新增设备的完整步骤见[XPU 接入流程](11-xpu-integration.md)。

## 职责与安装内容

| 树 | 环境变量 | 本项目行为 |
|---|---|---|
| gem5 | `GEM5_HOME` | 安装 CoralNPU 设备、`HetAxiMonitor`、功能内存和异构配置；给 `DmaPort` 增加 byte-enable |
| Vortex | `VORTEX_HOME` | 安装 trace/异步请求 ABI 与多 outstanding/WSTRB 补丁 |
| CoralNPU | `CORALNPU_HOME` | 安装 gem5 动态库、原生 AXI 回调 seam 和测试内核 |
| mem_sim | `MEMSIM_HOME` | 固定上游源码，直接构建 `hbm_sim`；无项目内在线后端 |

Vortex 的 `third_party/ramulator` 是 SimX 既有依赖，构建 Vortex 时仍必须存在。它与外部
`MEMSIM_HOME` 是两个不同角色，不能用其中一个替代另一个。

## 安装顺序

先加载路径：

```bash
source scripts/native_env.sh
```

然后安装增量：

```bash
# Vortex 树先打补丁，再把补丁后的 gem5 SimObject 安装进 gem5
VORTEX_HOME="$VORTEX_HOME" ./vortexint/install.sh
GEM5_HOME="$GEM5_HOME" "$VORTEX_HOME/sim/simx/gem5/install.sh"

# 项目自己的 gem5 与 CoralNPU 内容
GEM5_HOME="$GEM5_HOME" ./gem5int/install.sh
CORALNPU_HOME="$CORALNPU_HOME" ./coralnpuint/install.sh
```

重复安装是幂等的；三个项目安装器都支持 `--revert`。`mem_sim` 不需要安装脚本：

```bash
cmake -S "$MEMSIM_HOME" -B "$MEMSIM_BUILD" -DCMAKE_BUILD_TYPE=Release
cmake --build "$MEMSIM_BUILD" -j"$(nproc)"
```

## gem5 边界

`gem5int/install.sh` 镜像以下目录：

- `src/hettrace/`：透明 request/response monitor 及其 SimObject 构建声明；
- `src/dev/coralnpu/`：CoralNPU SimObject；
- `src/mem/unified_timing/`：稀疏功能 responder；
- `configs/het/`：独立与异构配置。

唯一修改上游既有源码的 gem5 补丁是 `patches/dma_byte_enable.patch`，用于把真实 WSTRB 作为
byte-enable 送过 timing request，避免设备侧额外 RMW。主配置的唯一正式 trace 点是
`system.axi_monitor`；per-device tap 在该配置中关闭，只保留给 standalone 诊断。

atomic fast-forward 有一个容易踩中的时间语义：`sendAtomic()` 返回服务 delay，但不会在 monitor
的当前调用栈中推进 `curTick()`。统一 monitor 因此按来源暂存合成的 B/R 完成事件，到其完成
tick 到达或仿真退出时再依次写出；AW/AR/W 仍使用真实 `curTick()`。这样既不会把 atomic 响应
伪装成零延迟，也不会先写入未来响应、随后让下一笔当前请求造成时间戳回退。
`run_vortex_shared.sh` 同时检查 `non_monotonic=0` 和多数 host 事务具有非零响应延迟。

复制 SimObject `.py` 或 C++ 后必须重新构建，生成的 params 头才会同步：

```bash
cd "$GEM5_HOME"
.venv/bin/scons build/X86/gem5.opt -j"$(nproc)"
test -f build/X86/params/HetAxiMonitor.hh
```

## Vortex 与 CoralNPU 补丁栈

Vortex 安装器按以下顺序维护：

```text
trace ABI → timing_feedback.patch → axi_transactions.patch
```

后两层让 core 与 CP DMA 能经 gem5 timing request 完成功能数据交接，并携带 transaction context
和 byte-enable；它们不表示外部 `hbm_sim` completion 会反馈。卸载必须逆序，安装器已处理。

CoralNPU 的 base/async wrapper 补丁也有顺序，提供非阻塞 AXI issue/completion seam，使真实
READY/VALID、ID、WSTRB 和 RESP 可由 gem5 驱动。不要手工重复应用单个 hunk。

CoralNPU 共享库显式链接 `libatomic`，并用 `-Wl,-z,defs` 在链接阶段拒绝未解析符号；
这是干净 Clang/Verilator 构建后 `dlopen` 不应依赖宿主进程碰巧提供原子操作符号的保证。
`coralnpuint/tests/run_smoke.sh` 用纯 C 调用 ABI，应在新库构建后先运行一次。

## 补丁打不上

安装器用反向 dry-run 判断补丁是否已经存在。若仍报错：

1. 对照 [UPSTREAM.md](../UPSTREAM.md) 核对目标 commit；
2. 检查目标树是否已有用户修改，不要覆盖；
3. 查看 `.pre-hettrace`、`.pre-timing-feedback` 等备份，只作为 diff 参考；
4. 将必要接口移植到新上游，重新生成最小增量补丁；
5. 验证安装、重复安装、`--revert`、重新安装和完整三源回归。

CoralNPU 单文件补丁相对 `hw_sim/` 使用 `-p0`；Vortex 组合补丁相对树根使用 `-p1`。更新后一
定先检查 patch stack 的前置层，不能从最终文件直接套用旧上下文。

## 常见问题

若 Vortex `dlopen` 报 Ramulator 符号缺失，通常是加载了系统中另一份同名库。把其私有依赖
放在最前：

```bash
export LD_LIBRARY_PATH="$VORTEX_HOME/third_party/ramulator${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
```

若 gem5 启动时不认识新参数，重新运行对应安装器并重编 gem5，不要只复制 Python 配置。若
`HETTRACE_DIR` 没有 trace，先看 monitor 的 close 统计和 source requestor 分类，再检查地址图；
不要启用 per-device 诊断 tap 来掩盖统一观察点的问题。
