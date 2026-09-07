# AXI2FLIT SystemC 模型设计

> 对应课题：2026ZTE06-01「定制堆叠存储器链路性能建模和协议桥接单元技术」研究成果2
> 最近更新：2026-09-07（完成 P0-1～P0-7、P1-8、P1-10，补齐延迟/带宽测量）

## 1. 设计范围

本模型是 AoU（AXI over UCIe）initiator 侧的双向协议桥：

```text
发送：AXI AW/W/AR → AoU WriteReq / WriteData(Full) / ReadReq → FDI
接收：FDI → AoU WriteResp / ReadData → AXI B/R
```

已实现：消息打包/解包、跨 Flit 消息续传、per-RP × per-message-type credit 流控、
FDI 双向 ready/valid 背压、AXI 数据位宽 256/512/1024 参数化、按链路 TAT 反推的
缓冲与 credit 深度。

不在本阶段范围：链路激活状态机（Activation/CSR）、responder 侧（入站 WREQ/RREQ/
WDATA）、QoS 三模式仲裁、Flit2DFI（由存储控制器侧同事合并对齐）。

## 2. 模块与数据流

```text
                              Axi2Flit
 ┌──────────────────────────────────────────────────────────────────────┐
 │ AW 线程 ─┐                                                            │
 │ AR 线程 ─┼→ per-RP TX FIFO ─→ FlitPacker ─→ flit_out / flit_ready    │
 │ W  线程 ─┘   (route fifo)         ↑ credit_update                     │
 │                                   ↓ credit_return                     │
 │ flit_in / flit_in_ready ─→ FlitUnpacker ─→ per-RP RX FIFO ─→ B/R 线程 │
 └──────────────────────────────────────────────────────────────────────┘
```

### 2.1 FlitPacker

打包侧是带宽的决定性模块，本版本有四个关键机制：

1. **跨 Flit 续传**（`pack_fragment`）
   一条消息装不进当前 Flit 尾部时，先填满当前 Flit，剩余部分接在下一个 Flit 的
   **G0** 且不置 MsgStart 位。这是 1024b 可用的硬前提：WriteDataFull1024 占 27
   granule，若不许跨包，48 granule 的 Flit 只能装 1 条，填充率立刻掉到 56%。

2. **一拍打多条消息**（`PACK_MSGS_PER_CYCLE = 8`）
   256b 的 WriteDataFull 只有 7 granule，一拍一条要 7 拍才填满一个 Flit，打包器
   自身就会成为瓶颈。8 条 × 7 granule = 56 > 48，保证单拍可填满一个 Flit。

3. **输出寄存器 + 在建 Flit 的两级缓冲**
   输出被链路背压时继续填 `cur_flit_`，链路一取走上一包，下一包同拍就能发出，
   中间无空泡。等价于参考 RTL 的 2-entry TX ring buffer。

4. **flush 策略：无候选即发**（`DEFAULT_FLUSH_TIMEOUT_CYCLES = 0`）
   原先固定攒 4 拍（@500MHz = 8ns）直接加在时延路径上。改为"本拍没有可打的消息
   就立刻发出"，代价是链路空闲时会发半满 Flit —— 但那种情况下链路本来就有余量。

调度：`ReadReq > WriteReq > WriteData`，同优先级内按 RP 轮转。`sc_fifo` 无 peek
接口，因此每个 `[RP][类型]` 设一个 staging slot：先取出队头，再按消息实际 granule
数检查 credit，credit 不足时消息留在 slot 中等待，不阻塞其他 RP。

### 2.2 FlitUnpacker

- 一个 Flit holding register 解耦 FDI 接收与内部 FIFO 时序，`flit_in_ready` 为
  寄存输出（`flit_in_ready = !holding_valid_`）。
- 按 `MsgStart[47:0]` 切分消息。**消息长度由首字节自描述**
  （`message_granules_from_header`），不能靠两个 MsgStart 位之间的距离推断——
  消息被截断到下一个 Flit 时，它后面根本没有第二个 MsgStart 位。
