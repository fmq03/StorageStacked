# AXI2Flit 接入 UCIe 链路实施指南

## 1. 文档定位

本文用于指导后续将 AXI2Flit 与 `reference/ucie-model` 连接。当前阶段不修改代码；待 AXI2Flit 的协议行为和外部接口冻结后，再按本文分阶段实施。

本文重点记录：

- 接入前必须冻结的 AXI2Flit 内容；
- 推荐的端到端模块结构；
- FIFO 与 ready/valid 两种边界方案；
- AoU Format 6 与 UCIe 256B Flit 的格式适配；
- 双向流控、时延和参数处理；
- 存储侧最小读写模型；
- 实施顺序、测试矩阵和验收条件。

当前模型的详细现状与兼容性分析见 [UCIe模型接口与AXI2Flit连接分析.md](UCIe模型接口与AXI2Flit连接分析.md)。

---

## 2. 目标与非目标

### 2.1 接入目标

接入完成后，应形成以下端到端闭环：

```text
AXI Master
   │ AW/W/AR
   ▼
AXI2Flit
   │ AoU Flit
   ▼
UCIe SoC侧适配与链路模型
   │ 256B UCIe Flit
   ▼
存储侧AoU解包模块
   │ SimpleMemRequest
   ▼
简单Memory/HBM行为模型
   │ SimpleMemResponse
   ▼
存储侧AoU响应打包模块
   │
   └──────── UCIe反向链路 ────────► AXI2Flit ──► AXI R/B
```

系统应支持：

- AXI AW、W、AR 请求转换为 AoU RREQ、WREQ、WDATA；
- UCIe 正向和反向传输；
- 存储侧生成 RDATA、WRESP；
- AXI2Flit 恢复 AXI R、B 响应；
- AoU credit、FIFO 背压以及 UCIe ACK/NAK/replay 同时工作；
- 单 RP 默认配置，并保留多 RP 参数化能力；
- AXI 256/512/1024 bit 数据宽度配置；
- 正常、背压和 UCIe 错误重传场景的端到端验证。

### 2.2 当前阶段非目标

首轮接入不要求：

- 实现引脚级 FDI `irdy/trdy` 时序；
- 实现完整 HBM/DFI 协议；
- 实现 UCIe 多 VC 的线上编码和调度；
- 将 UCIe 模型综合为 RTL；
- 在同一仿真对象中完成任意时刻的热复位或速率切换；
- 用复杂存储控制器替代简单、可预测的行为模型。

当前 UCIe 接口是事务级 `sc_fifo<FdiFlit>`，适合功能、性能和可靠性联合验证，不代表真实 FDI pin timing。

---

## 3. 接入前必须冻结的内容

AXI2Flit 尚未冻结时，不应提前把 UCIe 适配器绑定到其内部辅助字段。至少应先确定以下内容。

### 3.1 AoU 线协议格式

必须明确 `AouFlit` 到 250B 协议层字节流的唯一映射：

- 10B Protocol Header 的逐字节、逐 bit 布局；
- `FDId`、`MsgStart[47:0]`、`MsgCredit[15:0]` 的位置；
- Header 未使用 bit 的固定值；
- 数值字段的字节序和 bit 序；
- 240B payload 的 granule 顺序；
- 消息跨 Flit 延续规则；
- credit-only Flit 的编码；
- 空闲 granule 的填充值；
- 非法 Header、非法消息长度的处理方式。

应为每种消息生成可人工核对的 golden byte vector，不能只比较两个使用相同代码路径生成的 C++ 对象。

### 3.2 外部 Flit 接口

冻结时应选择以下一种边界：

1. 保留当前 `FlitTransfer + ready/valid`；或
2. 改为 `sc_fifo` Flit 事务接口。

该选择只影响边界流控，不应改变 AoU 格式和业务行为。

### 3.3 非线上辅助状态

当前 `AouFlit` 中以下字段不是实际线协议字段：

- `valid`；
- `used_granules`。

后续通过真实字节流传输时：

- FIFO 中存在一个对象或 ready/valid 握手成功，就代表该 Flit 有效；
- `used_granules` 应由 `MsgStart`、消息自描述长度和跨 Flit carry 状态重建；
- 不得把辅助字段放进 `FdiFlit` 元数据以绕过 PHY 和 CRC。

