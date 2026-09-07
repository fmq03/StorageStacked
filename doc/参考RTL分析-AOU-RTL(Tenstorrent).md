# 参考 RTL 分析：AOU-RTL（AXI-over-UCIe Bridge IP）

| 项目 | 内容 |
| --- | --- |
| 分析日期 | 2026-09-07 |
| 分析人 | 陈飞扬 |
| 分析对象 | `reference/tt-oca-harness-aou/`（上游名 aou-rtl，Tenstorrent / BOS Semiconductors） |
| 对象性质 | AoU 规范发起方给出的**产品级参考实现**，含 RTL + 微架构规格书 + CSR + cocotb 验证环境 |
| 对标依据 | AoU Spec v0.7/v0.8、`DOC/MAS/aou_core_mas.md`（3399 行）、`RTL/packet_def_pkg.sv` |
| 用途 | 校准本项目 `systemc/` 模型的架构与参数，**不复制其代码** |

> **合规提示**：该目录为第三方开源代码，携带 `LICENSE` / `LICENSE-DOCS` / `COPYRIGHT` / `NOTICE`，且 README 明确标注 **pre-release / evaluation quality**。本项目仅作**架构参考与参数校准**，交付物中的 RTL 与 SystemC 需自行实现；若后续要引用其常量表或结构定义，需先确认许可条款并在交付文档中注明出处。

---

## 一、仓库总览

```
reference/tt-oca-harness-aou/
├── RTL/            42 个 .sv + 24 个 .v，设计源码
│   ├── AXI4MUX_3X1/    AXI 3:1 mux / splitter / aggregator
│   └── LIB/            行为级库单元（SRAM、同步器）
├── DOC/
│   ├── MAS/            aou_core_mas.md —— 微架构规格书（核心参考）
│   ├── integration_guide/  集成指南（1227 行）
│   └── csr/            寄存器文档
├── csr/            SystemRDL 寄存器定义
├── VERIF/          cocotb + Verilator 验证环境
└── INTEG/          SDC 约束 / UPF 功耗意图 / IP-XACT
```

两个顶层：`AOU_TOP`（含 FDI bringup 状态机的交钥匙封装）、`AOU_CORE_TOP`（纯协议引擎，FDI bringup 由外部负责）。两者都提供 **AXI4 Manager + AXI4 Subordinate + APB3 配置口 + 参数化 FDI 数据面**。

主要模块及规模：

| 模块 | 规模 | 职责 |
| --- | --- | --- |
| `AOU_CORE.sv` | 114 KB | 协议引擎主体 |
| `AOU_RX_CORE.sv` | 90 KB | Chunk 解码 / 消息还原 |
| `AOU_TX_CORE.sv` | 73 KB | 消息打包 / ring buffer |
| `AOU_CORE_SFR.v` | 75 KB | 配置寄存器 |
| `AOU_CORE_RP.sv` | 66 KB | 单 RP 数据通路实例 |
| `AOU_TX_AXI_BUFFER.sv` | 57 KB | AXI 侧入口缓冲 |
| `AOU_TX/RX_CRD_CTRL.sv` | 32/33 KB | 发/收方向 credit 管理 |
| `AOU_AXIMUX_1XN_SS.v` | 27 KB | RP 分发 |
| `AOU_ACTIVATION_CTRL.sv` | 25 KB | 激活/去激活流程 |
| `AOU_FDI_BRINGUP_CTRL.sv` | 20 KB | FDI 链路拉起 |
| `packet_def_pkg.sv` | 16 KB | **消息位域与粒度常量定义** |

另有：`AOU_EARLY_BRESP_CTRL_AWCACHE`（写早响应）、`AOU_AW_W_ALIGNER`、`AOU_AXI_WLAST_GEN`、`AOU_EARLY_TABLE`、`AOU_TX_QOS_ARBITER/BUFFER`、`AOU_TX/RX_FDI_IF`、`AOU_SLV_AXI_INFO`、`AOU_ERROR_INFO`、`AOU_FIFO_RP`、`AOU_DATA_W/R_FIFO_NS1M_TPSRAM`。

---

## 二、总体架构与数据流

