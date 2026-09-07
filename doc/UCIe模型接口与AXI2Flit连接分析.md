# UCIe 参考模型接口与 AXI2Flit 连接分析

## 1. 文档目的

本文分析 `reference/ucie-model` 的模块结构、输入输出接口、数据格式、流控和参数化能力，并判断它能否与当前 `systemc` 目录下的 AXI2Flit 转接桥连接。

本文只做设计分析，不修改任何设计代码。分析对象为当前工作区中的以下实现：

- UCIe 参考模型：`reference/ucie-model/src/`
- AXI2Flit 转接桥：`systemc/include/`、`systemc/src/`

重点回答四个问题：

1. UCIe 模型对外提供什么接口？
2. 它能否与 AXI2Flit 直接连接？
3. 若不能直接连接，需要增加什么适配层、连接哪些信号？
4. AXI 数据位宽、UCIe Flit 格式、通道深度等参数能否调整？

---

## 2. 结论摘要

### 2.1 总体结论

当前两个模型在 SystemC 层面可以组成联合仿真系统，但**不能直接端口绑定**，并且当前数据格式还存在一个必须先解决的容量矛盾。

主要结论如下：

1. **接口机制不同，需要增加双向适配器。**
   - AXI2Flit 使用 `FlitTransfer` 加 `ready/valid` 信号。
   - UCIe 模型使用四个 `sc_fifo<FdiFlit>` 事务级端口。
   - 因此需要增加一个 `AouFdiTransactor`，完成握手转换、类型转换、序列化和反序列化。

2. **当前 Flit 有效载荷容量不兼容，不能只靠接线解决。**
   - 当前 AXI2Flit 按 AoU Format 6 建模：一个 256B Flit 中包含 6B 链路层开销和 250B 协议层内容。
   - UCIe 模型的 `Standard256` 格式只向 FDI 开放 236B payload，其余 20B 被模型内部的头部、DLLP、保留位和 CRC 使用。
   - 250B 大于 236B，因此当前 AoU Flit 无法完整放入一个 UCIe `Standard256` FDI 事务。

3. **推荐为 UCIe 模型增加明确的 AoU Format 6 格式，而不是拆成两个 Flit。**
   - 后续应在 UCIe 模型中新增一种格式，例如 `AouFormat6`，使 FDI 协议层容量为 250B，并按双方确认的 UCIe/AoU 位级格式构造 256B 物理 Flit。
   - 不建议把一个 AoU Flit拆成两个现有 UCIe Flit，因为这会改变带宽、时延、重放和信用语义，联调结果不能代表目标协议。

4. **参考模型自带的 `MemoryResponder` 不能充当真正的存储侧协议终端。**
   - 它只把请求 payload 回送，适合 UCIe 链路自测。
   - 完整系统需要在内存侧接入 AoU 解包/响应模块，例如后续的 Flit2DFI、HBM 控制器模型或专用 AoU target。

5. **数据位宽可以调整，但必须区分三个不同层次。**
   - AXI beat 位宽：当前支持编译期选择 256/512/1024 bit。
   - AoU Format 6：协议层 Flit 容量当前固定为 250B，其中 AoU payload 为 240B。
   - UCIe 参考模型 FDI payload：当前只能由格式选择为 236B 或 64B，不能任意配置成 250B。

因此，推荐的工作顺序是：先冻结 AoU Format 6 的位级序列化规则，再扩展 UCIe 模型的对应格式，最后实现 FIFO/ready-valid 适配器和端到端测试。

---

## 3. `reference/ucie-model` 的模块划分

### 3.1 文件与职责

| 文件 | 主要职责 |
|---|---|
| `ucie_common.h` | Flit 格式、链路参数、统计信息、CRC、公共类型和参数校验 |
| `ucie_fdi.h` | 对外 FDI 事务类型 `FdiFlit` 以及测试流量类型 |
| `ucie_phy.h` | PHY 行为模型，包括发送、信道、接收、误码和时延 |
| `ucie_link.h` | Tx/Rx Adapter、ACK/NAK、重放、双向链路和顶层 `UcieLink` |
| `ucie_systemc_main.cpp` | 独立仿真入口、流量源、回环响应器、结果输出和命令行解析 |
| `ucie_unit_tests.cpp` | CRC、格式、队列、重放、参数等单元测试 |

