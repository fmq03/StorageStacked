# 从零新增 XPU 的接入流程

本页定义在当前框架中增加第四个设备 `myxpu` 的实施路径。目录名、ID 与地址示例用于说明
新增工作，仓库目前没有 `MyXPU` 实现，也没有一个配置项能自动注册任意新设备。
目标是保持现有 Host/Vortex/CoralNPU 回归通过，同时让新设备进入同一功能内存与统一观察点。

## 1. 先确定接入契约

在设备适配目录中新增设计说明，记录下面这些具体选择，作为代码和验收共同依据：

| 契约 | 必填内容 |
|---|---|
| 源码与环境 | 公开/授权获取入口，固定 commit、submodule、工具链、构建命令、许可证 |
| 执行模型 | C++ transaction/cycle 模型或 RTL/Verilator；本地 SRAM/TCM 与外部访存边界 |
| 地址空间 | 设备地址位宽、Host VA/PA 转换、PIO/BAR/共享区的基址和大小 |
| 事务子集 | 读写宽度、对齐、burst、byte-enable、ID 位宽、同 ID 顺序、最大 outstanding |
| 时间 | 设备频率、一个 tick 调用的步长、外部请求 issue/completion 语义 |
| 控制 | 初始化、加载、启动、完成、错误、超时、reset/restart 是否支持 |
| Host API | 同步 PIO 或异步队列，buffer 所有权、事件依赖、线程上下文数 |
| 观察 | 来源 ID、requestor 分类、ctx/stream/substream、是否为 SYNTH |

第一版建议支持对齐单拍读写、部分 byte-enable、有限 outstanding 和确定性测试 kernel。
超出已声明子集的事务要显式拒绝或拆分，不能静默截断地址、ID 或 mask。

## 2. 准备隔离环境与最小设备库

使用[构建指南](08-build-run.md)的独立 `build/dependencies` 布局。新设备源码放在另一个
目录，用 `MYXPU_HOME`、`MYXPU_BUILD` 表示，在 `upstream.lock.json` 及下载/预检脚本中
新增获取与检查逻辑；当前 `scripts/upstreams.py:paths()` 也需要增加该路径，不能只改 JSON。

先在 gem5 之外编译设备模型，完成复位、加载、单步和自检。RTL 模型的生成 C++ 只放在设备库
内部；不要让 Verilator 头、Bazel 类型或私有 STL 容器跨越动态库 ABI。建议照
`coralnpuint/coralnpu_gem5.h` 定义不透明句柄和 `extern "C"` 函数：

```text
create / destroy / build_info / abi_version
load_program / start / tick_once / status
set_timing_backend(issue_read, issue_write, context)
complete_read(token, bytes, response) / complete_write(token, response)
```

这是推荐的新接口契约，现有两设备 ABI 的实际符号以各自头文件为准。`token` 必须能唯一标识
活跃事务，不能只用可能复用的 AXI ID。明确写 buffer 的复制/持有期限，读回指针的生命周期，
以及错误响应如何传播。动态库导出用 `nm -D --defined-only` 核对，写一个 dlopen smoke 做
create/load/tick/destroy，类似 `coralnpuint/tests/smoke_dlopen.c`。

运行期库调用必须非阻塞、可返回；不能在等待外存时用 `while(...) tick()` 自己推进时间。
初始化或加载若内部推进时钟，只能发生在该设备正式加入调度之前，且必须注明不属于测量区间。

## 3. 在 gem5 中增加 SimObject 和功能端口

新增这些文件（均为待实施路径）：

```text
myxpuint/                       库 ABI、构建、安装脚本、设备测试 kernel
gem5int/src/dev/myxpu/
    MyXPU.py                    SimObject 参数、PIO/DMA、时钟与库路径
    SConscript                  SimObject 与 C++ 构建声明
    myxpu_dev.hh / .cc          动态库绑定、PIO、事件与事务队列
gem5int/configs/het/myxpu_only.py
workloads/myxpu_shared/          Host↔新设备功能 workload
gem5int/tests/run_myxpu.sh
```

适配器可参考 CoralNPU 的 `DmaDevice`。PIO 读写实现寄存器协议，声明准确 `getAddrRanges()`；
用 `EventFunctionWrapper` 在 `clockEdge(Cycles(1))` 调度设备周期；`startup()` 加载库/程序，
关闭时释放资源，支持需要的 drain 行为。任何 restart/checkpoint 能力在测试前不标为支持。

每次 issue 建立包含 token、ID、sequence、地址、大小、payload/byte-enable 的持久上下文，
调用 DmaPort timing API。采用裸 RequestPort 时，还必须自行实现请求重试、响应反压与 sender
state 生命周期；采用 DmaPort 时，明确它可能按 cache line 拆分请求，避免丢失原事务边界。
只有真实 gem5 completion 到达后才调用设备 `complete_*`，再按设备规定允许后继执行。
当前 `dma_byte_enable.patch` 可复用，不能为了支持部分写而无意额外发一笔 RMW 读。

## 4. 分配地址并接入共享系统

在 `addrmap.json` 的 sources 为 `myxpu` 分配新 ID（现有 0/1/2 保持不变，例如新用 3），
增加所需 regions/accessors/trace_windows，再运行：

```bash
make addrmap
make check-addrmap
```