```
本端 AXI Manager
      │  AW/W/AR (Subordinate 口)
      ▼
 AOU_AXIMUX_1XN_SS ── 按地址/QoS 分发到 RP0..RP3
      ▼
 AOU_TX_AXI_BUFFER ── 每 RP 独立入口缓冲
      ▼
 AOU_TX_QOS_ARBITER ── Round-Robin / Port QoS / AXI QoS 三选一 + 超时提升
      ▼
 AOU_TX_CORE ── 取得 credit 后打包成 AoU 消息 → 写入 ring buffer（深度仅 2 entry）
      ▼
 AOU_TX_FDI_IF ── 切成 32B/64B/128B chunk，cut-through 送 FDI
      │
    ═══ UCIe D2D ═══
      │
 AOU_RX_FDI_IF ── 处理 pl_valid / pl_flit_cancel，2 entry staging buffer
      ▼
 Chunk Decoder ── 移位寄存器 + 多路并行解码器，12 granule/cycle
      ▼
 Message FIFO（AW/AR/B 深 44，W/R 深 88 或 140）
      ▼
 AOU_AXI4MUX_3X1_TOP ── 位宽转换（Bypass/Upsizer/Downsizer）+ Splitter + Aggregator
      ▼
本端 AXI Subordinate（Manager 口）
```

**关键结构性事实**：这是一个**双向对称**的桥。同一个 IP 既做 AXI Subordinate（收本端请求 → 打包发出），也做 AXI Manager（收对端 flit → 还原成 AXI 命令发给本端从设备）。我们当前的 SystemC 模型只实现了 initiator 半边（出站请求 + 入站响应），远端半边（入站请求 → AXI Manager 出口）尚缺，而这半边正是合同「Flit 流到存储控制器原子化命令的映射」的落点。

---

## 三、关键设计点及其对本项目的启示

### 3.1 Cut-through chunk 流水，**从不落 256B flit**

MAS §1.1 原文：

> processes data in 32-byte, 64-byte, or 128-byte chunks (selected via the `FDI_CONFIG` parameter) **in a cut-through manner, without converting them to 256-byte flit data**.

这是它低延迟的第一来源。TX 侧 ring buffer 只有 **2 entry**，消息一旦打包就以 chunk 为粒度往 FDI 推，不等整个 256B flit 攒满。

对照 B2B 单时钟域单向延迟（MAS §1.1）：

| 通道 | 单向延迟 |
| --- | --- |
| Read Request | 7 cycle |
| Read Data | 8 cycle |
| Write Request | 9 cycle |
| Write Data | 9 cycle |
| Write Resp | 6 cycle |

含 CDC 的 512-bit 配置（MAS Table 8）：AR 9 / R 11 / AW 12 / W 12 / B 10 / Misc 9 cycle @1GHz。AW、W 多出的 2 拍来自 WLAST 生成器和位宽转换器。**AXI 读请求经 UCIe 到读数据返回的 RTT ≈ 45 cycle**（不含远端存储延迟）。

> **对我们的启示（最重要一条）**：我们的 `FlitPacker` 是 store-and-forward + `FLUSH_TIMEOUT_CYCLES = 4` 的超时冲刷。实测桥接延迟里，写路径 16 ns 中有 **8 ns 纯粹是这 4 拍等待**。这条路走到底也只能靠减小 timeout 来逼近，架构上不对。指标要求 ≤20 ns，参考实现在 1 GHz 下单向 6~12 cycle 即 6~12 ns，留有充足余量。

### 3.2 消息粒度常量（源码级权威值）

`RTL/packet_def_pkg.sv` 第 37~48 行：

```systemverilog
parameter AW_G       = 3;    parameter AR_G       = 3;    parameter B_G        = 1;
parameter W256b_G    = 8;    parameter W512b_G    = 15;   parameter W1024b_G   = 30;
parameter WF256b_G   = 7;    parameter WF512b_G   = 14;   parameter WF1024b_G  = 27;
parameter R256b_G    = 8;    parameter R512b_G    = 14;   parameter R1024b_G   = 27;
```

与我们模型的对照：

| 消息 | 参考 RTL | 我们 `aou_types.h` | 结论 |
| --- | --- | --- | --- |
| WriteReq | **3** | `REQ_GRANULES = 4` | ❌ 偏大 1 |
| ReadReq | **3** | `REQ_GRANULES = 4` | ❌ 偏大 1 |
| WriteResp | **1** | `WRESP_GRANULES = 2` | ❌ 偏大 1 |
| Misc | 1 | `MISC_GRANULES = 1` | ✅ |
| WriteData 256/512/1024b | **8 / 15 / 30** | 8 / 12 / 24 | ❌ 512b、1024b 都错 |
| WriteDataFull 256/512/1024b | **7 / 14 / 27** | 未区分 | ❌ 缺失该消息类型 |
| ReadData 256/512/1024b | **8 / 14 / 27** | 8 / 12 / 24 | ❌ 512b、1024b 都错 |

注意一个我们此前没注意到的点：**WriteData / WriteDataFull / ReadData 三者的粒度表互不相同**，不能共用一个 `dlength_to_granules()`。WriteData 比 ReadData 多带 strb，1024b 时是 30 vs 27。