### 3.2 链路内部结构

每个方向都由如下路径组成：

```text
FDI 输入 FIFO
    │
    ▼
TxAdapter：组帧、序号、CRC、重放缓存
    │
    ▼
PHY：序列化时延、信道时延、误码/抖动/偏斜
    │
    ▼
RxAdapter：CRC、顺序检查、ACK/NAK、去重
    │
    ▼
FDI 输出 FIFO
```

模型内部有两条相反方向的业务链路，并分别配有反馈路径。ACK、NAK 和重放属于 UCIe 模型内部机制，不需要由 AXI2Flit 直接处理。

### 3.3 顶层方向关系

`UcieLink` 对外同时暴露 SoC 侧和 Memory 侧：

```text
SoC 侧发送：soc_tx_in  ──► 正向 UCIe 链路 ──► mem_rx_out：Memory 侧接收
SoC 侧接收：soc_rx_out ◄── 反向 UCIe 链路 ◄── mem_tx_in ：Memory 侧发送
```

这意味着 `UcieLink` 本身已经是一个完整的双向链路模型，而不是只表示单向 PHY。

---

## 4. UCIe 顶层输入输出接口

### 4.1 `UcieLink` 公共端口

`reference/ucie-model/src/ucie_link.h` 中的顶层端口如下：

```cpp
sc_fifo_in<FdiFlit>  soc_tx_in;
sc_fifo_out<FdiFlit> soc_rx_out;
sc_fifo_in<FdiFlit>  mem_tx_in;
sc_fifo_out<FdiFlit> mem_rx_out;
sc_out<unsigned>     link_state;
```

端口含义如下：

| 端口 | 相对 `UcieLink` 的方向 | 业务含义 | AXI2Flit 联调用途 |
|---|---:|---|---|
| `soc_tx_in` | 输入 | SoC 发往 Memory 的 FDI Flit | 接收 AXI2Flit 打包后的请求、写数据及相关信用信息 |
| `mem_rx_out` | 输出 | Memory 收到的 FDI Flit | 连接存储侧 AoU 解包器或 target |
| `mem_tx_in` | 输入 | Memory 发往 SoC 的 FDI Flit | 接收存储侧生成的读数据、写响应及相关信用信息 |
| `soc_rx_out` | 输出 | SoC 收到的 FDI Flit | 送入 AXI2Flit 的解包路径，恢复 AXI R/B 通道 |
| `link_state` | 输出 | 链路状态 | 用于启动门控、状态监控和错误报告 |

### 4.2 `FdiFlit` 的字段

`reference/ucie-model/src/ucie_fdi.h` 中的 `FdiFlit` 是一个事务级对象：

```cpp
struct FdiFlit {
    std::vector<std::uint8_t> payload;
    std::size_t valid_bytes;
    std::uint64_t transaction_id;
    std::uint8_t vc;
    BusinessKind kind;
    sc_time transaction_start;
    sc_time fdi_time;
};
```

字段建议解释如下：

| 字段 | 作用 | 与 AoU 的建议关系 |
|---|---|---|
| `payload` | 实际进入 UCIe 组帧和 PHY 的字节数组 | 承载序列化后的 AoU 协议层 Flit |
| `valid_bytes` | payload 中有效的字节数 | AoU 固定格式下建议使用固定协议层字节数，而不是 AXI beat 的有效字节数 |
| `transaction_id` | 仿真跟踪标识 | 建议按物理 AoU Flit 单调编号，不要直接等同 AXI ID |
| `vc` | 虚通道元数据 | 单 VC 场景设为 0；不能替代 AoU 的 RP 编号 |
| `kind` | 请求/响应方向标签 | SoC→Memory 设 Request，Memory→SoC 设 Response；不应据此过滤信用信息 |
| `transaction_start` | 事务起始时间 | 由适配器在接收 AoU Flit 时记录 |
| `fdi_time` | 到达 FDI 的时间 | 用于分段统计链路时延 |