### 3.4 AXI 行为边界

至少应冻结：

- AW 与 W 的关联规则；
- burst 拆分和地址推进；
- WSTRB 编码；
- AXI ID 在请求和响应中的保存方式；
- RLAST、BRESP、RRESP 的产生规则；
- 跨 Flit 消息的续传和重组；
- credit 扣减和归还时机；
- RP 选择策略及单 RP 配置行为。

---

## 4. 目标 Flit 分层

接入统一采用 AoU Latency-Optimized 256B Format 6：

```text
256B UCIe物理Flit
├── 2B   UCIe链路Header
├── 250B 协议层内容
│   ├── 10B AoU Protocol Header
│   └── 240B AoU消息载荷（48 × 5B granule）
└── 4B   CRC
```

由此得到三个不能混淆的容量：

| 层次 | 容量 | 用途 |
|---|---:|---|
| 物理 UCIe Flit | 256B | 决定串行化时间和 raw bandwidth |
| UCIe FDI/AoU 协议层 | 250B | 适配器和 UCIe 模型之间传输的完整协议内容 |
| AoU message payload | 240B | 供 RREQ/WREQ/WDATA/RDATA/WRESP 消息占用 |

固定开销合计为16B：

```text
2B UCIe Header + 4B CRC + 10B AoU Header = 16B
```

因此最大 AoU payload 物理效率为：

```text
240 / 256 = 93.75%
```

---

## 5. UCIe 模型的 Format 6 适配

### 5.1 当前格式的问题

当前 `reference/ucie-model` 的 `Standard256` 布局为：

```text
2B Header + 236B Payload + 4B DLLP + 10B Reserved + 4B CRC
```

其中4B DLLP和10B Reserved 当前全部填0；ACK/NAK 通过模型内部反馈通道传输。这14B在当前行为模型中没有承载有效业务内容，导致 FDI payload 只有236B，不能放入完整的250B AoU协议层。

### 5.2 目标格式

后续应新增独立格式，例如 `AouFormat6`：

```text
[0..1]     2B   Link Header：序号和replay标志
[2..251]   250B AoU协议层字节流
[252..253] 2B   CRC group A
[254..255] 2B   CRC group B
```

现有 CRC 分组可以延续：

- group A 覆盖 `[0..127]`；
- group B 覆盖 `[128..251]`；
- CRC 存放于 `[252..255]`。

新增格式比直接修改 `Standard256` 更安全，因为可以保留参考模型已有测试和基线结果。

### 5.3 Format 6 修改范围

实施时至少检查：

- `FlitFormat` 枚举；
- `flit_bytes()` 和 `payload_bytes()`；
- `build_flit()`；
- `check_flit()`；
- `unpack_fdi()`；
- CLI/配置打印；
- 格式和 CRC 单元测试；
- README、接口文档和验证文档；
- 现有 Standard256/Compact68 回归测试。

不能只把 `payload_bytes()` 从236改成250，而不检查所有偏移、CRC和测试断言。

---

## 6. AXI2Flit 与 UCIe 的边界方案

### 6.1 方案A：保留 ready/valid

若 AXI2Flit 最终保留当前接口：

```cpp
sc_out<FlitTransfer> flit_out;
sc_in<bool>          flit_ready;
sc_in<FlitTransfer>  flit_in;
sc_out<bool>         flit_in_ready;
```

则在 UCIe 侧增加一个双向薄适配器：

```text
AXI2Flit ready/valid
        ⇅
AouFdiTransactor
        ⇅
sc_fifo<FdiFlit>
        ⇅
UcieLink
```

优点：

- 不修改 AXI2Flit 外部接口；
- 与未来 RTL ready/valid 边界较接近；
- 可以明确观察逐拍背压。

代价：

- 需要 TX/RX holding 状态；
- 需要处理 SystemC signal 和 FIFO event 的边界；
- 注册实现可能增加1个时钟周期。

### 6.2 方案B：统一为 Flit FIFO

若 AXI2Flit 尚允许调整外部接口，可以使用：

```cpp
sc_fifo_out<AouWireFlit> flit_tx;
sc_fifo_in<AouWireFlit>  flit_rx;
```

其中一个 FIFO slot 对应一个完整 AoU Flit。FIFO 本身承担：

- 空/满状态；
- 生产者—消费者同步；
- 背压；
- 队列深度。