从位域定义还能反推出字段宽度（`st_write_req_packet` 等）：

```
WriteReq  = 4 msg_type + 2 rp + 10 awid + 64 awaddr + 8 awlen + 3 awsize
          + 1 awlock + 4 awcache + 3 awprot + 4 awqos + 1 rsvd
          + 4 profextlen + 12 prof = 120 bit = 15 B = 3 granule ✓
WriteResp = 4 + 2 + 10 bid + 2 bresp + 6 rsvd + 4 profextlen + 12 prof
          = 40 bit = 5 B = 1 granule ✓
```

> **对我们的启示**：AoU 的 **AXI ID 是 10 bit**（`awid[9:0]`），我们 `axi_if.h` 里 `AXI_ID_WIDTH = 8`。另外 `prof[11:0]` + `profextlen[3:0]` 是 profile 扩展字段，我们完全没有建模——`profextlen != 0` 时消息会变长，这直接影响 credit 的保守计算。

### 3.3 跨 flit 续传 + 每拍多消息并行处理

MAS §3.3：一条 B 消息 1 granule，收到 chunk 即可立刻解码；而 1024-bit 的 Rdata/Wdata 是 27~30 granule，**"may span up to four flits during transmission"**，需要约 3 个 cycle 才能解完。

RX 每拍吞吐（MAS §1.1）：**4 个 read req、4 个 write req、2 个 read data、2 个 write data、12 个 write resp**；Chunk Decoder 一拍解 **12 granule**。因此 AW/AR FIFO 实现为 4S1M，B FIFO 实现为 12S1M。

TX 侧：*"Once there is enough space to hold all messages, messages are stored to the ring buffer in parallel"*，并且 *"all TX requests and data messages are packed in a compact format, ensuring that **no AoU payload slots are wasted**. The current payload is even updated when the UCIe stalls the current chunk and space remains available."*

> **对我们的启示**：两条硬伤。
> 1. 我们的 packer **不拆消息**，unpacker **不接收续传**。这意味着 1024b 消息（27 granule）一个 flit 只能放 1 条，占用率 27/48 = 56.3%，链路效率反而只有 50.0%，比 256b 的 75.0% 还差——**跨 flit 续传是 1024b 路线能否成立的前提**。
> 2. 我们的 `select_candidate()` 每拍只打 1 条消息。24 GT/s ×16 lane 下链路每 2.67 cycle（@500MHz）吃掉一个 flit，需要 ≥18 granule/cycle 的打包速率；1024b 时 1 条/拍恰好 27 granule/cycle 够用，但 256b/512b 就完全跟不上。

### 3.4 Credit 控制器：保守派发公式与时序定义

MAS §3.9 给出了完整的 credit 计算方法，核心是**按最坏情况保守派发**：

```
MAX AVAILABLE CREDIT = min(所有可能消息组合下的最大可派发 credit)

RX AW MAX CREDIT = RX AW FIFO DEPTH × WriteReq Granules
RX AR MAX CREDIT = RX AR FIFO DEPTH × ReadReq Granules
RX B  MAX CREDIT = RX B  FIFO DEPTH × WriteResp Granules
RX W  MAX CREDIT = (RX W FIFO DEPTH / 4) × WriteDataFull-1024bit Granules
RX R  MAX CREDIT = {RX R FIFO DEPTH / (AXI_DATA_WD / 256)} × AXI_DATA_WD 对应 RDATA 粒度
```

W 通道取 `/4 × 27` 是因为：Data FIFO 每 entry 256 bit，1024b 消息占 4 entry；在所有组合中 **1024b WriteDataFull 的"每 FIFO entry 承载粒度数"最小（6.75）**，所以按它算最保守。R 通道同理，按 `AXI_DATA_WD` 对应的最宽 RDATA 保守假设。

其它关键约定：
- **Tx credit 在 TX_CORE 把消息压入 per-RP TX FIFO 的那一刻扣减**（不是发出 flit 时）。
- **Rx credit 在 credit 消息完成 valid/ready 握手时即视为"已派发并预留"**；归还则是**数据真正从 RX FIFO 出队时**才算，而不是消息离开 RX Core 时。
- 只用两种方式之一派发：Misc CreditGrant 消息 **或** 协议头 MsgCredit 字段，**不同时用**。单 RP 时仅初始派发用 Misc，之后全走 MsgCredit；多 RP 时全部走 MsgCredit，各 RP 轮转。
- 只有当 wreq/rreq/wdata/rdata/wresp 中至少一项非零时才发 grant。
- Credit 编码表 `CREDIT_TABLE[7:0] = {128, 64, 32, 16, 8, 4, 1, 0}` —— 与我们 `decode_credit_encoding()` 的 `VALUES[8] = {0,1,4,8,16,32,64,128}` 一致（顺序相反而已）。✅