需要特别注意：`transaction_id`、`vc`、`kind` 和时间戳在当前模型中主要是仿真元数据。真正经过 PHY 组帧、误码和 CRC 检查的是 `payload`。因此，任何影响协议正确性的字段都必须被序列化进 `payload`，不能只放在元数据中。

### 4.3 时钟与复位

`UcieLink` 没有外部 `clk` 和 `reset` 端口。它通过 SystemC 的 `wait(sc_time)` 表达 UI、串行化、信道和训练时间。

`link_state` 包含以下状态：

- Reset
- Training
- Active
- Degraded
- Failed

集成时应注意：

- AXI2Flit 仍工作在自己的 `clk`/`rst_n` 域中。
- 适配器应等待链路进入 `Active` 后再接受首个发送 Flit。
- 链路进入 `Degraded` 后当前模型仍可能正常交付数据，可继续运行并上报状态；`Failed` 应停止注入新事务。
- 当前 UCIe 顶层没有运行时复位输入。若要验证“传输中复位”，后续需要给模型增加显式复位/重训练机制，或者在新的仿真用例中重新构造模型实例。

### 4.4 接口抽象边界

这里的“FDI”是模型自定义的事务级边界，不是逐拍、逐引脚的硬件 FDI：模型没有暴露 `lp_data/pl_data`、`irdy/trdy` 等信号，也没有对外提供 FDI 时钟。因此：

- 与当前 SystemC AXI2Flit 联调时，使用本文提出的 FIFO transactor 即可；
- 若未来要把该模型接到 RTL FDI 接口，还需要另一层 pin-level transactor，并定义每拍数据宽度、握手、时钟和复位关系；
- 当前模型适合验证 Flit 数据、链路时延、误码和重放，不应被当作硬件 FDI pin timing 的直接替代品。

---

## 5. 当前 AXI2Flit 的外部 Flit 接口

当前 `systemc/include/axi2flit.h` 的协议侧端口为：

```cpp
sc_out<FlitTransfer> flit_out;
sc_in<bool>          flit_ready;

sc_in<FlitTransfer>  flit_in;
sc_out<bool>         flit_in_ready;
```

其含义是：

| AXI2Flit 端口 | 方向 | 含义 |
|---|---:|---|
| `flit_out` | 输出 | AXI2Flit 已打包的 SoC→Memory AoU Flit，`FlitTransfer.valid` 表示有效 |
| `flit_ready` | 输入 | 下游是否可以接收 `flit_out` |
| `flit_in` | 输入 | Memory→SoC 的 AoU Flit，供解包器生成 AXI R/B |
| `flit_in_ready` | 输出 | AXI2Flit 是否可以接收返回 Flit |

`FlitTransfer` 内部携带 `AouFlit`。这不是 `FdiFlit`，也不是字节数组，因此不能将上述端口直接连接到 `UcieLink` 的 `sc_fifo` 端口。

---

## 6. 直接连接的兼容性评估

### 6.1 接口层兼容性

| 检查项 | AXI2Flit | UCIe 模型 | 结论 |
|---|---|---|---|
| SystemC 抽象层 | 模块级行为模型 | 模块级行为模型 | 兼容 |
| 数据类型 | `FlitTransfer/AouFlit` | `FdiFlit/vector<uint8_t>` | 需要转换 |
| 流控接口 | `ready/valid` | 有界 `sc_fifo` | 需要转换 |
| 时序方式 | 显式 `clk/rst_n` | `sc_time` 延时，无外部时钟 | 需要边界处理 |
| 双向传输 | `flit_out`、`flit_in` | SoC/Memory 双向 FIFO | 方向兼容 |
| 链路可靠性 | 不处理 ACK/NAK | 内部处理 ACK/NAK/重放 | 可由 UCIe 模型封装 |
| 物理 Flit 长度 | AoU Format 6，256B | Standard256 或 Compact68 | 名义长度可一致 |
| 协议层容量 | 250B | Standard256 为 236B | **不兼容** |

### 6.2 最关键的数据格式矛盾

当前 AXI2Flit 对 AoU Format 6 的理解是：

