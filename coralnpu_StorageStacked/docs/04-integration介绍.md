# integration 介绍

[返回文档目录](README.md)。

`integration/` 把 CoralNPU 设备接口接到公共 AXI 存储项目，并提供仿真进程入口。
用户通过 `user/run.sh` 启动它，通常只需修改项目 JSON。

```text
integration/
├── CMakeLists.txt
├── config.hh
├── main.cc
├── axi_master.hh
├── axi_master.cc
├── storage.hh
└── storage.cc
```

```mermaid
flowchart LR
    A["user/run.sh：配置与 ELF"] --> B["main.cc：仿真入口"]
    B --> C["storage.cc：组装模块"]
    D["CoralNPU 设备接口"] <-->|请求与完成| E["axi_master.cc"]
    C --> E
    E <-->|AXI256 五通道| F["axi_StorageStacked"]
    F <--> G["AXI2Flit → UCIe → 在线 MEMSIM"]
    C --> H["监控记录与 VCD"]
    B --> I["completion.json"]
```

## 1. CMakeLists.txt：构建运行器

接收公共存储的相对路径 `STORAGE_PATH` 和四个 `LINK_*` 参数。
通过 `add_subdirectory` 使用公共存储源码，关闭公共项目的独立 CLI。
把 `main.cc`、`axi_master.cc`、`storage.cc` 编译为 `coralnpu_sim`，
链接 `StorageStacked::axi` 和 `libcoralnpu-native.so`。
设备库在 `coralnpu/native/BUILD` 中固定定义 `ENABLE_RVV`，
使 `CoreMiniAxiWrapper` 实例化 `VRvvCoreMiniAxi`，包含 RVV 执行后端。
RVV 属于设备编译配置，不是用户 JSON 中的运行开关。
编译出的文件放在 `coralnpu/.cache/build/`，由根目录构建入口和用户运行入口管理。

```mermaid
flowchart LR
    A["公共存储路径 + UCIe 参数"] --> B["CMakeLists.txt"]
    C["三个 .cc + 设备库 + 公共存储库"] --> B
    B --> D["coralnpu_sim"]
```

## 2. config.hh：接入参数

定义 `ConfigParams`，继承公共项目的 `StorageConfig`。
公共部分保存存储窗口、内存和链路参数；新增设备周期、AXI 周期、
watchdog 上限、在途请求数、反压开关和 ELF 路径。
内部时间字段使用 fs，默认设备与 AXI 周期均为 2000000 fs。
用户改 JSON，`main.cc` 负责转换和填充这些字段。

```mermaid
flowchart LR
    A["StorageConfig：公共存储参数"] --> C["ConfigParams"]
    B["设备周期、AXI 周期、ELF、在途数"] --> C
    C --> D["Master + Demo"]
```

## 3. main.cc：读取配置并推进仿真

`sc_main` 接收两个参数：`resolved.json` 和结果目录。
它设置全局 1 fs 时间分辨率，把 JSON 转成 `ConfigParams`，
并检查运行参数与已编译的 UCIe 参数是否一致。
创建 `Demo` 后推进 SystemC，直到设备完成或超时。

输出 `config.json`、`ucie_config.json` 和 `completion.json`。
设备完成且 mailbox 为 `0x600d0000` 才算执行成功；之后还需运行独立校验器，
才能得到用户看到的最终 `summary.json.passed=true`。

```mermaid
flowchart LR
    A["resolved.json + 结果目录"] --> B["转换参数，检查 UCIe 配置"]
    B --> C["创建 Demo，推进 SystemC"]
    C --> D{"设备完成且状态正确？"}
    D -->|是| E["收尾记录，退出 0"]
    D -->|否或超时| F["记录失败，非零退出"]
```

## 4. axi_master.hh：设备主端的数据结构

声明 `Master` 模块及其 AXI 主端端口。
`Txn` 保存一笔请求的地址、16 字节数据、掩码、设备 ID、AXI ID、读写拍计数和时间。
`active` 保存未完成请求，`awq/wq/arq` 管理通道队列，
`order` 保证同一读写方向、同一设备 ID 的完成顺序。
统计字段提供接收数、完成数、最大在途数和容量拒绝次数。

```mermaid
flowchart LR
    A["设备请求"] --> B["Txn"]
    B --> C["active：在途表"]
    B --> D["AW / W / AR 队列"]
    C --> E["order：按设备 ID 保序完成"]
```

## 5. axi_master.cc：请求与响应转换

创建设备、加载 ELF，分别按设备时钟和 AXI 时钟推进。
收到 16 字节对齐的设备请求后，分配空闲 AXI ID（1～1023），
根据 `axi.burst_beats` 发出 INCR burst：`AxLEN=burst_beats-1`，
每拍为 `16/burst_beats` 字节。默认 4 拍，因此 `AxLEN=3`、`AxSIZE=2`。

