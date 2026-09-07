# AXI2FLIT 技术路线与设计 Spec

> 项目：定制堆叠存储器链路性能建模和协议桥接（2026ZTE06-01）
> 协议基线：**AXI over UCIe Protocol Specification v0.8（doc/ 下的 PDF）**
> 最近更新：2026-09-07（第一阶段 SystemC 模型收口）

---

## 0. 本文档的定位与两条已定案的结论

本文是项目的**顶层技术路线**，回答"做什么、怎么分步做、指标怎么算"。
模块内部的实现细节见 `systemc/doc/design.md`，验证方法与实测数据见
`systemc/doc/verification.md`。三份文档不重复，有冲突以本文的结论条为准。

以下两条在 2026-09-07 定案，后续不再讨论：

**结论一：协议基线是 v0.8 PDF，不是参考 RTL。**
Tenstorrent/BOS 的 AoU RTL 早于 v0.8。v0.8 changelog 明确写着
"Repurposed PROF/PROFEXTLEN fields as FLEX fields, defined use of FLEX fields
as USER bits in the Basic Profile" —— 也就是说 RTL 里的 PROF/PROFEXTLEN 布局
在 v0.8 已经作废。凡是 PDF 与 RTL 冲突的地方，一律以 PDF 为准。
RTL 仅作**架构与参数（如各消息的 granule 数）的旁证**使用，不抄代码。

**结论二：带宽指标按口径B 考核。**
口径B = 应用字节 ÷（数据方向 Flit 数 × 240B），即"我们能支配的 240B 协议载荷，
装了多少有效应用数据"。选它的原因见 §9.2。三个口径在报告里都会打印，
但**只有口径B 参与门限判定**。

---

## 1. 目标

在存储芯片堆叠场景中，用基于 UCIe 流式 Flit 的传输机制，实现 AXI 事务到存储
原子化读写命令的低延迟映射，交付一个高效协议桥接单元（Protocol Bridge）。

**关键指标（来自指标要求）与当前达成情况**

| 指标 | 要求 | 当前实测（1024b 配置） | 状态 |
|---|---|---|---|
| 单通道传输速率 | ≥ 24GT/s | 建模按 UCIe x16 @24GT/s = 48GB/s 裸带宽 | 由链路侧保证 |
| 全链路带宽利用率 | ≥ 90% | 口径B 读 94.79% / 写 94.22% | **达标** |
| 协议桥接引入延迟 | ≤ 20ns | 读 TX 7.80ns + RX 4.00ns；写 TX 4.20ns + RX 4.00ns | **达标** |

> 延迟口径说明：表中是**桥接单元自身**引入的时延（AXI 握手 → Flit 上链路，
> 以及入站 Flit → AXI 响应握手），不含链路飞行时间和对端存储访问时间 ——
> 这两项不由本单元决定。空载单笔读事务的往返（含零飞行）为 10.00 ns。

---

## 2. 总体技术路线

参考 AoU 协议，在 UCIe 链路之上搭建 AXI 桥接。分五步推进，先验证后实现：

```
┌────────────────────────────────────────────────────────────┐
│ ①SystemC 模型        验证功能/打包策略/流控，量化带宽与延迟  │ ← 当前阶段（已完成）
│ ②Cycle-Approximate   加入打包时序，仿真指标是否达标          │ ← 已在①中一并完成
│ ③Cycle-Accurate      对齐时序，作为 RTL 前仿参考             │
│ ④RTL 实现            28nm 工艺，达成时序和面积目标           │
│ ⑤前仿/后仿验证       与 SystemC 参考模型对拍，输出报告       │
└────────────────────────────────────────────────────────────┘
```

阶段①的实际产出已经越过了纯 TLM 的范围：模型带 500MHz 时钟、逐拍的
credit 收发、按 credit 环路反推的 FIFO 深度、可配置的链路飞行时间，
因此②的"加入打包时序"是在①内部完成的，不再单列一步。