```text
256B 总 Flit
├── 6B  UCIe 链路层开销
└── 250B 协议层
    ├── 10B AoU Protocol Header
    └── 240B AoU Payload（48 个 5B granule）
```

当前 UCIe `Standard256` 模型的内部布局是：

```text
256B 总 Flit
├── 2B   模型链路头
├── 236B FDI payload
├── 4B   DLLP
├── 10B  保留区
└── 4B   CRC
```

两者虽然总长度都是 256B，但上层可用容量不同：

```text
AoU 需要：250B
UCIe 当前提供：236B
差额：14B
```

所以，不能把 `AouFlit` 简单序列化后复制进当前 `FdiFlit.payload`。

### 6.3 不推荐的规避方法

以下方法虽然可能让仿真“跑通”，但不适合作为正式集成方案：

1. **将一个 AoU Flit 拆成两个 UCIe Flit。**
   - 会使物理 Flit 数量、有效带宽、时延和重放粒度全部改变。
   - 还需要额外的分片头和重组状态，而这不是当前 AoU Format 6 的定义。

2. **把放不下的 AoU 字段塞进 `FdiFlit` 元数据。**
   - 元数据不经过 PHY、误码和 CRC，无法验证真实链路数据完整性。
   - 联调看似正确，但不能证明协议线上格式正确。

3. **直接截断或减少 AoU payload。**
   - 会破坏 48 个 granule 的打包规则以及消息跨 Flit 关系。

4. **直接使用 Compact68。**
   - Compact68 的 FDI payload 只有 64B，除非另行定义完整的分片协议，否则不能承载当前 AoU Format 6。

### 6.4 推荐的数据格式方案

后续推荐在 UCIe 模型中新增独立格式，例如：

```cpp
enum class FlitFormat {
    Standard256,
    Compact68,
    AouFormat6
};
```

该格式应满足：

- 物理 Flit 总长为 256B；
- 向 FDI/AoU 侧提供完整的 250B 协议层内容；
- 剩余 6B 的位分配、CRC 范围和链路字段以项目采用的 UCIe/AoU 规范版本为准；
- 保留现有 `Standard256` 行为，避免破坏参考模型已有测试；
- 为新格式补充构帧、解析、CRC、错误注入和 golden vector 测试。

在实现该格式之前，应先冻结 `AouFlit` 到 250B 字节流的精确序列化规则，包括字节序、位序、Protocol Header 布局、`MsgStart`、`MsgCredit` 和空闲区域填充值。

---

## 7. 推荐的连接结构

### 7.1 端到端拓扑

```text
                    SoC / AXI 时钟域

AXI Master
    │ AW/W/AR
    ▼
+---------------+
|   AXI2Flit    |
| pack + unpack |
+---------------+
   │ flit_out / flit_ready
   │ flit_in  / flit_in_ready
   ▼
+-----------------------+
|   AouFdiTransactor    |
| ready-valid ⇄ sc_fifo |
| AouFlit ⇄ byte stream |
+-----------------------+
   │ soc_tx FIFO                 ▲ soc_rx FIFO
   ▼                             │
+------------------------------------------------+
|                    UcieLink                    |
|     forward link            reverse link       |
+------------------------------------------------+
   │ mem_rx FIFO                 ▲ mem_tx FIFO
   ▼                             │
+-----------------------+
| Memory-side AoU      |
| target / Flit2DFI    |
+-----------------------+
    │ DFI/HBM command and data
    ▼
HBM/Memory model
```

### 7.2 SoC 侧信号映射

| AXI2Flit | 适配器行为 | UCIe 端口/对象 |
|---|---|---|
| `flit_out.valid` | 与 `flit_ready` 同时为 1 时接收一个 AoU Flit | 向连接 `soc_tx_in` 的 FIFO 写一个 `FdiFlit` |
| `flit_out.flit` | 按 AoU Format 6 序列化为固定字节流 | `FdiFlit.payload` |
| `flit_ready` | 当链路可用且 TX FIFO 有空间时置 1 | 由 FIFO `num_free()`/`nb_write()` 状态产生 |
| `flit_in` | 将收到的字节流反序列化，并保持到握手完成 | 从连接 `soc_rx_out` 的 FIFO 读取 `FdiFlit` |
| `flit_in_ready` | AXI2Flit 解包器接收返回 Flit | 控制适配器是否释放 RX 暂存寄存器 |
| `clk` | 驱动适配器 SoC 侧状态机 | UCIe 本身不接此时钟 |
| `rst_n` | 清空适配器持有的未交付事务 | UCIe 当前没有对应复位端口 |