> **对我们的启示**：我们的 `CreditManager` 机制方向是对的（这一条是模型里质量最高的部分），但**容量取值和保守公式没有落地**。`RX_RDATA_CREDITS_PER_RP = 4 × 8 = 32 granule`，而按 42.7 GB/s × 40 ns RTT 反推需要 ≈342 granule，只有需求的 9%。另外我们的归还时机需要复核是否严格对齐"出队时才归还"。

### 3.5 FIFO 深度按 TAT 反推（可直接套用的方法学）

MAS §3.4 把推导过程写得很清楚：

> AOU_CORE 含 D2D adaptor 的 TAT ≈ **40 ns**（@1GHz 即 40 cycle）。因此 AW / AR / B FIFO 深度取 **44**（含余量）。
>
> Data FIFO 不同：按 512-bit 总线，一条 W 消息 15 granule，解码器 12 granule/cycle，故最快 15÷12 = **1.25 cycle** 到达一条；512b 消息占 **2 个 FIFO entry**；覆盖 50 cycle 的 TAT 则深度 = 50 ÷ 1.25 × 2 = 80，含余量取 **88**。
>
> `FDI_CONFIG` 选 1024b(128B) 接口时每 chunk 粒度率翻倍，W/R FIFO 深度默认提到 **140**。
>
> 面积段补充：**期望支撑 128 GB/s 需要 R/W FIFO 176 entry**。

参数默认值（MAS §6.4 Table 6）：

| 参数 | 默认 |
| --- | --- |
| `RP_COUNT` | 1（可参数化到 4） |
| `RP*_AXI_DATA_WD` | 512 |
| `AXI_PEER_DIE_MAX_DATA_WD` | 1024（固定） |
| `RP*_RX_AW/AR/B_FIFO_DEPTH` | 44 |
| `RP*_RX_W/R_FIFO_DEPTH` | 88（1024b FDI 配置下 140） |
| `S/M_RD/WR_MO_CNT` | 32（outstanding 表项数） |
| `FDI_CONFIG` | 32B / 64B / 128B，单 PHY 或双 PHY |

> **对我们的启示**：这直接印证了上一次评审里"credit 深度不足会在接入 D2D 模型时立刻塌掉带宽"的判断，而且给了**可复用的推导公式**。我们的 `RX_RDATA_FIFO_DEPTH_PER_RP = 4` 与参考的 88~140 差 20~35 倍。建议把这些深度做成"由 TAT 参数算出来"的 `constexpr` 函数，而不是写死常数——这样 D2D 模型接进来改一个 TAT 就能重算。

### 3.6 QoS：三种仲裁模式 + 超时提升防饿死

- **Round-Robin**（`ARB_MODE = 2'b00`）：有效 RP 间轮转，握手时决定下一个 grant 归属。
- **Port QoS**（`2'b01`）：每 RP 的优先级由 SFR `PRIOR_RP_AXI.PRx_PRIOR` 配置，不看 AXI QoS 字段。
- **AXI QoS**（`2'b10`）：用 AXI 的 `AxQOS` 字段。

三档优先级 High / Normal / Low，并用 `PRIOR_TIMER.TIMER_THRESHOLD` / `TIMER_RESOLUTION` 做**超时提升**防止低优先级饿死。

> **对我们的启示**：我们的 `map_qos_to_rp(qos) = qos % rp_count_` 把 QoS 直接当成 RP 索引用，语义是错的——QoS 决定的是**仲裁优先级**，RP 决定的是**独立流控域**，两者是正交概念。而且我们的 `select_candidate()` 是固定的 ReadReq > WriteReq > WriteData 静态优先级，无防饿死机制。

### 3.7 位宽适配三件套：Converter / Splitter / Aggregator

**位宽转换器** `AOU_AXI4MUX_3X1_TOP`：3 个 slot 分别对应远端 256/512/1024b 数据宽度，按本端 `AXI_DATA_WD` 选择 Bypass / Upsizer(`AOU_AXI_UP`) / Downsizer(`AOU_AXI_DOWN`)。写路径按 **DLEN** 选路，读路径按 **ARSIZE** 选路。DLENGTH 由 ARSIZE 推导：ARSIZE ≤ 5 → `2'b00`；= 6 → `2'b01`；= 7 → `2'b10`。