先做 SystemC 的核心理由：
- 快速量化打包策略对利用率和延迟的影响（本阶段已用它否掉了"攒满才发"）
- 建模前验证 per-message-type credit 流控，提前发现死锁与深度不足
- 系统级仿真模型本身就是合同交付物

---

## 3. 架构与模块划分

```
AXI Master                    Protocol Bridge                   UCIe Link
AW ─┐                    ┌──────────────┐  ┌─────────────┐
W  ─┤                    │  MsgBuilder  │→ │ FlitPacker  │→ 256B Flit → FDI → 对端
AR ─┼───► Axi2Flit ─────►│ (消息构造)   │  │48×5B granule│
   │      (五通道线程)   └──────────────┘  └─────────────┘
R  ─┤                    ┌──────────────┐  ┌─────────────┐
B  ─┘◄─────────────────  │  MsgDecoder  │← │FlitUnpacker │← 入站 Flit ← FDI ← 对端
                         │ (消息解析)   │  │ credit 上报 │
                         └──────────────┘  └─────────────┘
```

**发送路径（Axi2Flit → MsgBuilder → FlitPacker）**
- 通道仲裁：AW/W/AR 独立线程与队列，按固定优先级 + RP 轮转选择
- 消息构造：AXI 信号 → AoU 消息（WriteReq / ReadReq / WriteData / WriteDataFull）
- Flit 打包：消息填进 240B 载荷（48×5B granule），生成 10B Protocol Header
- 流控：按消息类型独立核算 credit（WREQ/WDATA/RREQ/RDATA/WRESP）；Misc 不消耗 credit

**接收路径（FlitUnpacker → MsgDecoder → Axi2Flit）**
- 解析 Protocol Header（FDId / MsgStart[47:0] / MsgCredit[15:0]）
- 用 MsgStart 位图在 48 granule 中切分消息边界，并重组跨 Flit 续传的消息
- 消息 → AXI 通道信号还原，R/B 通道回送；释放缓冲后归还 credit

---

## 4. Flit 结构（AoU 关键字段）

单 flit = 240B 协议载荷 + 10B 协议头，嵌入 UCIe 256B Latency-Optimized
Flit Format 6（另有 6B 链路层开销，256 = 6 + 10 + 240）。

| 字段 | 大小 | 说明 |
|---|---|---|
| FDId[1:0] | 2bit | Flit 目的 ID（多桥共享 FDI 用），单桥固定为 00 |
| MsgStart[47:0] | 48bit | 位图，标记 48 个 granule 中哪些是新消息起点 |
| MsgCredit[15:0] | 16bit | 随 flit 捎带分发的 credit |
| 载荷 | 48×5B | 多条消息连续打包；消息可跨 flit 续传 |

**位序规则（v0.8 §5.8）**：`Bytes count up · Bits count down · MSB-first`。
整条序列化链路按 MSB-first 实现。这一条极容易搞错而又不容易测出来
（builder 与 decoder 只要自洽，一个系统性的位域错误可以让整条链路全绿），
因此专门做了黄金字节向量对拍，见 §10。

---

## 5. 消息映射与粒度（按 v0.8 核定）

| AXI 通道 | AoU 消息 | 256b | 512b | 1024b |
|---|---|---|---|---|
| AW | WriteReq | 3 granule = 15B | 同左 | 同左 |
| AR | ReadReq | 3 granule = 15B | 同左 | 同左 |
| B | WriteResp | 1 granule = 5B | 同左 | 同左 |
| W | WriteData（带 WSTRB） | 8 | 15 | 30 |
| W | WriteDataFull（全 strobe，省 WSTRB） | 7 | 14 | 27 |
| R | ReadData（带 RLAST） | 8 | 14 | 27 |
| 流控 | Misc / Activation | 1 granule = 5B | 同左 | 同左 |
| 流控 | Misc / CrdtGrant | 2 granule = 10B | 同左 | 同左 |

> **这三张数据消息的粒度表互不相同，任何时候都不能互相代用。**
> WriteData 比 WriteDataFull 多的 1/1/3 granule 就是 WSTRB 字段；
> ReadData 与 WriteDataFull 在 512b/1024b 下恰好相同（14/27），
> 但在 256b 下不同（8 vs 7），不要因为"看起来一样"就合并。