优点：

- 更符合当前事务级 SystemC 模型；
- 接口和控制逻辑更简单；
- 不再需要独立 `valid` 字段。

代价：

- 不直接表达 RTL ready/valid 时序；
- 若在时钟线程中使用阻塞 `write()`，线程可能在非时钟沿恢复；应优先使用按时钟检查的 `nb_write()`，或者使用独立边界线程；
- 仍需完成 `AouFlit` 与250B字节流的序列化，FIFO不能消除类型和格式转换。

### 6.3 选择建议

| 使用目标 | 推荐方案 |
|---|---|
| 纯 SystemC 功能/性能联合仿真 | 方案B：Flit FIFO |
| 保持当前 AXI2Flit 接口不变 | 方案A：UCIe侧 ready/valid 适配器 |
| 后续需要对齐 RTL FDI 接口 | 方案A，并保留可配置寄存延迟 |

最终选择应在 AXI2Flit 接口冻结时记录到本文。无论选择哪一种，序列化、Format 6 和存储侧协议处理均保持一致。

---

## 7. 推荐顶层模块结构

建议把 UCIe 侧接入逻辑封装为一个模块，避免 AXI2Flit 直接依赖参考模型内部类型：

```text
UcieAouEndpoint
├── AouTxBoundary
│   ├── ready/valid或FIFO接收
│   ├── AouFlit序列化
│   └── FdiFlit封装
├── UcieLink
└── AouRxBoundary
    ├── FdiFlit读取
    ├── AoU字节流反序列化
    └── ready/valid或FIFO发送
```

连接关系如下：

| 上游端口 | UCIe侧动作 | `UcieLink` 端口 |
|---|---|---|
| AXI2Flit `flit_out` 或 TX FIFO | 序列化并封装 `FdiFlit` | `soc_tx_in` |
| AXI2Flit `flit_ready` | 反映 TX FIFO 空间和链路状态 | `soc_tx_in` 对应通道 |
| AXI2Flit `flit_in` 或 RX FIFO | 反序列化返回 Flit | `soc_rx_out` |
| AXI2Flit `flit_in_ready` | 控制 RX holding/FIFO 释放 | `soc_rx_out` 对应通道 |
| 存储侧接收 FIFO | 解包请求和写数据 | `mem_rx_out` |
| 存储侧发送 FIFO | 注入读数据、写响应和 credit | `mem_tx_in` |
| 状态监控 | 控制训练前流量并报告故障 | `link_state` |

`UcieLink` 内部 ACK、NAK、CRC、去重和 replay 不应暴露给 AXI2Flit。

---

## 8. 序列化与元数据规则

### 8.1 必须进入250B字节流的内容

以下内容影响协议正确性，必须经过 PHY 和 CRC：

- AoU Protocol Header；
- `FDId`；
- `MsgStart`；
- `MsgCredit`；
- 所有消息 Header；
- 地址、长度、AXI ID、响应码；
- WDATA/RDATA；
- WSTRB或等价 byte enable；
- padding 和保留位。

### 8.2 `FdiFlit` 元数据建议

| 字段 | 建议赋值 | 说明 |
|---|---|---|
| `payload` | 固定250B AoU字节流 | 真正经过PHY的数据 |
| `valid_bytes` | 250 | Format 6固定长度，包括credit-only Flit |
| `transaction_id` | 每个物理Flit单调递增编号 | 只用于跟踪，不等同AXI ID |
| `vc` | 0 | 首轮单VC配置 |
| `kind` | 按物理方向标记 | 不用于过滤AoU信用字段 |
| 时间戳 | 边界接收和写入时间 | 用于延迟分解 |

不能依赖元数据恢复协议字段，因为这些元数据不经过当前 PHY 损伤模型。

### 8.3 `used_granules` 重建

接收侧应按以下顺序推导有效粒度：

1. 若存在上一 Flit 的未完成消息，从 G0 开始读取续传部分；
2. 根据该消息总长度计算续传占用；
3. 根据 `MsgStart` 找到本 Flit 内的新消息；
4. 根据每条消息自描述长度推进到下一消息；
5. 最后一条完整或截断消息结束后，其后区域视为空闲；
6. `MsgStart==0` 且无 carry 时，可判定为没有 payload 消息的 credit-only Flit。