注意：SystemC 连接时需要在顶层创建 `sc_fifo<FdiFlit>` 通道。`UcieLink.soc_tx_in` 和适配器的 FIFO 输出端口绑定到同一个 TX FIFO；`UcieLink.soc_rx_out` 和适配器的 FIFO 输入端口绑定到同一个 RX FIFO。

### 7.3 Memory 侧信号映射

Memory 侧至少需要一个真正理解 AoU 消息的模块：

| UCIe 端口 | Memory 侧模块动作 |
|---|---|
| `mem_rx_out` | 读取 SoC 发来的 AoU Flit，解析 RREQ/WREQ/WDATA 以及信用字段 |
| `mem_tx_in` | 写入返回的 RDATA/WRESP Flit以及反向信用信息 |

参考模型自带的 `MemoryResponder` 只做 payload 回送。若把它直接接到 AXI2Flit，回送内容仍然是请求/写数据，而 AXI2Flit 的接收侧只期望读数据或写响应，因此无法形成正确的 AXI 事务闭环。

若另一团队提供 Flit2DFI，则推荐将其放置在 `mem_rx_out/mem_tx_in` 与 DFI/HBM 模型之间，而不是使用当前回环响应器。

### 7.4 业务方向不能只按“请求/响应”机械过滤

通常：

- SoC→Memory 方向主要携带 RREQ、WREQ、WDATA；
- Memory→SoC 方向主要携带 RDATA、WRESP。

但 AoU 的信用授予字段会跟随两个方向的 Flit 传输，信用专用 Flit 也可能在没有业务消息时发送。因此：

- `FdiFlit.kind` 可以作为统计用方向标签；
- 不能因为 `kind == Request` 就丢弃其中的响应通道信用；
- 不能因为 `kind == Response` 就丢弃其中的请求/写数据信用；
- 协议判断应以序列化 payload 中的 AoU Header 和消息类型为准。

---

## 8. 适配器应承担的功能

### 8.1 发送方向：AXI2Flit → UCIe

建议流程如下：

1. 等待 `rst_n == 1` 且 `link_state` 已完成训练。
2. 检查 TX FIFO 是否有空位。
3. 仅在 `flit_out.valid && flit_ready` 时接收一个 `AouFlit`。
4. 将完整 AoU 线协议字段序列化为固定长度字节数组。
5. 填写 `FdiFlit` 的跟踪元数据。
6. 使用 `nb_write()` 或等价的受控写入送入 SoC TX FIFO。

推荐的元数据赋值为：

```text
payload          = 完整 AoU 协议层字节流
valid_bytes      = AoU 格式固定协议层长度
transaction_id   = 每个物理 AoU Flit 单调递增的跟踪编号
vc               = 0（当前单 VC 场景）
kind             = Request（SoC→Memory 方向标签）
transaction_start= 接收 AXI2Flit 输出的时间
fdi_time         = 写入 FDI FIFO 的时间
```

`transaction_id` 不宜直接使用 AXI ID，因为一个 AoU Flit 可以同时包含多个 AXI 消息片段，不存在稳定的一一对应关系。

### 8.2 接收方向：UCIe → AXI2Flit

建议流程如下：

1. 适配器内部设置一个单项或多项 RX 暂存队列。
2. 暂存队列有空间时，通过 `nb_read()` 从 SoC RX FIFO 读取 `FdiFlit`。
3. 校验 `valid_bytes` 和格式类型。
4. 把 `payload` 反序列化成 `AouFlit`。
5. 置 `flit_in.valid`，并保持所有字段稳定。
6. 直到 `flit_in.valid && flit_in_ready` 握手后，才释放该暂存项。

