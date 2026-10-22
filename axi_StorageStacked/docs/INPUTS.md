# 项目输入：AXI 信号、配置与 SMOKE 驱动

[返回文档目录](README.md)。

本文对应 `VERSION=1.0.0` 的源码接口。输出字段、实际测试结果以及本次改动清单见
[OUTPUTS.md](OUTPUTS.md)。简明端口契约见 [INTERFACE.md](../INTERFACE.md)。
需要从空的测试文件开始开发时，见 [从零添加 SMOKE 测试](03-从零添加SMOKE测试.md)。

## 1. 本项目接收什么

本项目接收 **AXI4 子集的真实五通道信号**，把访存请求经过 AXI2Flit、UCIe 链路送入
在线 mem_sim，再把结果沿原链路返回。地址、数据、掩码、ID、握手和时钟都是模型的实际输入。

```text
处理器驱动的 MasterPorts ─────────────────────────┐
                                                 ↓
JSON → FileMaster → Signals → AouBackend → AXI2Flit → UCIe → mem_sim
                     ↑                              返回 B / R
                     └────────────────────────────────────┘
```

两种接入方式共用同一个存储实现：

| 接入方式 | 用户提供的输入 | 适用场景 |
|---|---|---|
| SystemC 信号集成 | `clk`、`resetn`、AXI 输入信号、`StorageConfig` | gem5+Vortex 或 CoralNPU 等主设备驱动 |
| 独立文件运行 | 一个 JSON 文件和结果目录 | 无处理器环境的读写试验、SMOKE、接口验收 |

JSON 是独立测试驱动的输入格式，存储模块本身没有 JSON 端口。文件驱动在仿真中生成 AXI 握手，
不直接写入内存模型。Python 中的字节级参考模型只比较返回结果，不提供存储侧读数据。
本项目输入也不包含 ELF、指令、神经网络或 TLM payload；这些由上游驱动处理。

## 2. 信号级输入

以下方向均以存储侧 `storage_axi::AouBackend` 为参照。
声明位于 [axi_signals.hh](../storage_axi/axi_signals.hh)。

| 输入信号 | 位宽 | 含义与要求 |
|---|---:|---|
| `clk` | 1 | AXI 时钟，所有传输在上升沿的 `VALID && READY` 时发生 |
| `resetn` | 1 | 低有效启动复位；当前接口不支持运行中取消在途事务 |
| `awaddr` | 64 | 写 burst 首地址，按每拍字节数对齐 |
| `awid` | 16 | 写事务 ID；实际允许值为 1～1023 |
| `awlen` | 8 | 写拍数减一，0 表示 1 拍、255 表示 256 拍 |
| `awsize` | 3 | 每拍字节数的 log2，允许 0～5，即 1～32 字节 |
| `awburst` | 2 | 必须为 1，即 INCR 递增地址 burst |
| `awvalid` | 1 | AW 地址负载有效；等待 `awready` 期间保持地址负载 |
| `wdata` | 256 | 本拍 32 个总线字节通道的数据，低 8 位是 lane 0 |
| `wstrb` | 32 | bit i 为 1 时，允许写入 `wdata[8*i+7:8*i]` |
| `wlast` | 1 | 当前写 burst 的最后一拍为 1 |
| `wvalid` | 1 | 写数据有效；等待 `wready` 期间保持数据、掩码和末拍标志 |
| `bready` | 1 | 主设备可接收写响应；可置 0 对 B 通道施加反压 |
| `araddr`、`arid` | 64、16 | 读 burst 首地址与 ID，限制同写地址通道 |
| `arlen`、`arsize`、`arburst` | 8、3、2 | 读拍数减一、每拍大小、INCR 类型 |
| `arvalid` | 1 | AR 负载有效，等待 `arready` 时保持负载 |
| `rready` | 1 | 主设备可接收一拍读数据；可置 0 对 R 通道施加反压 |

AW 与 W 是独立通道，不能把 AW 接收理解为写完成。当前桥接按 AW 顺序接收 W，
AW 尚未接收时可以暂时拉低 WREADY；主设备需要持续保持 WVALID 和负载。
W 通道没有 WID。主设备负责分配 ID，完成 B 或最后一拍 R 握手后才可回收、复用。
不要重复使用活跃 ID。

地址规则：