**事务拆分器** `AOU_AXI_SPLIT_TR`：把超长 burst 拆成 `MAX_AxBURSTLEN+1`（2^n−1）的子 burst，**保留 2 个 AXI ID bit** 做子 burst 跟踪；重排序缓冲重建 RLAST；B 响应用 `r_wr_pending_cnt` 合并并传播 BRESP 错误；ID 不匹配触发中断。

**聚合器** `AOU_AGGREGATOR`（MAS §3.8）：把窄 beat 合并成满位宽 beat。

> 例：AR size 256-bit、burst length 16 → 转成 size 1024-bit、burst length 4。

写聚合触发条件：`AWSIZE < clog2(DATA_WD/8)` **且** `AWLEN != 0`。做法是提升 AWSIZE 到满宽、按比例重算 AWLEN、对连续窄 beat 的 WDATA/WSTRB 做 OR 合并。用一个 **深度 16 的同步 FIFO** 解耦 AW 与 W，AW 只在确认 W FIFO 有足够空间容纳重算后的 AWLEN 时才发到 master 侧，**防止下游反压 W 时死锁**。读聚合同理，`AOU_AGGREGATOR_INFO` 记录原始 burst 几何形状，用于回来时重建从侧 RLAST。每 RP 一个实例，由 `AOU_CON0.RPn_AXI_AGGREGATOR_EN` 开关。

> **对我们的启示**：上次评审里我推荐的"AXI 侧上 1024b，1 个 AXI beat = 1 条 AoU 1024b 消息"，参考实现给出了两条落地路径：要么本端 AXI 直接就是 1024b，要么保持 256b 但打开 Aggregator 做 4:1 合并。后者对上层更友好（上层无需改总线宽度），但要注意那个**深度 16 的解耦 FIFO 与死锁防护**——这是我们建模时容易漏掉的细节。

### 3.8 `AOU_AXI_WLAST_GEN`：AoU 不传 WLAST

AoU 的 WriteData 消息里**没有 WLAST 字段**（对照 `st_write_data*_packet` 位域可确认）。接收侧必须从 `I_S_WDLENGTH` 加上 AW 相位对齐来重新生成 WLAST，这也是 AW/W 通道多 2 拍延迟的原因之一。配套还有 `AOU_AW_W_ALIGNER` 做 AW/W 对齐。

> **对我们的启示**：我们用 `WriteRoute {rp, beats_remaining}` 的 AW 路由 FIFO 来跟踪，但 beat 数不匹配时只打 warning 不做纠正。应改成显式的 WLAST 重生成 + 错误上报。

### 3.9 写早响应（Write Early Response）

`AOU_EARLY_BRESP_CTRL_AWCACHE`，每 RP 一个实例，由 `WRITE_EARLY_RESPONSE_RPn.EARLY_BRESP_EN` 开关。收到 AW + 末拍 W（WLAST）后**立刻**回 `BRESP = OKAY`，不等对端。

- 仅对 **bufferable 写**（`AWCACHE[0] = 1`）生效，non-bufferable 原样透传。
- 真正的 B 回来时静默吞掉：`O_AXI_S_BVALID` 保持低，`O_AXI_M_BREADY` 拉高把响应从 RX 通路排空。
- 若真实 BRESP 带错（`BRESP[1] = 1`），无法再经 B 通道上报，改为把 BID/BRESP 捕获进 SFR 并触发 `INT_EARLY_RESP_ERR` 中断，软件读完写 1 清除。

> **对我们的启示**：这是一个"用错误上报换写延迟"的显式取舍。**如果 20 ns 指标在写路径上卡住，这是一张明牌**。但它把错误语义从同步变成异步，需要在设计报告里写清代价。建议做成可配开关，默认关闭，测延迟时打开对比。

### 3.10 UCIe flit cancel 与 stall 处理

`AOU_RX_FDI_IF` 用 FDI 的 `pl_valid` / `pl_flit_cancel` 区分三种情况：

| `pl_valid` | `pl_flit_cancel` | 动作 |
| --- | --- | --- |
| 1 | 0 | **正常**：相位计数器 +1，chunk 存入 staging buffer |
| 1 | 1 | **重传 / 部分有效**：相位计数器 −1，staging buffer 用 `{zero_data, pl_data}` 重对齐，valid pattern `{1'b0, 1'b1}` |
| 0 | 1 | **整 flit 取消**：相位计数器 −2，staging buffer 清空 |

staging buffer 深 `HF_PHASE_CNT = PHASE_CNT / 2`（512-bit FDI 下为 2 entry，即半个 flit）。输出门控：`rx_chunk_data_valid = r_data_valid[1] && !pl_flit_cancel`。取消处理是**纯组合**的，不引入额外流水级。

另外还处理 D2D Adapter 的 stall 请求，**同时保持 flit 对齐边界**。