- 维护跨 Flit 续传状态：`MsgStart[0] == 0` 表示本 Flit 以续传片段开头。
- 解析 header 的 `MsgCredit[15:0]` 与 Misc/CrdtGrant，把对端 grant 送入 Packer。
- initiator 侧未实现的入站 WREQ/RREQ/WDATA 按协议异常计数。

### 2.3 AXI 五通道线程

- AW/AR：`AxQOS % RP_COUNT` 映射 RP，去气泡握手，可达 1 beat/cycle。
- W：AXI4 无 WID，通过 AW 顺序路由队列继承 RP，并校验 AWLEN/WLAST 一致性。
  AW 与 W 是**独立线程**，AW 的开销不会串行化进 W 数据流。
- B/R：解码后保持 valid 与数据直到 AXI `valid && ready`；**握手后**才释放对应
  granule credit —— credit 必须代表"接收缓冲真的空出来了"。

## 3. Credit 模型

### 3.1 计数维度

```text
[RP][WREQ | RREQ | WDATA | RDATA | WRESP]
```

1 credit = 该类型的 1 个 5B granule 接收空间。Misc 不消耗 credit。
`WriteData` 与 `WriteDataFull` 共用 WDATA credit pool。

### 3.2 缓冲深度按 credit 环路反推（不是按链路 TAT）

接收 credit 的本质是"我保证还能收下 N 个 granule"。本端把 credit 还给对端后，
数据要经过一整个环路才会到达；这段时间里链路持续灌数据。若接收 FIFO 装不下
`环路时间 × 链路带宽`，credit 会先于链路耗尽 —— 此时测出来的带宽反映的是
FIFO 太小，而不是协议效率。

**关键点：这里要用的"环路时间"不是 `LINK_TAT_NS`。** `LINK_TAT_NS`（40 ns）
只涵盖"credit 已经上了链路"之后的飞行与对端处理。credit 从"接收缓冲腾出来"
到"真的上了链路"，还要额外经过三段，而且每段都以 Flit 为粒度发生：

| 段 | 内容 | 代价 |
|---|---|---|
| ① | 消息离开接收 FIFO、送上 AXI 之后，credit 才变成"待归还" | ≈ 接收流水线 2 拍 |
| ② | 待归还量要攒够一个可编码档位（编码只有 {1,4,8,16,32,64,128} 七档，凑不满的余数只能等下一轮） | ≈ 1～2 个 Flit 周期 |
| ③ | 归还量要搭上一个出站 Flit 的头部，或单独发一条 CrdtGrant | ≈ 1 个 Flit 周期 |

三段合计按 3 个 Flit 周期计入：

```text
CREDIT_LOOP_NS = LINK_TAT_NS(40) + 3 × FLIT_PERIOD_NS(5.333) = 56 ns
在飞字节数 = CREDIT_LOOP_NS(56ns) × LINK_BYTES_PER_NS(48B/ns) = 2688 B
一条 N granule 消息的链路裸字节 = N × 5B × 256/240
```

| AXI 位宽 | ReadData granule | 每条裸字节 | RDATA FIFO 深度/RP | RDATA credit/RP | WRESP FIFO 深度/RP | WRESP credit/RP |
|---:|---:|---:|---:|---:|---:|---:|
| 256b  |  8 | 42.7 B | 68 | 544 | 32 | 32 |
| 512b  | 14 | 74.7 B | 41 | 574 | 32 | 32 |
| 1024b | 27 | 144 B  | 23 | 621 | 32 | 32 |

深度 = `ceil(2688B ÷ 每条裸字节) + FIFO_MARGIN_ENTRIES(4)`。三种位宽的"在飞
字节数"是一样的（2688B），只是每条消息更大、条数更少。WriteResp 的源头是本端
发出的写请求，速率上限是 AXI AW 的 1 条/拍而非链路带宽，因此按环路**周期数**
定深度：`CREDIT_LOOP_CYCLES(28) + 4 = 32` 条。