- `bytes_per_beat = 1 << AxSIZE`，`beats = AxLEN + 1`。
- 首地址必须按 `bytes_per_beat` 对齐。
- `address % 4096 + bytes_per_beat * beats <= 4096`，一个 burst 不跨 4 KiB。
- 整笔访问必须位于 `[base, base+size)`；结构合法但超出窗口时返回 DECERR。
- 对齐、burst 类型等结构错误不属于普通 DECERR 用例；文件入口会拒绝，直接信号集成必须遵守契约。
- 不提供外部 lock/cache/prot/user/qos 端口；资源平面由 `ID % planes` 选取。

### 2.1 字节顺序、窄拍与掩码

第 `b` 拍的有效数据从下列总线字节通道开始：

```text
beat_address = address + b * (1 << size)
lane = beat_address % 32
总线字节 [lane + j] = 当前拍的第 j 个输入字节
```

例如向 `0x9000001c` 写 4 字节 `aa bb cc dd`，`size=2`：

| 拍内字节 j | 绝对地址 | 总线 lane | 输入数据 | JSON strobe=5 是否写入 |
|---:|---|---:|---|---|
| 0 | `0x9000001c` | 28 | `aa` | 是 |
| 1 | `0x9000001d` | 29 | `bb` | 否，保留原值 |
| 2 | `0x9000001e` | 30 | `cc` | 是 |
| 3 | `0x9000001f` | 31 | `dd` | 否，保留原值 |

文件中的拍内掩码 `5=0b0101` 会转换为总线 `WSTRB=5<<28=0x50000000`。
直接连接信号时，驱动必须自行放置数据和掩码。若原数据为 `1c 1d 1e 1f`，
写后回读应为 `aa 1d cc 1f`，而非完整替换为 `aa bb cc dd`。

## 3. 独立运行的 JSON 输入

入口为 [scripts/run.py](../scripts/run.py)，信号产生器为
[examples/axi_storage.cc](../examples/axi_storage.cc) 中的 `FileMaster`。

```bash
# 在 axi_StorageStacked 仓库根目录执行
./run.sh setup
./run.sh build
./run.sh run --input examples/roundtrip.json --output results/my-roundtrip
```

结果目录必须不存在。省略 `--output` 时自动生成带时间戳的新目录；相对路径按调用命令时的
工作目录解析。`setup` 仅检查基础工具和源码存在性，实际编译依赖由 `build` 验证。
编译需要 Linux x86-64、C/C++20 编译器、CMake ≥ 3.20、make、Python 3 与 Boost 头文件。

输入顶层只允许 `config` 和 `transactions`；未知字段会报错。
`config` 可以省略或为空，`transactions` 必须是非空数组。
文件驱动严格串行完成每笔事务，因此输入顺序也是响应顺序。

```json
{
  "config": {"standard": "hbm4", "response_stall_cycles": 3},
  "transactions": [
    {"command": "write", "id": 1, "address": 2415919104,
     "size": 2, "data": ["29000000"], "strobe": [15]},
    {"command": "read", "id": 2, "address": 2415919104,
     "size": 2, "beats": 1, "expected": ["29000000"]}
  ]
}
```

这里 `2415919104` 是 `0x90000000`。JSON 数字使用十进制整数，不接受 `0x...` 数字语法、
十六进制字符串地址、浮点数或用布尔值代替整数。数据字符串按地址从低到高排列：
`"29000000"` 表示字节 `29 00 00 00`，若按小端 uint32 解释就是十进制 41；模型存储的是字节。

### 3.1 每笔事务的字段

| 字段 | 类型与默认值 | 约束、作用 |
|---|---|---|
| `command` | 必填字符串 | 只能为 `read` 或 `write` |
| `address` | 必填整数 | 0～2^64−1；满足对齐、4 KiB 和窗口规则 |
| `id` | 整数，默认 1 | 1～1023 |
| `size` | 整数，默认 5 | 0～5；这里是 AxSIZE，不是字节数 |
| `beats` | 写默认 `len(data)`；读默认 1 | 1～256；这里是实际拍数，不是 AxLEN |
| `data` | 写必填字符串数组 | 一拍一个字符串，每个恰好 `2*(1<<size)` 个十六进制字符；不带 `0x` |
| `strobe` | 写可选整数数组，默认每拍全 1 | 数量等于 beats；每项范围 `0..(1<<(1<<size))-1`；掩码针对拍内字节 |
| `expected` | 读可选字符串数组 | 格式同 data；这是检查器的期望值，不输入存储模型 |
| `expected_response` | 整数，默认 0 | 0～3；检查器的预期响应，不改变存储侧处理 |