> **对我们的启示**：这部分等第 4 点的 FDI UCIe D2D 链路仿真模型接进来才需要。但**现在就应该把 FDI 接口信号补全**（至少留出 `pl_valid` / `pl_flit_cancel` / stall 的端口），否则联调时要动接口。

### 3.11 低功耗模式（顺带一提的实现细节）

`AOU_CON0.TX_LP_MODE = 1` 时只在有真实 AXI 消息要发时才向 FDI 送 payload，用 `TX_LP_MODE_THRESHOLD` 配置心跳频率。`THRESHOLD = 0` 是全门控模式。有一句话对我们很有用：

> **Once a flit is started it always finishes**; any granule slots that were not filled are sent as zeros and the corresponding MsgStart bits are 0.

即 flit 一旦开始就必须发完，未填满的 granule 补零、MsgStart 位置 0。默认值是 0（always send），**为了降低延迟**。

### 3.12 PPA 基准（我们可以直接拿来对标的数字）

**效率**（MAS Table 9，512-bit 配置，公式 `128 / (cycle_from_1st_valid_to_last_valid + 1)`）：

| 场景 | 实测 | AoU 理论上限 |
| --- | --- | --- |
| 16-burst 读 | 85.3% | 85.7% |
| 16-burst WriteFull | 84.2% | 84.6% |
| 16-burst 普通写 | 79.0% | 79.0% |
| 1-burst 读 | 85.3% | 85.7% |
| 1-burst WriteFull | 70.3% | — |
| 1-burst 普通写 | 67.0% | 66.7% |

公式里 `+1` 是"第一个 valid 之前隐藏的那个气泡"，MAS 解释为**不可避免**，因为 AoU 数据消息装不进单个 64B chunk。

**面积**（1 GHz，`FDI_CFG_SP_64B` 等效配置）：1 RP（AXI 512-bit）**97K um²**；2 RP（256b + 512b）197K um²。RX_W_FIFO 和 RX_R_FIFO 占面积大头（当前是寄存器堆实现）。

> **对我们的启示**：三点。
> 1. **参考实现在 512-bit 下实测效率 79%~85.3%，普通写只有 79%。** 这从工程侧再次确认了指标②的 90% 在"应用数据 / 链路原始字节"口径下**不可达**——AoU 512b 的协议上限就是 85.7%，1024b 也只有 88.9%。上次评审提出的三种口径（A/B/C）需要在与中兴微对齐时正式提出来。
> 2. 时钟目标是 **1 GHz**，我们模型跑 500 MHz。20 ns 指标下，1 GHz 意味着 20 拍预算，500 MHz 只有 10 拍——**频率假设直接决定延迟指标的宽松程度**，需要尽早确定。
> 3. 它给了完整的测试方法（效率公式、burst 长度扫描、单/双向），我们的 testbench 可以照这个格式产出对标表格。

---

## 四、参考实现 vs 当前 SystemC 模型

| 维度 | 参考 RTL | 当前 `systemc/` | 差距 |
| --- | --- | --- | --- |
| 打包架构 | cut-through，chunk 流水，TX ring buffer 仅 2 entry | store-and-forward，`FLUSH_TIMEOUT_CYCLES = 4` | 🔴 架构级 |
| 跨 flit 续传 | 支持，1024b 消息跨最多 4 flit | 不支持 | 🔴 架构级 |
| 每拍处理量 | TX 并行打包；RX 12 granule/cycle | 1 条消息/拍 | 🔴 |
| 粒度常量 | 3/3/1，8/15/30，7/14/27，8/14/27 | 4/4/2，8/12/24 | 🔴 正确性 |
| WriteDataFull | 支持（1024b 省 3 granule = 10%） | 未实现 | 🔴 效率 |
| AXI ID 宽度 | 10 bit | 8 bit | 🟡 |
| profile 字段 | `prof[11:0]` + `profextlen[3:0]` | 未建模 | 🟡 |
| AXI 数据宽度 | 参数化 256/512/1024，含 Up/Downsizer | 固定 256 | 🔴 带宽 |
| AXI 吞吐 | 1 beat/cycle | **1 beat/2 cycle**（握手后强制插气泡） | 🔴 带宽 |
| Aggregator | 有（256b×16 → 1024b×4） | 无 | 🟡 |
| Splitter | 有（保留 2 ID bit + 重排序） | 无 | 🟡 |
| WLAST 生成 | 专用模块 | AW 路由 FIFO，仅告警 | 🟡 |
| RX FIFO 深度 | AW/AR/B 44，W/R 88~140，按 TAT 推导 | 一律 4 | 🔴 带宽 |
| Credit 容量 | 保守公式，按 FIFO 深度算 | 32 granule（需求的 9%） | 🔴 带宽 |
| Credit 机制 | 双通道（Misc / MsgCredit），互斥使用 | 已实现，机制正确 | ✅ |
| Credit 编码表 | `{128,64,32,16,8,4,1,0}` | 一致 | ✅ |
| 多 RP 隔离 | 4 RP，独立 FIFO/credit | 已实现，参数化 | ✅ |
| QoS 仲裁 | 3 模式 + 超时提升防饿死 | `qos % rp_count`，静态优先级 | 🔴 语义错误 |
| Early BRESP | 有，可配 | 无 | 🟢 可选 |
| Flit cancel / stall | 完整支持 | 无（FDI 接口信号未预留） | 🟡 待 D2D |
| Activation 流程 | `AOU_ACTIVATION_CTRL` + CSR | 无 | 🟢 可选 |
| 反向通路（AXI Manager 出口） | 有，双向对称 | 无（仅 initiator 半边） | 🔴 交付项 |
| 延迟测量 | Table 8，逐通道 cycle | testbench 无测量 | 🔴 指标 |
| 带宽测量 | Table 9，burst 扫描 | testbench 无测量 | 🔴 指标 |
| 链路速率建模 | 有（FDI 时序） | `flit_ready` 常拉高，无节流 | 🔴 指标 |