> **这三个 Flit 周期是实测出来的，不是拍脑袋加的余量。** 第一版只按
> `LINK_TAT_NS` 反推深度，零飞行时间下一切正常；PERF-5 把单向飞行时间设成真实的
> 20 ns 之后，读吞吐只能保持基线的 **90.10%**，数据方向的 Flit 平均填充从 100%
> 掉到 90% —— 对端不是没数据可发，是没 credit 可用。补上这三段之后保持率回到
> 100.00%。**这条经验对 RTL 实现同样成立**：按链路 TAT 反推缓冲深度是不够的，
> 而且这种损失在零延迟仿真里完全看不到。

### 3.3 Credit 分发

1. **复位后分批公布初始容量**。credit 字段是 3bit 离散编码
   （`{0,1,4,8,16,32,64,128}`），单个字段一次最多表示 128 granule。1024b 的
   RDATA 容量 621 granule 若只发一条 CrdtGrant，对端只拿到 128，"在飞字节数"
   仅 682B ≈ 14ns，覆盖不住 40ns 的 TAT，读带宽会被 credit 往返卡死在 ~17GB/s
   —— 这与协议效率无关，纯粹是公布方式的问题。因此复位时把全部容量放进
   pending，由后续若干个 CrdtGrant / MsgCredit **累加发满**。
2. 平时优先在业务 Flit 的 `MsgCredit[15:0]` 中捎带归还。一个 header 只服务一个
   RP，多 RP 间轮转。
3. 无业务 Flit 可捎带时（`!output_active_ && !cur_flit_.valid`），2 周期后发
   dedicated `CrdtGrant`。**这条路径只使用空闲的链路槽位**：PERF-4 读写混合场景
   实测 credit 粒度占该方向容量 0.00%～0.02%，即业务一满，credit 全部转为捎带，
   不与数据抢带宽。
4. WriteResp 字段只有 2bit（最大编码 3 → 8 granule），其余字段 3bit。
   `encode_credit_amount` 取"不超过 pending 的最大合法值"，因此 pending=32 时
   一次只发 8（WriteResp 字段 2bit 的上限），剩余留到后续继续发放。

## 4. 参数化

### 4.1 AXI 数据位宽

```bash
make WIDTH=1024 run      # 或 make test-all / make perf-all 跑全部三种
```

`-DAXI_DATA_WIDTH_CFG=256|512|1024` 同时决定：AoU DLENGTH、三类数据消息的
granule 数（**三张表互不相同**：WriteData 8/15/30、WriteDataFull 7/14/27、
ReadData 8/14/27）、以及上表的 FIFO/credit 深度。位宽改变会同时动到这几处，
是最容易出回归的维度，`make test-all` 必须三种全跑。

### 4.2 Resource Plane

```cpp
Axi2Flit single_rp("bridge");     // 默认 RP_COUNT=1
Axi2Flit qos_bridge("bridge", 2); // 启用 RP0/RP1
```

`RP_COUNT` 范围 1～4（AoU 的 RP 字段是 2bit）。RP 不是读/写通道编号；RP0 内部
仍有五套独立 credit，读写消息不会因共用 RP0 而共用同一 credit pool。单一 HBM
交互场景 RP_COUNT=1 即可。