UCIe 模型已经在内部完成 CRC、序号、ACK/NAK、去重和重放。适配器只应看到按序且已通过校验的 FDI 数据，不需要把这些链路内部信号暴露给 AXI2Flit。

### 8.3 `used_granules` 的处理

当前 `AouFlit` 中的 `used_granules` 是模型辅助字段，不是明确的线上字段，不能直接作为额外元数据绕过 PHY 传输。

接收端要重建它，应结合以下信息：

- `MsgStart` 位图；
- 每种消息头中自描述的长度；
- 前一 Flit 是否有跨 Flit 延续的消息；
- credit-only Flit 与 continuation-only Flit 的区别。

特别是 `MsgStart` 全 0 时，可能表示没有消息，也可能表示上一条消息仍在本 Flit 中继续。因此反序列化器需要保留跨 Flit 状态。若只在 SystemC 对象中复制 `used_granules`，虽然便于早期调试，却不能作为正式链路验证结果。

---

## 9. 流控如何贯通

### 9.1 外层 ready/valid 与 FIFO 背压

适配器可以把两种流控机制自然衔接：

```text
UCIe retry buffer/FIFO 满
        │
        ▼
soc_tx FIFO 无空位
        │
        ▼
适配器 flit_ready = 0
        │
        ▼
AXI2Flit 保持 flit_out 不变
        │
        ▼
内部 Packer 停止继续提交消息
```

返回方向也类似：当 AXI2Flit 暂时不能接收时，适配器保持 `flit_in`；暂存和 `soc_rx` FIFO 逐步填满，最终把背压传回 UCIe 接收链路。

### 9.2 UCIe FIFO 深度与 AoU credit 的关系

两者不应简单绑定为同一个数值：

- UCIe FIFO 深度是 SystemC 传输边界的缓存容量，单位是 FDI Flit。
- UCIe retry buffer 是链路层未确认 Flit 的保存空间，单位也是 Flit，但服务于 ACK/NAK 重放。
- AoU credit 是协议层各消息通道的接收能力，通常按消息或 granule 表达。

三者共同影响是否发生背压，但语义和单位不同。正确做法是分别参数化，并在验证中检查最坏情况下不会死锁，而不是令它们数值相等。

### 9.3 建议的首轮联调配置

首轮功能联调应使用保守配置：

- SoC TX/RX 公共 FIFO：至少 8～16 个 Flit；
- UCIe retry buffer：不小于链路带宽时延积所需的未确认 Flit 数，并保留模型允许的余量；
- 关闭随机误码、抖动和过量 skew；
- 功能稳定后，再逐步缩小 FIFO、增加反馈时延和注入错误。

具体深度应由目标链路速率、ACK 反馈时延和系统可接受停顿反推，不应在缺少业务负载数据时固定为唯一值。

---

## 10. 参数化能力分析

### 10.1 必须区分的三类“位宽”

| 层次 | 当前取值 | 能否配置 | 影响 |
|---|---|---|---|
| AXI `DATA_WIDTH` | 256/512/1024 bit | 可以，编译期配置 | 每个 AXI W/R beat 的数据量和消息长度 |
| AoU Format 6 协议层 | 250B，其中 payload 240B | 当前固定 | 每个 AoU Flit 的消息承载量和 Header 布局 |
| UCIe FDI payload | Standard256=236B；Compact68=64B | 只能随格式二选一 | UCIe 模型一次 FDI 事务能承载的字节数 |

因此，把 `AXI_DATA_WIDTH_CFG` 从 256 改为 1024，不会把 UCIe 物理 Flit 改成 1024 bit 或 1024B。它只会使一个 AXI beat 被编码为更多 AoU granule，必要时跨多个固定大小的 AoU Flit。

### 10.2 UCIe 模型当前可直接配置的参数

`Config` 当前支持的主要运行时参数包括：