写事务不允许 `expected`，读事务不允许 `data` 或 `strobe`。
无论是否提供 `expected`，所有 OKAY 读返回都会与独立的字节级参考模型比较。
参考模型从零开始，仅在成功写响应后按掩码更新。
使用 `expected_response: 3` 可以测试合法的窗口外访问，但不能让结构非法的请求绕过检查。
解析器接受预期响应 1 不代表支持独占访问；当前公开接口没有 EXOKAY 场景。

### 3.2 config 字段、默认值和单位

以下为 Python 文件入口的允许范围；范围检查通过后，具体内存配置还必须被原生 mem_sim 接受。

| 字段 | 默认值 | 允许范围／含义 |
|---|---|---|
| `base` | 2415919104 (`0x90000000`) | 0～2^64−1，AXI 窗口首地址 |
| `size` | 805306368 (`0x30000000`) | 1～2^64−1 字节，且 `base+size <= 2^64−1`；默认窗口 768 MiB |
| `planes` | 2 | 1～4，AoU 资源平面数 |
| `replay` | false | 布尔值；true 启用链路错误注入以验证重放 |
| `standard` | `hbm4` | `hbm3`、`hbm4`、`lpddr5`、`lpddr6` |
| `slots` | 8 | 1～1023，内存桥接可持有的 burst 槽位数 |
| `channels` | 2 | 1～64，传给原生内存配置的通道数 |
| `scale` | 1 | 1～1024，内存时序缩放；不改变 AXI 时钟周期 |
| `queue` | 4 | 1～1024，原生内存提交队列配置 |
| `response_hold` | 0 | 0～1000000，原生访存完成后额外等待的内存 step 周期数 |
| `period_fs` | 2000000 | 2～10^9，必须为偶数；AXI 时钟周期，默认 2 ns |
| `max_ticks` | 200000000000 | 1～10^15，仿真绝对时间 watchdog 上限，单位 fs；默认 200 μs |
| `response_stall_cycles` | 3 | 0～100000，文件主设备观察到 BVALID/RVALID 后额外等待的 AXI 周期数 |

`response_stall_cycles` 不等于日志中每次响应的实际停顿计数。文件主设备在观察到 VALID 后
再改变 READY，采样边沿还会贡献停顿；本次配置为 3，实际每次 B/R 有 4 个 `VALID && !READY`
采样沿。即使设为 0，文件驱动的接收时序仍可能产生停顿。

`max_ticks` 包含启动复位和训练时间，不是每笔事务单独的超时。仿真按步长推进，
检查点可能略晚于上限；它用于防止仿真一直等待，不能作为精确的完成时刻约束。
命令行 `--scale 4` 覆盖 JSON 中的 scale，`--replay` 将 replay 设为 true。
短 SMOKE 不使用重放；需要观察到实际重放事件时用完整验收中的 replay 场景。

### 3.3 C++ 集成时的配置对应关系

`StorageConfig` 定义在 [storage_config.hh](../storage_axi/storage_config.hh)。
`base/size/planes/replay` 与 JSON 同名；`standard/slots/channels/scale/queue/response_hold`
分别对应 `memsim_standard/memsim_slots/memsim_channels/memsim_scale/memsim_queue/memsim_response_hold`。
此外 `trace_dir` 必须设置为已创建、可写的结果目录；`memory_backend` 当前必须为 `memsim`。

`period_fs`、`max_ticks`、`response_stall_cycles` 是文件测试程序的参数，不是 `StorageConfig`
字段；处理器集成由调用方提供时钟、超时和 READY 策略。Python 入口的完整范围检查不会自动
应用到自定义 C++ 主设备，集成方必须遵守同一接口契约及原生内存限制。

生命周期顺序如下：

1. 构造任何 SystemC 模块之前设置 `sc_set_time_resolution(1, SC_FS)`。
2. 创建结果目录、`Signals`、时钟、主设备与 `AouBackend`，绑定端口并注册波形。
3. 保持 `resetn=false` 至少 3 个 AXI 周期，同时推进仿真等待 `ready()` 表示链路就绪。
4. 在非采样沿解除复位，然后发起 AW/W/AR，按 B/R 握手回收事务。
5. 全部 B/R 完成后调用 `finish(directory)`，保存统计和内存镜像并关闭波形。