> **集成约束 C-1（RP_COUNT > 1 时必须满足）**
>
> 同一个 AXI ID 在同一方向上未完成的事务，必须全部映射到同一个 RP。
>
> 原因：AXI4 要求同 ID 同方向的响应保序，而 AoU 的多个 Resource Plane 是
> **相互独立的流控平面** —— 各有各的 credit、各有各的接收 FIFO、发送侧按
> round-robin 轮询。两笔落在不同 RP 上的事务，响应回来的先后顺序是不确定的。
> 本模型默认按 `AxQOS % RP_COUNT` 分流，如果 SoC 侧对同一个 AxID 发出了不同
> 的 AxQOS，这条约束就被破坏了。
>
> 充分条件（满足任意一条即可）：`RP_COUNT = 1`；或同一 AxID 只用一个 AxQOS；
> 或按 ID 空间静态划分（如用 `AxID[9:8]` 直接当 RP 号）。
>
> 模型侧的处理：**不做跨 RP 重排序缓冲**，而是加运行时断言。理由见
> `include/rp_order_guard.h` 的文件头注释 —— 重排序缓冲的面积随 outstanding
> 深度增长，且会把快 RP 拖到慢 RP 的节奏上，反而抵消了分平面的意义，AoU 规范
> 本身也没有要求桥接单元承担这件事。违例计数可通过
> `Axi2Flit::order_violations()` 读出，TC9 会检查它为 0。

## 5. 文件划分

| 文件 | 职责 |
|---|---|
| `include/aou_types.h` | AoU 消息/Flit 结构、协议常量、链路参数、深度反推 |
| `include/credit_manager.h` | credit counter、MsgCredit/CrdtGrant 编解码 |
| `include/msg_builder.h` / `msg_decoder.h` | AXI ↔ AoU 消息互转 |
| `include/axi_if.h` | AXI4 通道结构（`AXI_USER_WIDTH = 16`，对齐 FLEX[15:0]） |
| `include/rp_order_guard.h` | 「同一 AXI ID 只能映射到固定 RP」的约束检查（约束 C-1） |
| `src/flit_packer.*` | 多 RP 调度、打包、跨 Flit 续传、credit 消耗、FDI 发送 |
| `src/flit_unpacker.*` | FDI 接收、header 解析、续传重组、R/B 分流 |
| `src/axi2flit.*` | 顶层连接与 AXI 五通道握手 |
| `tb/tb_common.h` | 独立参考模型（FlitScanner / LinkPacer / FlitDelayLine / RemoteAouModel）与数据图样 |
| `tb/tb_axi2flit.cpp` | 功能自检测试 TC1～TC9 + 波形 |
| `tb/tb_axi2flit_perf.cpp` | 延迟/带宽性能测量 PERF-1～PERF-5 + 硬门限考核 |
| `tb/tb_golden_vectors.cpp` | 黄金字节向量对拍：与 v0.8 PDF 手工转录的线上字节逐字节比对 |

## 6. 位域布局的来源：以 v0.8 PDF 为唯一基线

**协议基线 = `doc/AXI over UCIe Protocol Specification v0.8.pdf`。**
所有 granule 常量、字段位置、字节序一律以该 PDF 的字段表为准；参考 RTL
只作架构与命名参考，**不再作为位域依据**。

### 6.1 为什么不能拿参考 RTL 当基线

参考 RTL（`reference/tt-oca-harness-aou/`）比 v0.8 更早。v0.8 的 changelog
第 189 行明确写着：

> "Repurposed PROF/PROFEXTLEN fields as FLEX fields, defined use of FLEX fields
> as USER bits in the Basic Profile."

也就是说 RTL 里的 `PROF` / `PROFEXTLEN` 两个字段在 v0.8 已经被合并重定义成
`FLEX`，并规定在 Basic Profile 下承载 AXI 的 USER 位。照 RTL 实现会得到一个
**与 v0.8 不兼容**的线上格式。本模型据此把 `AXI_USER_WIDTH` 定为 **16**
（FLEX[15:0]），删掉了 PROF/PROFEXTLEN。

同类问题还有两处，都已按 PDF 纠正：

| 项 | 按 RTL 的旧实现 | 按 v0.8 PDF 纠正后 |
|---|---|---|
| USER 位 | PROF(4) + PROFEXTLEN(8)，`AXI_USER_WIDTH = 12` | FLEX[15:0]，`AXI_USER_WIDTH = 16` |
| CrdtGrant | 带前导 rsvd 字段、尾部多 17 bit | 按 PDF 字段表精确对齐，无多余位 |
| 位序 | 序列化按 LSB-first | 按 §5.8「Bytes count up · Bits count down · MSB-first」全面改为 MSB-first |