**效率策略：优先采用 1024b 数据粒度 + WriteDataFull。**
1024b ReadData 一条 27 granule = 135B 中承载 128B 应用数据，是三种位宽里
application-data 占比最高的；写方向用 WriteDataFull 省掉 WSTRB，
把 30 granule 降到 27。这两条是达到 90% 利用率的主要途径，实测口径B
读 94.79% / 写 94.22%。

---

## 6. 发送触发策略

- **有消息就发**：默认 `flush_timeout_cycles = 0`，本拍没有更多可打的消息
  就立刻把当前 Flit 发出去，时延最优。
- **攒包超时可配**：`flush_timeout_cycles > 0` 时允许多等几拍攒包。
- 实测结论：**1024b 满载场景下不需要攒包**。因为一条 1024b 数据消息就占
  27 granule，两条 54 granule 已经超过一个 Flit 的 48 granule，配合跨 Flit
  续传，链路占用率天然就是 100%，攒包只会平白增加延迟。
  这一条推翻了立项时"必须靠水位触发才能填满 Flit"的预判。
- **跨 Flit 续传已实现**（收发双侧）：消息在 Flit 尾部被截断时记录续传状态，
  下一个 Flit 从 G0 接着放且不置 MsgStart[0]；接收侧据此重组。
  这是"不靠攒包也能填满"的前提。

---

## 7. 关键设计决策

| 决策点 | 方向 | 理由 |
|---|---|---|
| Flit Format | Format 6（Latency-Optimized） | 降低解码延迟与开销 |
| 数据粒度 | 1024b 为主 + WriteDataFull | 满足 90% 带宽利用率 |
| 流控 | per-message-type credit，多 Resource Plane | 独立通道流控，防互阻塞 |
| 缓冲深度 | 按 **credit 环路**反推，不是按链路 TAT | 见 §7.2，这是本阶段最关键的一处修正 |
| 消息调度 | 有消息即发（超时可配） | 见 §6，1024b 下不需要攒包 |
| 跨 RP ordering | 写成集成约束 + 运行时断言，**不做重排序缓冲** | 见 §7.3 |

### 7.1 Resource Plane 工程配置

- `RP_COUNT` 是构造参数，取值 1～4（AoU 的 RP 字段是 2bit），默认 1。
- RP 用于隔离多端口或 QoS 流量，**不用于区分读写消息**；单一 HBM 交互场景
  通常只需 RP0。即使只启用一个 RP，五种消息类型的 credit 仍必须独立计数。
- 多 RP 模式下按 `AxQOS % RP_COUNT` 映射，同一消息类型内按 RP 轮询调度。

### 7.2 Credit 与缓冲区约束

- 1 credit = 指定 `[RP][消息类型]` 的 1 个 5B granule 接收空间。
- 发送端在一条消息开始时**一次性扣除整条消息的 credit**。
- 接收端初始 credit 与可接收 granule 容量一致，AXI R/B 握手完成、
  释放缓冲后才归还。
- 优先通过业务 Flit 的 `MsgCredit` 捎带归还；无业务 Flit 时发 dedicated
  `CrdtGrant`（2 granule），避免死锁。

> **深度必须按 credit 环路算，不能按链路 TAT 算。** credit 从"接收侧腾出空间"
> 到"发送侧看到它"，除了链路往返 40ns，还要加上：接收流水 2 拍、credit 编码
> 分档的余数等待 1~2 个 Flit 周期、捎带或专用 CrdtGrant 再占 1 个 Flit 周期。
> 合计 `CREDIT_LOOP_NS = 40 + 3×5.333 ≈ 56 ns`，在飞字节数 = 56×48 = 2688B。
> 按 40ns 算出来的深度会让吞吐在真实 TAT 下掉到 90.10%；按 56ns 算是 100.00%
> （PERF-5 实测）。**这条结论要原样带进 RTL**，否则 RTL 会重犯同一个错误。