此状态机需要逐 Flit 保存 carry，不能只对单个 Flit 做无状态扫描。

---

## 9. 时钟、训练、复位与适配延迟

### 9.1 时钟关系

- AXI2Flit 使用显式 `clk` 和低有效 `rst_n`；
- UCIe 模型使用 `sc_time` 表达 UI 和链路时延；
- `sc_fifo` 是两者之间的事务级时间解耦边界。

### 9.2 Link training 门控

- `Reset/Training`：不接受新的 AXI2Flit 输出；
- `Active`：允许正常发送和接收；
- `Degraded`：当前模型仍可继续传输，记录告警和统计；
- `Failed`：停止注入新 Flit，并由测试平台结束或报错。

### 9.3 适配器延迟

建议将适配延迟独立参数化：

| 模式 | 行为 | 用途 |
|---|---|---|
| 0周期 | 仅delta cycle/type conversion，不推进`sc_time` | 默认功能和链路性能测试 |
| 1周期 | TX/RX各带一级holding register | 接近硬件边界 |
| N周期 | 显式pipeline延迟 | 研究D2D Adapter时延预算 |

适配器延迟必须与以下延迟分开统计：

- AXI2Flit 打包等待；
- FIFO 排队；
- UCIe TX/RX pipeline；
- PHY 串行化和信道传播；
- replay；
- 存储器响应。

### 9.4 复位限制

当前 UCIe 顶层没有外部 reset 端口。首轮联调应限定为：

1. 仿真开始时 AXI2Flit 保持复位；
2. UCIe 完成训练；
3. AXI2Flit 退出复位并开始发送；
4. 业务期间不执行独立热复位。

若以后需要中途复位，必须同时定义公共 FIFO、retry buffer、序号、carry、credit 和未完成存储请求的清理/重训练规则。

---

## 10. 分层流控

接入后存在三套不同流控，不能把深度简单设置成相同数值。

| 层次 | 单位 | 作用 |
|---|---|---|
| AoU credit | 消息或granule | 保证对端协议接收缓冲有空间 |
| FDI `sc_fifo` | Flit | 解耦模块执行和提供事务级背压 |
| UCIe retry buffer | 未确认Flit | 保存ACK前副本，用于NAK/replay |

背压传播方向为：

```text
UCIe retry/FIFO没有空间
    → UCIe侧停止接收AoU Flit
    → AXI2Flit输出停顿
    → FlitPacker及消息FIFO逐步停顿
    → AXI AW/W/AR ready按内部容量拉低
```

反向链路同理。

FIFO 深度和 retry buffer 深度应分别参数化。具体数值依据：

- 链路带宽时延积；
- ACK反馈时延；
- AXI最大burst；
- 允许的并发事务数；
- 存储侧最坏响应时间；
- credit返回频率。

首轮功能测试可使用较宽松的8～16 Flit公共FIFO，稳定后再做小深度和压力边界验证。

---

## 11. 存储侧最小模型

### 11.1 推荐结构

```text
UcieLink.mem_rx_out
        │
        ▼
MemoryAouUnpacker
        │ SimpleMemRequest
        ▼
SimpleMemoryModel
        │ SimpleMemResponse
        ▼
MemoryAouPacker
        │
        ▼
UcieLink.mem_tx_in
```

这套模块替代参考模型中只做 payload 回送的 `MemoryResponder`。

### 11.2 简单请求接口

建议逻辑字段至少包括：

```text
SimpleMemRequest
├── opcode：READ / WRITE
├── address
├── transaction tag / AXI ID
├── length与beat size
├── RP，单RP时固定0
├── write data或数据流引用
├── byte enable / WSTRB
└── last
```

实现可选择：

- 将完整写请求和所有写数据重组后一次性交给 Memory；或
- 请求和写数据分流，以 beat/片段方式送入 Memory。

首轮模型可选择前者以降低接口复杂度，但应限制最大 burst 并明确内部缓存上限。

### 11.3 简单响应接口

```text
SimpleMemResponse
├── type：READ_DATA / WRITE_ACK
├── transaction tag / AXI ID
├── response status
├── read data
└── last
```

MemoryAouPacker 负责将其转换为 RDATA/WRESP 消息，并按照240B payload容量进行分片和打包。

### 11.4 不能省略的状态