同时修改 `het_system.py` 中显式地址常量与配置，增加 `--myxpu-library`、kernel 与启用参数，
实例化设备并连接 `myxpu.pio = membus.mem_side_ports`、`myxpu.dma = membus.cpu_side_ports`。
设备外存请求应走已有 `system.axi_monitor`，不要旁接第二条不受观测的内存路径。

PIO 可在未占用低地址规划一个窗口（例如评估 `0x40000000` 一页），必须先运行地址图校验；
共享数据优先用设备可寻址且已由 `UnifiedTimingMemory` 拥有的区域，采用独立子区避免覆盖
现有 workload。为 Host 增加 uncacheable 映射。新地址不能落入 SE 普通页池；如需要新内存段，
同步 responder 的 ranges，而非只改 `system.mem_ranges` 列表。

地址生成器目前特别约束 CoralNPU DDR 窗口及其 32 位地址。若新设备需要独立 DDR/BAR 超出此
窗口，需要调整这些校验为针对实际 accessor 的规则，并测试原 CoralNPU 越界仍被拒绝。
这属于需实现的扩展，不能简单扩大 `npu_addr_bits` 来绕过真实硬件限制。

## 5. 扩展统一 trace 分类（必须改代码）

| 修改点 | 必要动作 |
|---|---|
| `addrmap.json` / `make addrmap` | 生成 `kSrcMyxpu`、时钟、地址与 Python 名称映射；保持既有 ID 稳定 |
| `HetAxiMonitor.py` | 增加 `trace_myxpu` 与 requestor pattern 参数 |
| `het_axi_monitor.hh/.cc` | 扩展 `Source`、writer、打开/关闭/统计、`classify()` 和 user/src/context 映射 |
| `het_system.py` | 配置来源启用参数，独立设备 tap 默认关闭 |
| Python 工具 | 审计 source 名、过滤、合并、validator 的活动重叠与共享区逻辑 |
| 回归脚本 | 新增四源检查；原三源脚本仍只验证原三源，不直接改成四源固定计数 |

当前分类先匹配 CoralNPU，再匹配 Vortex，其他默认 Host。若仅把新设备 DMA 接到 membus，
它可能被静默记为 Host，所以“第四源文件存在、Host 分类未受污染”是必须独立断言的验收项。
不要让新设备 requestor 名包含已有来源 pattern。每源独立文件，统一全局 tick，
`txn` 用 `(src_id, txn)` 关联，ctx/stream/substream 的语义写进设备说明。

packet 投影继续标 `level=interconnect`、`SYNTH=true`。如果未来输出原生 RTL 五通道 trace，
应在单独观察层级验证并明确转换的损失，不能在统一 monitor 文件里偷偷取消 SYNTH。
source 数扩展通常不改变 v2 的 56 B 布局；但超过现有 `user` 等字段位宽，或新增 payload，
需设计格式升级和向后兼容检查，不能截断。

## 6. 安装、构建与逐级调试

把新目录加入 `gem5int/install.sh` 的安装集合，增加 `myxpuint/install.sh`，定义幂等安装与
明确的项目文件撤销范围；更新顶层 Makefile 的串行安装顺序及 preflight 的参数头/库/kernel
检查。源码安装完成后先构建设备库，再构建 gem5 与 Host workload。

调试顺序为：设备库 standalone → 无 CPU 的 gem5 单设备 → Host↔myxpu → 原三源回归 → 四源
并发 → 离线重放。每一级使用独立输出目录，定位通过后再增加交互面。单设备 bring-up 可使用
`python3 -m hettrace validate TRACE_DIR --allow-single-source`；正式多源验收不能带此豁免。

## 7. 验证与接入完成标准

| 测试 | 通过标准 |
|---|---|
| ABI / 生命周期 | 必需符号齐全，create/load/start/tick/complete/destroy 可重复建销，无悬空上下文 |
| 真实数据 | 非零非均匀输入、精确结果、部分 mask 保留未使能字节、边界地址读写 |
| 时序与顺序 | 未完成请求不会提前退休；多 outstanding/同 ID/不同 ID/重试和响应反压均无丢失重复 |
| 反向对照 | 断开共享、错地址或故意损坏结果至少一项能触发预期失败 |
| trace | 新来源计数非零，其他来源不被污染；meta=interconnect/SYNTH；unmapped/non_monotonic=0 |
| 协同 | workload 明确依赖和所有权，存在预期区域交接与活动重叠，无超时 |
| 离线 | request/mapping/response ID 一致、字节数一致、类型/地址/状态/时序正确 |
| 回归保护 | `make check`、RTL、原三源和新四源回归全部通过 |

针对支持的协议子集还要覆盖跨 4 KiB/非对齐处理、最大 burst、ID 复用和错误响应；不支持的
组合必须可诊断地失败。定延迟加大/总线带宽降低的对照应让设备等待增加，用以发现提前 completion。

接入完成时提交设备契约、固定版本/获取方式、安装构建命令、地址图与生成物、ABI、配置、测试、
结果表和已知限制，并记录输入清单、实际配置与构建/运行命令。此处“上线”指合入仿真框架并通过回归；
未包含真实硬件部署、完整操作系统驱动或远端发布。可用新设备开关关闭新路径，以便逐级定位回归。