### 7.3 集成约束 C-1（RP_COUNT > 1 时必须满足）

**同一个 AXI ID 在同一方向上未完成的事务，必须全部映射到同一个 RP。**

AXI4 要求同 ID 同方向的响应保序，而 AoU 的多个 RP 是相互独立的流控平面
（各有 credit、各有 FIFO、发送侧 round-robin），两笔落在不同 RP 上的事务
响应顺序不确定。按 `AxQOS % RP_COUNT` 分流时，如果 SoC 对同一 AxID 发出了
不同的 AxQOS，这条约束就被破坏。

充分条件（任一即可）：`RP_COUNT = 1`；或同一 AxID 只用一个 AxQOS；
或按 ID 空间静态划分（如 `AxID[9:8]` 直接当 RP 号）。

模型侧**不做跨 RP 重排序缓冲**——重排序缓冲面积随 outstanding 深度增长，
且会把快 RP 拖到慢 RP 的节奏上，反而抵消分平面的意义，AoU 规范本身也没有
要求桥接单元承担这件事。改为加运行时断言，违例计数由
`Axi2Flit::order_violations()` 读出，TC9 考核其为 0。
**约束本身要由 SoC 的 ID/QoS 规划保证，桥接单元只能检出、不能修复。**

---

## 8. SystemC 当前实现边界

**已实现**
- AW/W/AR 打包、R/B 解包、FDI 双向 ready/valid、credit 耗尽与回填
- 1～4 RP 参数化；AXI 数据位宽 256/512/1024 三档参数化（`AXI_DATA_WIDTH_CFG`）
- **跨 Flit 消息续传**（收发双侧）
- 按 credit 环路反推的接收 FIFO 深度与初始 credit
- 可配置的链路单向飞行时间与对端处理时延
- 同 ID 跨 RP 顺序违例检测（约束 C-1）

**未实现 / 后续**

| 项 | 状态 |
|---|---|
| responder 侧（入站 WREQ/RREQ/WDATA） | 由 testbench 的 RemoteAouModel 扮演 |
| AXI 合法性负向用例（AxBURST≠INCR / AxSIZE 超位宽 / 跨 4KB / 非对齐） | 未实现，当前假定上游为合法 AXI4 |
| QoS 三模式仲裁 + 防饿死超时 | 待做，当前为固定优先级 + RP 轮转 |
| WLAST 重建 | 随 responder 侧一起做（AoU 的 WriteData 不含 WLAST） |
| ACTIVATE/DEACTIVATE 完整状态机 | 当前在复位释放后直接交换初始 CrdtGrant |
| Aggregator/Splitter、FDI cancel/stall、Early BRESP、CSR | P2，本阶段不做 |
| Flit2DFI | 与存储控制器侧同事合并对齐 |
| 与 FDI 侧 UCIe D2D 链路仿真模型联合测试 | 待对方模型就绪 |

---

## 9. 指标验证方法

### 9.1 分场景测试

| 指标 | 场景 | 测量方法 |
|---|---|---|
| 利用率 ≥ 90% | 高并发恒定流（1024b、多 outstanding） | 口径B：应用字节 ÷（数据方向 Flit 数 × 240B） |
| 延迟 ≤ 20ns | 空载系统单笔小事务 | AXI 握手 → Flit 上链路（TX），入站 Flit → AXI 响应（RX） |

带宽与延迟不矛盾，它们描述的是两个不同的流量状态：满载时看单位时间搬了多少
有效字节，空载时看单笔事务穿过桥接单元要多久。同一套 RTL 在两个状态下分别
成立即可，不需要用一个折衷点同时满足。

### 9.2 带宽口径定案

| 口径 | 定义 | 说明 |
|---|---|---|
| 口径A | 应用字节 ÷（数据方向 Flit 数 × **256B**） | 分母含 6B 链路开销 + 10B 协议头 |
| **口径B** | 应用字节 ÷（数据方向 Flit 数 × **240B**） | **合同考核口径** |
| 口径C | 应用字节 ÷（双向业务粒度 × 5B × 256/240） | 收发合算，含 AXI 请求/响应开销，最保守 |