文件驱动实际先复位 3.5 周期，再按周期等待训练。独立程序链接仓库内的一套 SystemC；
gem5 则用其自身的 SystemC 编译公共源码，同一进程不能同时加载两套内核。

## 4. 本次 SMOKE 的输入与运行

固定输入为 [examples/smoke.json](../examples/smoke.json)，固定入口为
[scripts/smoke.py](../scripts/smoke.py)。它复用现有文件驱动，不需要任一处理器项目。

```bash
# 在 axi_StorageStacked 仓库根目录执行
./run.sh build
./run.sh smoke
# 或指定尚不存在的目录：
./run.sh smoke --output results/my-smoke
```

`smoke` 只接受可选 `--output`，确保验收向量和覆盖要求固定。需要编辑事务或配置时，
复制 JSON 后用 `./run.sh run --input <文件> --output <新目录>`；通用 run 不会生成 SMOKE 专属摘要。
SMOKE 要求使用正常的 Python 断言模式，禁止 `PYTHONOPTIMIZE` 或 `python -O` 关闭检查。

| 顺序／ID | AXI 操作 | 输入与目的 |
|---:|---|---|
| 1 | 从 `0x90000000` 读 2×32 字节 | 显式期望 64 个零字节，检查新进程的初始存储状态 |
| 2 | 向相同地址写 2×32 字节 | 第一拍 `00..1f`，第二拍 `20..3f`，全字节使能 |
| 3 | 从相同地址读 2×32 字节 | 显式期望刚写入的 64 字节，检查多拍与跨 32 字节边界 |
| 4 | 向 `0x9000001c` 写 1×4 字节 | `aabbccdd`、拍内 strobe=5，只更新第 28、30 字节 |
| 5 | 从 `0x90000000` 读 2×32 字节 | 检查掩码生效，且其余 62 字节（包括整个第二拍）保留 |
| 6 | 从 `0x9000001c` 读 1×4 字节 | 显式期望 `aa1dcc1f`，检查窄拍返回的总线字节位置 |

所有事务都位于默认窗口内，预期响应均为 OKAY。两笔写、四笔读会产生
AW=2、W=3、B=2、AR=4、R=7 次握手。每次 B/R 都主动施加反压。
本次 SMOKE 对源码入口、验收流程和仿真内存产生的具体变化见
[OUTPUTS.md 的改动说明](OUTPUTS.md#6-smoke-对项目做了哪些更改)。

## 5. 常问的格式问题

### --input 能不能直接填五通道信号？

当前不能。`--input` 读取事务 JSON 的文件路径；`FileMaster` 再生成真实五通道握手。
逐周期 CSV/VCD 回放尚无解析入口。上游已有信号时，通过 SystemC `Signals/MasterPorts`
绑定公共端口，按 [INTERFACE.md](../INTERFACE.md) 处理时钟、复位和握手。

### 两个 size 为什么不同？

`config.size` 是整个存储窗口的字节数；`transactions[i].size` 是每拍大小的编码。
例如窗口可以是 805306368 字节，一次请求仍可以每拍只读 4 字节：

| 事务 size | 每拍字节数 | 每拍 data 字符数 | 全写时 strobe |
|---:|---:|---:|---:|
| 0 | 1 | 2 | 1 |
| 1 | 2 | 4 | 3 |
| 2 | 4 | 8 | 15 |
| 3 | 8 | 16 | 255 |
| 4 | 16 | 32 | 65535 |
| 5 | 32 | 64 | 4294967295 |

每个十六进制字符表示 4 bit，所以两字符表示一字节。
两拍写请求的 `data` 数组有两个字符串，`strobe` 若提供也要有两个整数。

### 与处理器项目的配置可以直接互换吗？

不可以。AXI 独立输入顶层是 `config/transactions`；
处理器项目则是 `program`、设备、`axi`、`ucie`、`memsim` 等组。
AXI 独立入口可以省略 `config` 的默认字段；两个计算项目要求提供完整配置。
AXI 的 `--input` 相对调用命令时的目录，计算项目的 `--config` 相对选中的 `user/项目`。

完整入门例子见 [环境配置与独立负载入门](01-环境配置与独立负载入门.md)。
