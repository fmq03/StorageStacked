# AoU 线格式与 UCIe 接入边界

日期：2026-09-07。本文定义接入代码可直接依赖的接口；协议字节位置依据本地
AoU v0.8 PDF 图 4、图 5，消息字段依据该规范对应表格。测试方法见
[verification.md](verification.md)，阶段交付记录见
[接入前整改与验证](../../doc/review/2026-09-07-UCIe接入前整改与验证.md)。

## 1. 类型和职责

| 类型/模块 | 职责 | 是否跨链路 |
|---|---|---|
| `AouFlit` | 桥内部组包对象，含统计辅助字段 | 不按对象内存发送 |
| `AouWireFlit` | 固定 `std::array<uint8_t,250>` | 全部 250B 进入 FDI payload |
| `serialize_aou` / `deserialize_aou` | PH 与 240B payload 编解码 | 无状态、无额外仿真时间 |
| `AouStreamDecoder` | 根据 MsgStart/首字节重组跨 Flit 消息 | 每个接收方向独立一个实例 |
| `aou_format6::scatter/gather` | 250B 逻辑顺序与 256B 物理布局转换 | 链路侧调用 |
| `UcieAouEndpoint` | ready/valid 与 FDI FIFO 相连 | 位于 UCIe 一侧 |

逻辑 250B 唯一排列为 `[PH B0..B9][G0..G47]`。数组元素就是一个线上字节，
不能使用 `reinterpret_cast`、`sizeof(AouFlit)` 或直接结构体 memcpy 代替编码。
`AouFlit::valid` 不传输，外层握手/FIFO 读写表示有效；`used_granules` 不传输，
反序列化后置为 -1，使误用容易暴露。发送侧仍可用它组包和计数。

## 2. Protocol Header

下表 bit 编号采用每字节 bit0 为最低有效位，按图 5 直接对应。消息数据中的
MSB-first 是另一层编码规则，不能用于反转此表。

| PH byte | 位映射 |
|---|---|
| B0 | bit7:4=MsgStart[3:0]，bit3:2=0，bit1:0=FDId |
| B1 | bit7:0=MsgStart[11:4] |
| B2 | bit7:4=MsgStart[15:12]，bit3:0=0 |
| B3 | bit7:0=MsgStart[23:16] |
| B4 | bit7:0=MsgCredit[7:0] |
| B5 | bit7:0=MsgCredit[15:8] |
| B6 | bit7:4=MsgStart[27:24]，bit3:0=0 |
| B7 | bit7:0=MsgStart[35:28] |
| B8 | bit7:4=MsgStart[39:36]，bit3:0=0 |
| B9 | bit7:0=MsgStart[47:40] |

黄金例：FDId=2、MsgStart=`FEDCBA987654`、MsgCredit=`1234` 时，PH 为：

```text
42 65 70 98 34 12 A0 CB D0 FE
```

编码拒绝 FDId 超 2 bit、MsgStart 超 48 bit；解码拒绝非零 PH 保留位。
线格式支持四个 FDId；当前单桥 `FlitUnpacker` 只接收 FDId=0。

## 3. Format 6 物理布局

| 物理 byte | 内容 |
|---|---|
| 0..1 | FH B0/B1 |
| 2..61 | G0..G11 |
| 62..65 | PH B0..B3 |
| 66..125 | G12..G23 |
| 126..127 | C0 B0/B1 |
| 128..129 | PH B4/B5 |
| 130..189 | G24..G35 |
| 190..193 | PH B6..B9 |
| 194..253 | G36..G47 |
| 254..255 | C1 B0/B1 |

物理 256B = FH 2B + CRC 4B + PH 10B + 数据区 240B。250B PLP 在物理帧中
分散排列，不能从 byte2 连续复制 250B。

参考链路新增 `FlitFormat::AouFormat6`，CLI 名称 `aou256`；保留原 `Standard256`
和 `Compact68`。AoU FDI payload 必须恰好 250B，包括纯 credit Flit。

字节位置符合 AoU 图 4；FH 仍使用参考模型的 seq/replay 抽象，CRC 仍采用该模型的
CRC16-CCITT（初值 FFFF，多项式 1021）。C0 覆盖 byte0..125，C1 覆盖 byte128..253，
高字节在前。**这是行为链路的明确约定，不是完整 UCIe DLL/FH/CRC 标准实现声明。**
后续替换标准 CRC 算法不应改变 250B FDI 类型和 PH/载荷的位置。

## 4. 接收定界与错误行为