`口径A = 口径B × 0.9375`（240/256），两者信息等价，只是分母口径不同。

**为什么定口径B：**
1. v0.8 规范 Table 23 自己的效率表就是按口径A 算的，且明确写出 1024b 的
   上限是 **88.9%** —— 也就是说 ≥90% 在口径A 下**物理上不可达**，
   6B 链路开销和 10B 协议头由 UCIe 与 AoU 规范固定，不由本单元决定。
2. 口径B 的分母 240B 恰好是本单元能支配的全部空间，衡量的正是"打包策略好不好"
   这件我们真正负责的事。
3. 换算关系是常数，报告里三个口径全打印，不丢信息，甲方要看哪个都能立刻换算。

**实测（1024b、满载）：读 口径B 94.79%、口径A 88.87%；写 口径B 94.22%、
口径A 88.33%。链路占用率 100%。**

### 9.3 性能硬门限（回归即报警）

性能测试带门限判定，跌破即非零退出，可直接接 CI：

| 位宽 | 读吞吐 | 写吞吐 | 口径B | TX 延迟 | RX 延迟 |
|---|---|---|---|---|---|
| 256b | ≥15.80 GB/s | ≥15.80 GB/s | 不考核 | ≤12ns | ≤8ns |
| 512b | ≥31.50 GB/s | ≥31.50 GB/s | 不考核 | ≤12ns | ≤8ns |
| **1024b** | **≥42.00 GB/s** | **≥41.50 GB/s** | **≥93%** | ≤12ns | ≤8ns |

256b/512b 不考核口径B，是因为这两档下瓶颈在 AXI 侧（16 / 32 GB/s，
低于链路的 48 GB/s），链路本来就填不满，考核链路载荷效率没有意义。
只有 1024b 是 link-bound，口径B 才真正反映打包质量。

---

## 10. 验证策略（防"自洽但全错"）

编解码器互为反函数时，一个系统性的位域错误可以让整条链路全绿。因此不能只靠
"MsgBuilder 编码后 MsgDecoder 能解回来"——那是无效验证。两道防线：

1. **黄金字节向量对拍**（`tb/tb_golden_vectors.cpp`）：从 v0.8 PDF 的字段表
   **手工转录**出每类消息的期望线上字节，与模型输出逐字节比对。
   这条路径完全绕开模型自身的编解码代码，是唯一能抓住系统性位序错误的手段。
   当前 3 种位宽各 71 项检查全通过。
2. **可辨识激励**：数据图样用递增 ⊕ 走一位（而不是全 0xAA 这种均匀字节，
   均匀字节会掩盖字节序错误），地址用 `0x0123456789abcdef` 这类每个 nibble
   都不同的模式，并用独立解析器单独校验地址/SIZE 等请求字段。

功能测试 TC1～TC9、性能测试 PERF-1～PERF-5 的详细内容见
`systemc/doc/verification.md`。三套测试（功能 / 黄金向量 / 性能）在
256b / 512b / 1024b 三种位宽下全绿。

---

## 11. 风险与注意

- **规范状态**：AoU v0.8 标注 Under Development。本项目已把基线固化在
  doc/ 下的这一份 PDF；若甲方后续提供更新版本，需重跑黄金向量对拍确认差异。
- **参考 RTL 不能当基线**：见 §0 结论一。RTL 是 Apache-2.0 且标注
  pre-release/evaluation quality，仅作架构与参数旁证，不抄代码。
- **纯"攒满才发"是错误设计**：会导致空载场景滞留。当前实现为有消息即发，
  攒包超时可配但默认为 0。
- **缓冲深度按 40ns TAT 算是错误的**：见 §7.2，必须按 56ns credit 环路算。
  这是本阶段踩过的坑，RTL 阶段要避免重犯。
- **口径不再讨论**：按口径B。见 §0 结论二与 §9.2。