| 参数类别 | 代表参数 | 是否可在不改核心代码时调整 |
|---|---|---:|
| Flit 格式 | `Standard256`、`Compact68` | 是，但只有两种预定义格式 |
| Lane 数 | `num_lanes` | 是 |
| Lane 速率 | `lane_rate_gtps` | 是 |
| 调制方式 | NRZ/PAM4 | 是 |
| 公共 FDI FIFO 建议深度 | `fdi_queue_size` | 是；实际顶层 FIFO仍由集成代码创建 |
| 重放缓存 | `retry_buffer_size` | 是，模型限制最大值 |
| 链路时延 | TX、RX、channel、feedback、training UI | 是 |
| 信道特性 | 误码、抖动、ISI、skew、deskew、CDR | 是 |
| 随机种子/看门狗 | seed、watchdog 等 | 是 |

`Config` 通过常量引用传入 `UcieLink`，所以集成顶层应在模块构造前完成配置，并保证该对象的生命周期覆盖整个仿真。它不适合作为传输过程中随意修改的动态寄存器接口。

### 10.3 当前不能直接配置的内容

以下内容不是普通配置项：

- 将 `Standard256` 的 FDI payload 从 236B 任意改成 250B；
- 任意指定物理 Flit 总字节数；
- 任意改变 Header、DLLP、CRC 的字节位置；
- 在运行中切换 Flit 格式；
- 将 `vc` 数值自动映射为多个 AoU RP；
- 给 `UcieLink` 动态切换外部时钟或复位。

这些修改会影响 `payload_bytes()`、构帧、解析、CRC 覆盖范围、格式校验和测试向量，必须作为一种新的协议格式完整实现，而不能只改一个常量。

### 10.4 Lane 速率配置应与现有性能模型统一

当前 AXI2Flit 的性能估算中，x16、24 GT/s 若按每 UI 传 1 bit 计算，对应约 384 Gbit/s，即 48 GB/s。

UCIe 模型默认使用 PAM4，每 UI 按 2 bit 计算。若仍配置 x16、24 GT/s，则原始带宽为约 768 Gbit/s，即 96 GB/s。两边若直接使用各自默认值，会导致性能结论相差约一倍。

首轮与现有 48 GB/s 假设对齐时，可采用以下任一配置：

- x16、24 GT/s、NRZ；或
- x8、24 GT/s、PAM4。

最终应以项目目标的 UCIe 代际、lane 数和调制方式为准，并在设计文档中只保留一套明确的基准配置。

### 10.5 RP 与 UCIe VC 的关系

当前 AXI2Flit 的 RP 数可以参数化，实际项目只有一个 RP 时可配置为 1。RP 是 AoU 协议层的路由/端点概念，应由 AoU 消息头或协议字段携带。

`FdiFlit.vc` 在当前 UCIe 模型中只是元数据，既没有形成完整的线上 VC 编码，也不会替代 AoU 的 RP 字段。因此：

- 单 RP、单 VC 场景：RP 数设 1，`vc` 设 0；
- 多 RP 场景：仍应按 AoU 规则编码 RP，不能仅把 RP 编号写进 `vc`；
- 若未来确实需要 UCIe 多 VC，还要单独定义 VC 的线上编码、调度和信用隔离。

---

## 11. 建议的集成阶段

### 阶段 1：冻结线协议表示

目标是消除“SystemC 对象正确，但字节流未定义”的风险。

需要确认：

- AoU 250B 协议层字节布局；
- 所有字段的 bit/byte order；
- 消息跨 Flit 的重建规则；
- credit-only Flit 编码；
- 填充字节规则；
- Format 6 的 6B 链路开销与 CRC 定义。

输出物应包括可人工检查的 golden byte vector。

### 阶段 2：扩展 UCIe Format 6

在保留现有两种格式的前提下，增加 AoU Format 6：

- 新格式枚举和参数校验；
- 250B FDI payload；
- 256B 物理 Flit 构建与解析；
- 正确的 CRC 覆盖；
- 格式级单元测试和错误注入测试。

### 阶段 3：实现 `AouFdiTransactor`

实现：

- `ready/valid` 与 `sc_fifo` 转换；
- `AouFlit` 与固定字节流互转；
- link training 门控；
- RX holding register；
- 重置时的本地状态清理；
- 跟踪 ID 和时延统计。

### 阶段 4：接入存储侧 target

用真实的 AoU target/Flit2DFI 替换回环 `MemoryResponder`，形成：