图例：🔴 必须补 / 🟡 建议补 / 🟢 可选 / ✅ 已达标

---

## 五、SystemC 模型完善清单

### P0 —— 协议正确性与指标前提（建议本周内完成）

| # | 项 | 涉及文件 | 说明 |
| --- | --- | --- | --- |
| 1 | 修正粒度常量 | `include/aou_types.h` | `REQ_GRANULES` 4→3、`WRESP_GRANULES` 2→1；把 `dlength_to_granules()` 拆成三个函数：`wdata_granules()` = 8/15/30、`wdatafull_granules()` = 7/14/27、`rdata_granules()` = 8/14/27。连带更新 `msg_builder.h`、`credit_manager.h` 初始容量、`flit_unpacker.cpp` 与 `tb` 中写死的 8/2 期望值 |
| 2 | 实现跨 flit 续传 | `src/flit_packer.cpp`、`src/flit_unpacker.cpp` | Packer 允许消息在 flit 尾部截断、下一 flit 从 G0 续；Unpacker 维护跨 flit 的接续状态机。**这是 1024b 路线成立的前提**，不做则 1024b 效率反而低于 256b |
| 3 | AXI 通道去气泡 | `src/axi2flit.cpp` | AW/AR/W 三个线程当前在握手后强制拉低 `*_ready` 一拍，吞吐腰斩。改为 `ready` 只取决于下游 FIFO 是否有空间 |
| 4 | 每拍打包多条消息 | `src/flit_packer.cpp` | `select_candidate()` 改为循环，直到 flit 填满或无可用候选。目标 ≥18 granule/cycle |
| 5 | 取消固定 flush 超时 | `include/flit_packer.h`、`src/flit_packer.cpp` | `FLUSH_TIMEOUT_CYCLES` 从 4 改为 0（无候选即立即 flush），仅在需要凑 credit 时保留兜底。这一项单独就能把写延迟从 16 ns 降到约 8 ns |
| 6 | testbench 加延迟与带宽测量 | `tb/tb_axi2flit.cpp` | AW/AR 握手打时间戳 → 对应 flit 出 FDI 打时间戳，统计 min/avg/max；带宽按上次评审的 A/B/C 三口径分别统计。这是**指标能否验收的直接证据**，目前完全没有 |
| 7 | 链路速率节流 | `tb/tb_axi2flit.cpp` | 按 x16 @24 GT/s = 48 GB/s，每 5.33 ns 才允许一个 flit 通过，用 `flit_ready` 节流。当前 `flit_ready` 常拉高，等于假设链路无限快，测出来的带宽没有意义 |

### P1 —— 达成 ≥90%（口径 C）与 ≤20 ns 所需