每个方向保存一条未完成消息及已收粒度数。接收时先补 G0 续传，再扫描 MsgStart。
消息长度由首字节推导；其余 MsgStart 为 0 的粒度可为空，不检查空闲字节的值。
无 carry 且 MsgStart=0 表示无 payload 消息，有 carry 则表示续传；纯 credit Flit
不能插在必须连续续传的两个片段之间。

解析器拒绝重叠起点、保留 DLENGTH=3、未知 MSGTYPE/MISCOP；检查成功才提交
续传状态。`FlitUnpacker` 在完整结构检查后再处理 header credit 和响应。
credit事件采用待发送队列和非阻塞FIFO写；事件未发完时仍占用holding并撤销ready，
覆盖RP4合法多CrdtGrant一次产生80条事件、超过内部64项FIFO深度的情况。
错误采用异常/`SC_REPORT_FATAL` 定位，CRC/重放由链路负责，不由协议层猜测恢复。
激活消息格式可识别，但当前桥不执行 ACTIVATE/DEACTIVATE 状态机。

## 5. 接线和时序

| AXI2Flit/链路信号 | Endpoint 端口 | 通道类型 |
|---|---|---|
| `Axi2Flit.flit_out` | `tx` | `sc_signal<FlitTransfer>` |
| `Axi2Flit.flit_ready` | `tx_ready` | `sc_signal<bool>` |
| `Axi2Flit.flit_in` | `rx` | `sc_signal<FlitTransfer>` |
| `Axi2Flit.flit_in_ready` | `rx_ready` | `sc_signal<bool>` |
| `UcieLink.soc_tx_in` | `fifo_tx` | 同一个 `sc_fifo<FdiFlit>` |
| `UcieLink.soc_rx_out` | `fifo_rx` | 同一个 `sc_fifo<FdiFlit>` |
| `UcieLink.link_state` | `link_state` | `sc_signal<unsigned>`，编码为 LinkState |
| AXI 时钟与复位 | `clk/rst_n` | `sc_signal<bool>` / `sc_clock` |

TX 按寄存 ready 预约 FIFO 空位，在握手沿直接写入，无新增 TX 流水周期。RX 在
时钟沿从 FIFO 取出，使用一个 holding register，最快下一 AXI 上升沿握手。
因此 RX 有一个 AXI 周期，另加 FIFO/时钟相位等待；不能把整个适配器称为零延迟。
ready 一旦公布必须兑现；链路状态同拍改变时最多还有一次已预约的 TX 握手。

Reset/Training 不接受新 Flit；Active/Degraded 允许传输。启动时先保持桥复位，
训练完成后释放，随后发布初始 credit。`transaction_id` 单调递增，仅用于跟踪，
VC=0；入站不依赖 ID/VC/kind 元数据解析协议。全部协议语义来自 250B payload。

## 6. 参数、复位与存储端契约

`link_config.h` 是速率配置源，默认 x16、24G、NRZ、48 GB/s、TAT=40ns。
支持 `AOU_LINK_LANES`、`AOU_LINK_RATE_GTPS`、`AOU_LINK_BITS_PER_SYMBOL`、
`AOU_LINK_TAT_NS` 编译参数。`make_aou_ucie_config()` 生成对应运行期配置，
`require_aou_ucie_config()` 拒绝不匹配的格式、lane、速率和调制。
修改宏必须重编桥与适配器；`preflight` 的回归目标每次重新编译。

AoU credit 按接收消息 FIFO 容量计粒度，FDI FIFO 按 Flit 计数，retry buffer
按未确认 Flit 计数，三者独立。默认 credit 环路预算为 40ns+3×5.333ns=56ns。
联调需测量真实 credit 往返和排队时间，再校准预算；ACK feedback_ui 不是 AoU TAT。

本地复位清空五类消息 FIFO、写路由、credit 事件、顺序表和 packer/unpacker
局部状态，重新发布初始容量。测试对端同步丢弃旧事务后可恢复；由于 UcieLink
无 reset 端口，接上链路后仍不支持单端热复位或运行期速率切换。

AXI 请求在握手入口检查 INCR、SIZE 上限、按 SIZE 对齐和 4KB 边界；WLAST
必须与 AWLEN 一致。违例 fail-fast，不继续使用错误写路由。窄传输保留原 lane
和 strobe，不做 beat 压缩，其内存语义由下一阶段目标端验证。

存储接口定义在 `simple_mem_if.h`：`SimpleMemRequest` 包含方向、RP、完整地址
属性和整 burst 写数据；`SimpleMemResponse` 包含 ID/RP、B 状态或逐 beat R 数据。
单请求最大 256 beat，同时受 4KB 约束；存储端仍须限制 outstanding 数与总缓存。
存储解包、实际读写模型、WREQ/WDATA 配对和 WLAST/RLAST 重建属于下一阶段实现。