即使只做简单读写，也必须正确处理：

- WREQ 与 WDATA 的配对；
- burst 地址、长度和最后一个 beat；
- WSTRB；
- AXI ID/事务标签；
- 跨 Flit 续传；
- RDATA 分片和 WRESP 生成；
- FIFO 背压；
- 接收缓冲释放后的 credit 返还。

单 RP 时可将 RP 固定为0，不需要跨 RP 仲裁，但不能删除线上 RP 字段的编码和合法性检查。

---

## 12. 参数统一

### 12.1 关键参数

| 参数 | 建议默认值 | 参数层次 |
|---|---:|---|
| AXI data width | 256 bit；测试覆盖512/1024 | 编译期AXI配置 |
| RP count | 1 | AXI2Flit/AoU配置 |
| UCIe physical Flit | 256B | Format 6固定 |
| UCIe FDI payload | 250B | Format 6固定 |
| AoU payload | 240B/48 granule | AoU格式固定 |
| FDI FIFO depth | 8～16起步 | 集成顶层运行时配置 |
| retry buffer | 按反馈时延配置 | UCIe运行时配置 |
| adapter latency | 0 cycle | 集成配置 |

### 12.2 Lane、速率和调制

现有 AXI2Flit 性能基准按 x16、24 GT/s、每UI 1bit，即48 GB/s raw bandwidth 计算。UCIe 模型默认 PAM4，每UI 2bit；若使用 x16、24 GT/s，则为96 GB/s。

联调前必须统一配置。与当前48 GB/s基准对齐可使用：

- x16、24 GT/s、NRZ；或
- x8、24 GT/s、PAM4。

最终只保留与项目目标硬件一致的一套默认值，其他配置作为参数扫描用例。

---

## 13. 带宽与延迟统计口径

### 13.1 主要带宽利用率

最终主指标剔除不可避免的16B固定格式开销，以240B AoU payload为满载基准：

```text
AoU payload填充率
= 已占用granule数 /（发送Flit数 × 48）
= 已占用AoU payload字节 /（发送Flit数 × 240B）
```

按时间归一化：

```text
AoU有效带宽利用率
= 实际AoU payload吞吐率
  /（raw bandwidth × 240/256）
```

满载、无空闲、无重传时为100%。

### 13.2 建议同时记录的指标

- 实际 AXI W/R 数据吞吐率；
- AoU payload填充率；
- raw physical bandwidth占用；
- 空Flit和credit-only Flit数量；
- FIFO等待时间；
- credit不足停顿时间；
- retry buffer满事件；
- CRC错误、NAK和replay Flit数量；
- P50/P95/P99端到端读写延迟。

### 13.3 延迟分解

```text
总延迟
= AXI接收与消息构造
+ Flit打包等待
+ 边界适配
+ FDI FIFO排队
+ UCIe链路
+ 存储侧解包
+ Memory服务
+ 返回方向对应延迟
```

统计时必须分别保存这些时间戳，避免把打包等待或存储器延迟误认为 UCIe PHY 延迟。

---

## 14. 分阶段实施计划

### 阶段1：冻结 AXI2Flit

- 完成发送、接收和credit功能；
- 冻结 RP 参数和默认单RP行为；
- 确定外部使用 ready/valid 还是 Flit FIFO；
- 输出 AoU 逐字节 golden vectors；
- 删除对非线上辅助字段的协议依赖。

完成标志：AXI2Flit 独立回环测试能够通过字节级序列化检查。

### 阶段2：增加 UCIe `AouFormat6`

- 提供250B FDI payload；
- 保留2B Header和4B CRC；
- 保留现有 Standard256/Compact68；
- 增加格式、CRC、错误检测和回归测试。

完成标志：随机250B payload 经 UCIe 正常传输和错误重放后逐字节一致。

### 阶段3：实现 SoC 侧边界

- 实现选定的 FIFO 或 ready/valid 接口；
- 实现 AoU 序列化/反序列化；
- 接入 `link_state`；
- 增加可配置适配延迟和统计。

完成标志：AXI2Flit 发出的 AoU Flit 可经过 UCIe 回环并恢复为相同字节流。

### 阶段4：实现存储侧最小模型

- 解包 RREQ/WREQ/WDATA；
- 输出简单读写请求；
- 生成确定性读数据和写响应；
- 打包 RDATA/WRESP；
- 实现双向credit闭环。