| # | 项 | 涉及文件 | 说明 |
| --- | --- | --- | --- |
| 8 | AXI 数据宽度参数化到 1024b | `include/axi_if.h` 及全链路 | `AXI_DATA_WIDTH` 从固定 256 改为模板/构造参数；DLENGTH 按 AxSIZE 推导（≤5→`00`、6→`01`、7→`10`） |
| 9 | 实现 WriteDataFull | `include/msg_builder.h`、`aou_types.h` | strb 全 1 时改用 MSGTYPE `0x6`，1024b 下 27 granule 而非 30，**直接提效 10%** |
| 10 | FIFO/credit 深度按 TAT 反推 | `include/axi2flit.h`、`credit_manager.h` | 用 §3.5 的公式写成 `constexpr` 推导函数，输入 TAT（暂取 40 ns）、时钟频率、AXI 位宽，输出各 FIFO 深度与 credit 容量。当前 4 entry / 32 granule 必须提到 44~140 entry / 300+ granule 量级 |
| 11 | QoS 仲裁重构 | `src/flit_packer.cpp`、`src/axi2flit.cpp` | 把 RP 映射（流控域）与 QoS 仲裁（优先级）解耦；实现 Round-Robin / Port QoS / AXI QoS 三模式 + 超时提升防饿死 |
| 12 | WLAST 重生成 | `src/axi2flit.cpp` | 参照 `AOU_AXI_WLAST_GEN`，由 AW 的 len 显式重建 WLAST 并对不匹配上报错误，替换现在只打 warning 的做法 |
| 13 | 明确时钟频率假设 | `tb/`、设计文档 | 参考实现 1 GHz，我们 500 MHz。20 ns 预算在两种频率下差一倍，需在设计报告中固定并说明依据 |

### P2 —— 功能完整性与联调准备

| # | 项 | 说明 |
| --- | --- | --- |
| 14 | 反向通路：入站请求 → AXI Manager 出口 | 参考 IP 是双向对称的。合同「Flit 流到存储控制器原子化命令的映射」落在这半边，虽然 Flit2DFI 由冯梦奇/存控同事负责，但 AXI Manager 出口属于 AXI2FLIT 范畴 |
| 15 | AXI Aggregator | 256b×16 → 1024b×4，含深度 16 的 AW/W 解耦 FIFO 与死锁防护 |
| 16 | AXI 事务拆分器 | 超长 burst 拆分 + 保留 2 ID bit + 重排序重建 RLAST + B 合并 |
| 17 | FDI 接口信号补全 | 预留 `pl_valid` / `pl_flit_cancel` / stall，为 D2D 链路模型联调做准备（**建议提前做，避免联调时改接口**） |
| 18 | Early BRESP（可配开关） | 若写路径 20 ns 卡住的备选方案，默认关闭 |
| 19 | profile 字段 (`prof` / `profextlen`) | 影响消息长度与 credit 保守计算 |
| 20 | Activation / Deactivation + CSR 模型 | 完整性项，优先级最低 |

### 验证环境参考

参考实现用 **cocotb + Verilator**（`VERIF/`，含 `test_aou_loopback.py`、`test_csr_reset.py`、`decoder/fdi_flit_decoder.sv`）。两点可借鉴：

1. **Loopback 测试**：把两个 `Axi2Flit` 实例背靠背互连（一个的 `flit_out` 接另一个的 `flit_in`），构造完整的 AXI → Flit → AXI 回环，这样才能测端到端延迟，也天然覆盖了打包/解包的一致性。
2. **独立的 flit 解码器**：`fdi_flit_decoder.sv` 是与 DUT 无关的第三方解码器。我们的 `monitor_thread` 目前复用了 DUT 的解码常量，如果常量本身错了，测试也跟着错——建议把 monitor 的解码逻辑按 spec 独立写一份。

---

## 六、不建议照搬的部分

- **SFR / CSR 全套寄存器**：参考实现有 75 KB 的 `AOU_CORE_SFR.v`。我们的模型阶段用构造参数即可，不必建 APB 寄存器模型。
- **双 PHY / `FDI_CONFIG` 全部 5 种配置**：先支持单 PHY 一种宽度，等 D2D 模型定了再扩。
- **UPF 功耗意图、SDC 约束、IP-XACT**：属于后端交付物，与 SystemC 建模阶段无关，但 `INTEG/constraints/` 在做 RTL 时值得回来看。
- **AOU_TPSRAM / 行为级库单元**：SystemC 里用 `sc_fifo` 即可。

---

## 七、下一步

1. 先做 P0 的 1~7 条，做完后重跑仿真，**产出第一版有数字的延迟/带宽报告**（目前完全没有测量数据，这是最紧迫的缺口）。
2. 拿到数字后再决定 P1 里 1024b 与 Aggregator 两条路线走哪条。
3. 与中兴微对齐**带宽 90% 的口径定义**——参考实现实测 79%~85.3%、AoU 协议上限 88.9%，按"应用数据/链路原始字节"口径 90% 无法达成，这个问题需要在第二阶段（2026-10-30）之前书面澄清。

---

*相关文档：[2026-09-07 设计评审](./review/2026-09-07-设计评审.md)、[AXI2FLIT 技术路线与设计 Spec](./AXI2FLIT技术路线与设计Spec.md)*