- AW + W → WREQ/WDATA → 写操作 → WRESP → B；
- AR → RREQ → 读操作 → RDATA → R；
- 双向信用闭环。

### 阶段 5：联合验证

依次验证：

1. 单笔读、单笔写；
2. 不同 AXI 数据位宽；
3. burst、非对齐地址、不同 AXI ID；
4. AXI 与 FDI 两侧随机背压；
5. AoU credit 耗尽与恢复；
6. UCIe CRC 错误、NAK 和重放；
7. 大反馈时延下的 retry buffer 边界；
8. 多 RP 参数为 1 和大于 1 的配置；
9. reset/training/failed 状态；
10. 端到端数据和响应顺序 scoreboard。

---

## 12. 联调验收条件

建议以以下条件判断“成功连接”，而不只是仿真进程能够运行：

1. AoU 线格式有明确的 250B golden vector，序列化和反序列化逐字节一致。
2. 一个 AoU Format 6 Flit 对应一个 UCIe 256B 物理 Flit，不使用隐式分片。
3. UCIe 误码发生后，错误 Flit不会进入 AXI2Flit 解包器，重放后的正确 Flit只交付一次。
4. 随机背压下 `valid` 数据保持稳定，无丢失、重复和乱序。
5. AXI ID、burst、地址、数据、strobe 和 response 可由 scoreboard 端到端核对。
6. AoU credit、FDI FIFO 和 UCIe retry buffer 分别达到边界时，不产生死锁或非法溢出。
7. 256/512/1024 bit AXI 配置均能通过功能测试，而 UCIe 物理 Flit 格式保持不变。
8. 性能测试使用统一的 lane、GT/s 和 NRZ/PAM4 假设。

---

## 13. 当前主要风险与待确认项

| 优先级 | 问题 | 影响 | 建议 |
|---:|---|---|---|
| P0 | AoU 需要 250B，而 UCIe 当前只提供 236B | 无法正确一对一传输 | 新增 AoU Format 6 |
| P0 | `AouFlit` 尚需冻结位级序列化规则 | 对象级联调不能证明线协议正确 | 先完成 golden vector |
| P0 | 存储侧当前只有 echo responder | 无法产生合法 R/B 响应 | 接入 Flit2DFI/HBM target |
| P1 | `used_granules` 是本地辅助状态 | 经过真实链路后不能直接恢复 | 按 Header 和跨 Flit 状态重建 |
| P1 | UCIe 无外部运行时 reset | 中途复位可能残留 FIFO/重放状态 | 增加 reset/retrain 或限定测试方式 |
| P1 | 两边默认链路带宽假设不同 | 性能结果可能相差一倍 | 固定统一基准配置 |
| P1 | 错误、背压和信用属于不同层 | 参数配置不当可能造成停顿或误判死锁 | 分层参数化并做边界测试 |
| P2 | `vc` 当前主要是元数据 | 不能验证真实多 VC 行为 | 单 VC 先设 0，后续另行建模 |

---

## 14. 最终建议

从模块方向、双向通路和 SystemC 抽象层看，`reference/ucie-model` 可以作为 AXI2Flit 后端的 UCIe 链路模型。推荐连接关系为：

```text
AXI2Flit.flit_out  → AouFdiTransactor → UcieLink.soc_tx_in
AXI2Flit.flit_in   ← AouFdiTransactor ← UcieLink.soc_rx_out
Memory AoU input   ←                    UcieLink.mem_rx_out
Memory AoU output  →                    UcieLink.mem_tx_in
```

但当前还不能通过简单接线实现可信的联合仿真。正式集成前必须解决两个 P0 项：

1. 为 AoU Format 6 提供完整、可验证的 250B 序列化定义；
2. 让 UCIe 模型支持与之匹配的 256B 物理 Flit格式。

完成这两项后，接口适配器只是明确的工程实现：发送侧将 ready/valid 转成 FIFO 写入，接收侧将 FIFO 读取转成可保持的 ready/valid，并以 `link_state` 控制训练前后的流量。AXI beat 位宽仍可独立配置为 256/512/1024 bit，单 RP 场景将 RP 数设为 1 即可，无需把 RP 与 UCIe `vc` 强行绑定。