granule 常量本身与 RTL 一致（`AW_G=3 / AR_G=3 / B_G=1`，WriteData 8/15/30，
WriteDataFull 7/14/27，ReadData 8/14/27），已与 PDF 的表逐项核对通过。
注意这是**三张互不相同的表**，`WriteData` / `WriteDataFull` / `ReadData`
的 granule 数不能互相套用。

### 6.2 位序规则（最容易错、也最难测出来的一条）

规范 §5.8 的原话是 **"Bytes count up • Bits count down • MSB-first bit ordering"**。
整个序列化器因此是 MSB-first：字节地址递增、每字节内 bit7→bit0。

这个错误当初能长期存在，是因为**测试激励掩盖了它**：早期数据图样是"一整拍同一个
字节值"，地址也只用低 32 位，字节序整体翻转前后**完全一样**。现在的防线有两道：

1. **黄金字节向量对拍**（`tb_golden_vectors.cpp`）：期望字节序列是**从 PDF 字段表
   手工转录**的常量，不经过任何本模型的代码路径。三种位宽各 71 个检查项，
   覆盖全部 7 种消息 + Flit 头部（FDId / MsgStart / MsgCredit），
   每项都核对总 granule 数、首字节自描述编码、以及**整条消息的每个字节**。
2. **可辨识的激励**：数据图样改为「递增 ⊕ 走一位」（每字节都不同）并逐字节比对；
   地址高 32 位固定为 `TB_ADDR_TAG`，在链路上解回来核对。

> 只靠"MsgBuilder 编码后 MsgDecoder 能解回来"是无效验证 —— 两者共用同一份
> 位域理解，同时错时测试全绿。这是黄金向量套件存在的唯一理由。

### 6.3 参考 RTL 的定位与合规

参考 RTL 为 Apache-2.0 第三方代码（Tenstorrent / BOS Semiconductors），
且自身标注为 pre-release / evaluation 质量。本模型**只参考其架构划分与
参数命名，不复制任何实现代码**；位域与常量以 PDF 为准。RTL 实现阶段仍须按
冻结版 AoU 规范再逐 bit 复核一次。

## 7. 已知边界与后续计划

| 项 | 状态 |
|---|---|
| responder 侧（入站 WREQ/RREQ/WDATA） | 未实现，由 testbench 的 RemoteAouModel 扮演 |
| **约束 C-1 的 SoC 侧落实**（同 AxID 固定 RP） | 模型内已加运行时断言（TC9 考核），但**约束本身要由 SoC 的 ID/QoS 规划保证**，桥接单元只能检出、不能修复 |
| **AXI 合法性负向用例**（AxBURST≠INCR / AxSIZE 超位宽 / 跨 4KB / 非对齐） | 未实现。当前模型假定上游发出的是合法 AXI4 事务，非法激励行为未定义 |
| QoS 三模式仲裁 + 防饿死超时（P1-11） | 待做，当前为固定优先级 + RP 轮转 |
| WLAST 重建（P1-12） | 随 responder 侧一起做（AoU 的 WriteData 不含 WLAST） |
| 时钟频率决策（P1-13） | 当前 500MHz；1024b 下 AXI 侧上限 64GB/s，已不是瓶颈 |
| Activation/CSR、Aggregator/Splitter、FDI cancel/stall、Early BRESP | P2，本阶段不做 |
| Flit2DFI | 与存储控制器侧同事合并对齐 |
| FDI 侧 UCIe D2D 链路仿真模型联合测试 | 待对方模型就绪 |

> 带宽口径已定案为**口径B**（应用字节 ÷ 数据方向 Flit 数 × 240B），不再讨论。
> 三个口径的定义、为什么合同指标只能按口径B 考核、以及口径A 在 1024b 下
> 88.9% 的规范天花板，见 `doc/verification.md` §4.2。