每一拍的数据按该拍地址放到 32 字节 AXI 总线的对应字节位置，写掩码同步移动。
只有 W 握手成功才推进写拍计数；末拍置 WLAST，空闲时清零。
读侧按 RID 独立收集所有拍，核对 RLAST 并累计响应状态。
写侧等 B 握手、读侧等所有 R 握手后，按设备 ID 顺序调用
`coralnpu_complete`。达到 `outstanding` 上限时停止接收新请求。

`stalls=true` 会错开发送 AW/W，并周期性拉低 BREADY/RREADY，用于验证反压。
这里生成 `npu_requests.csv` 和 `transactions.csv`，记录请求、返回及其时间。

```mermaid
flowchart LR
    A["设备请求：地址、数据、掩码"] --> B["容量检查，分配 AXI ID"]
    B --> C["AW/W 或 AR 握手"] --> D["公共存储处理"]
    D --> E["B/R 返回真实状态与数据"]
    E --> F["按原设备 ID 顺序完成"] --> G["coralnpu_complete"]
```

## 6. storage.hh：仿真模块声明

声明顶层 SystemC 模块 `Demo`。
它持有一组 AXI 信号 `Signals`、AXI 时钟、复位信号、设备主端 `Master`、
公共存储 `AouBackend`、协议监控器 `AxiMonitor` 和 VCD 句柄。
对 `main.cc` 提供完成状态、设备周期数、mailbox 和 `finish()`。

```mermaid
flowchart TD
    A["Demo"] --> B["时钟 + 复位 + Signals"]
    A --> C["Master"]
    A --> D["AouBackend"]
    A --> E["AxiMonitor + VCD"]
```

## 7. storage.cc：连线、复位与收尾

将主端和公共存储绑定到同一组 AXI 信号，并连接时钟和复位。
开始时保持复位 3.5 个 AXI 周期，再等待存储链路 ready 后释放。
同时注册外部 AXI、内部链路和时钟复位波形。

`finish()` 收尾公共存储日志，将主端统计交给监控器，并关闭 VCD；重复调用不会重复收尾。
主要输出包括 `axi_wave.vcd`、`axi_events.csv`、`protocol_summary.json`，
以及公共存储生成的 Flit、MEMSIM、DRAM／DFI 记录。

```mermaid
flowchart LR
    A["构造并连接 Master、存储、监控器"] --> B["保持复位，等待链路 ready"]
    B --> C["释放复位，记录信号活动"]
    C --> D["finish：存储收尾 + 主端统计 + 关闭 VCD"]
```

修改连接行为后，从仓库根目录执行 `./build.sh --test`。
只修改用户 JSON 时，直接运行对应项目即可；UCIe 编译参数变化由入口自动处理。

## 跟着一笔读请求读源码

可以把“桥接”理解成翻译：上游说“读这个地址的若干字节”，桥把它转换成 AXI 信号，
等存储返回后再把字节交回上游。一次调用发起请求，不代表数据已经返回。

CoralNPU 原生接口一次交给桥一个 16 字节对齐的块。例如地址 `0x90000010`，
默认桥发 `ARLEN=3`、`ARSIZE=2`，在四次 R 握手中分别读取总线 lane 16～19、
20～23、24～27、28～31，收齐 16 字节后再按设备 ID 的顺序交回。
原生 RTL 接口仍提交固定 16 字节请求；适配器实现窄拍 INCR 转换，不合并多个原生请求。
AXI 返回已经到达后，可能仍需等待更早的同 ID 请求和下一次设备时钟。

| 到哪个阶段 | 应观察什么 |
|---|---|
| 上游发出请求 | 地址、读写方向、长度、来源 ID |
| 桥正式接收 | 是否还有在途容量、分配了哪个 AXI ID |
| 地址握手 | `ARVALID && ARREADY` 的时钟上升沿 |
| 数据握手 | `RVALID && RREADY`；核对 ID、字节、响应码、末拍 |
| 上游收到完成 | 返回时间与数据；此后才释放相应事务资源 |

“反压”就是接收方暂时忙，要求发送方等一等。
波形上 `VALID=1, READY=0` 表示正在等；`VALID=1, READY=1` 才传走一拍。
AW/W 可以分别等待，写地址握手不表示写操作完成，还要等 B 响应。

## 遇到问题，定位到具体文件

| 问题 | 首先阅读 |
|---|---|
| JSON 参数看似没生效 | `resolved.json` 和本目录的参数填充代码 |
| 请求进了桥，却没有 AXI 地址 | `axi_master.cc` 的容量判断、复位与发送队列 |
| 返回字节位置不对 | `axi_master.cc` 的 lane 和掩码转换 |
| 仿真末尾还有在途请求 | 完成处理、队列释放和 `finish` 的调用顺序 |
| VCD 找不到关键信号 | `storage.cc` 的 trace 注册和波形关闭 |

修改本目录后回根目录重新构建。数值结果和存储链路都应重新核对，
不能用一次编译成功证明时序和返回数据正确。