完成标志：单笔读写能够完整返回 AXI R/B。

### 阶段5：压力与可靠性验证

- burst和多ID；
- 256/512/1024 bit；
- 随机FIFO/AXI背压；
- credit耗尽和恢复；
- CRC错误、NAK和replay；
- 小FIFO和大反馈时延；
- 单RP及参数化多RP；
- 性能和延迟分解。

完成标志：scoreboard无丢失、重复、乱序和数据错误，且统计守恒。

---

## 15. 测试矩阵

| 类别 | 最小测试内容 | 核心检查 |
|---|---|---|
| 序列化 | 每种消息、Header边界、跨Flit | 250B golden vector |
| UCIe格式 | 250B全0、全1、递增和随机数据 | 长度、偏移、CRC |
| 基本读写 | 单读、单写 | AXI ID、地址、数据、响应 |
| Burst | INCR、多beat、边界长度 | 地址推进、LAST、数据顺序 |
| 数据宽度 | 256/512/1024 bit | granule数与跨Flit行为 |
| 背压 | AXI、FDI、Memory随机停顿 | valid保持、FIFO不溢出 |
| Credit | 初始授予、耗尽、恢复 | 不超发、不死锁、计数守恒 |
| UCIe错误 | payload/Header/CRC翻转 | 错包不交付、仅正确重放一次 |
| Replay压力 | 高反馈延迟、小retry buffer | 顺序、活性、占用上限 |
| RP | RP=1及多RP参数 | 路由和credit隔离 |
| 性能 | 连续读、连续写、混合负载 | 240B口径利用率、延迟分解 |

测试应同时具有：

- directed corner case；
- 固定随机种子的随机测试；
- 独立 scoreboard；
- 关键计数器守恒断言；
- 可重复的性能配置。

---

## 16. 最终验收条件

满足以下条件后，才能认为 AXI2Flit 已成功接入 UCIe：

1. 一个 AoU Format 6 Flit严格对应一个256B UCIe物理Flit。
2. UCIe FDI payload为完整250B，不使用截断、隐式分片或元数据旁路。
3. AoU Header和payload均经过PHY损伤及CRC保护。
4. ready/valid或FIFO背压下无数据丢失、重复和覆盖。
5. CRC错误Flit不进入上层；replay后只交付一次正确数据。
6. 单读、单写、burst、多ID以及三种AXI数据宽度通过端到端scoreboard。
7. WREQ/WDATA配对、WSTRB、RDATA分片、RLAST和WRESP正确。
8. AoU credit、FDI FIFO和UCIe retry buffer分别达到边界时无死锁。
9. 单RP配置为默认且行为正确，多RP保持可参数化。
10. 带宽利用率以240B AoU payload容量归一化，并与raw物理效率分开报告。
11. AXI、适配器、FIFO、UCIe和Memory延迟能够分别统计。
12. 原有 AXI2Flit 与 UCIe 独立回归测试均未退化。

---

## 17. 待 AXI2Flit 冻结时确认的决策表

| 决策项 | 推荐默认值 | 冻结结果 |
|---|---|---|
| 外部Flit接口 | 纯SystemC优先FIFO；保持现接口则用ready/valid | 待定 |
| 边界传输类型 | 固定250B `AouWireFlit` | 待定 |
| 适配器位置 | UCIe侧 `UcieAouEndpoint` | 待定 |
| 默认适配延迟 | 0 cycle | 待定 |
| UCIe格式 | 新增 `AouFormat6` | 待定 |
| Link Header/CRC | 2B + 4B | 待规范确认 |
| 默认RP数 | 1 | 待定 |
| 默认VC | 0 | 待定 |
| 默认链路配置 | 与项目目标统一，当前可先对齐48 GB/s | 待定 |
| Memory请求接口 | `SimpleMemRequest/Response` | 待定 |
| 写请求缓存方式 | 首轮整burst重组，后续可流式化 | 待定 |
| 主带宽利用率 | 240B AoU payload归一化 | 待定 |
| 中途复位 | 首轮不支持，后续单独设计 | 待定 |

AXI2Flit 冻结后，应先填写本表，再开始修改 UCIe 模型。这样可以避免在接入过程中反复更改公共类型、Flit格式和验证口径。
