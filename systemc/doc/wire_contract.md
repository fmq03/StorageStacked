# 字节格式与链路接口

本文定义项目使用的消息字段、250 字节逻辑内容、256 字节物理帧以及模块接线。字段值和位序以项目编码器及独立黄金向量为准，运行方法见[验证文档](verification.md)。

这是桥与 UCIe 之间的内部传输格式。接入主机和存储模型分别使用[SoC 侧接口表](../../doc/SoC侧接口对接表.md)和[存储侧接口表](../../doc/存储侧接口对接表.md)；两侧开发者无须自行生成本节描述的帧头、credit 消息或 CRC。

## 1. 类型与表示

| 类型或函数 | 用途 |
|---|---|
| `AouMessage` | 编码后的单条消息及本地辅助信息 |
| `AouFlit` / `FlitTransfer` | 桥内部帧及有效信号表示 |
| `AouWireFlit` | 固定 `std::array<uint8_t,250>`，只包含需要传输的字节 |
| `serialize_aou` / `deserialize_aou` | 帧头及 240 字节载荷的无状态编解码 |
| `AouStreamDecoder` | 根据起点和首字节重组跨帧消息 |
| `aou_format6::scatter/gather` | 250 字节逻辑内容与物理帧散布布局的转换 |

逻辑字节顺序固定为 `[PH B0..B9][G0..G47]`，每个粒度 G 包含 5 字节。不得用结构体内存布局、`sizeof(AouFlit)` 或结构体直接复制代替序列化。

`AouFlit::valid` 由外层握手或 FIFO 操作表达，不进入字节流；`used_granules` 仅供发送组包和统计使用，反序列化后设为 -1。消息 ID、地址等辅助字段也不能作为接收定界依据。

## 2. 消息字段

消息按下表从左到右写入，标量字段先写最高有效位，写入当前字节的高位。`0(n)` 表示 n 个零保留位。数据数组以字节通道 0 为低下标，线上先写最高下标的字节；解码时还原该顺序。

| 消息 | 顺序及位数 |
|---|---|
| 写请求 / 读请求 | 类型(4)、RP(2)、0(1)、LOCK(1)、USER(16)、ID(10)、SIZE(3)、PROT(3)、LEN(8)、CACHE(4)、QOS(4)、ADDR(64) |
| 写数据 | 类型(4)、RP(2)、DLENGTH(2)、USER(16)、DATA、STRB 位图、补零 |
| 全字节有效写数据 | 类型(4)、RP(2)、DLENGTH(2)、USER(16)、DATA、补零 |
| 读数据 | 类型(4)、RP(2)、DLENGTH(2)、USER(16)、ID(10)、RESP(2)、LAST(1)、0(3)、DATA、补零 |
| 写响应 | 类型(4)、RP(2)、0(2)、USER(16)、ID(10)、RESP(2)、0(4) |

USER 在消息中占 16 位，也称 FLEX。请求的 64 位地址占消息字节 7～14，最高地址字节先写。请求不传输 BURST，接收端恢复为 INCR；写数据不传输 WLAST，响应端根据写请求 LEN 组装整笔突发。写端入口会核对实际 WLAST。

类型编码分别为 Misc=0、写请求=1、读请求=2、写数据=3、读数据=4、写响应=5、全字节有效写数据=6。数据消息的 DLENGTH 为 0/1/2，分别表示 256/512/1024 位，编码 3 保留。接收数据宽度须与本地编译配置一致。

| 消息 | 256 位粒度数 | 512 位粒度数 | 1024 位粒度数 |
|---|---:|---:|---:|
| 写请求 / 读请求 | 3 | 3 | 3 |
| 写数据 | 8 | 15 | 30 |
| 全字节有效写数据 | 7 | 14 | 27 |
| 读数据 | 8 | 14 | 27 |
| 写响应 | 1 | 1 | 1 |
| 专用 credit 消息 | 2 | 2 | 2 |

STRB 位图的字节 i 中 bit j 对应数据字节 `i×8+j`，位图本身也按高字节先写。只有全部总线字节的选通均有效时才能选择全字节有效写数据格式；窄访问仍保留完整总线宽度的数据数组。

## 3. 帧头 PH

下表的 bit0 均表示当前字节最低有效位；PH 的字节映射独立于消息字段的写入顺序。

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

编码拒绝超出 2 位的 FDId 和超出 48 位的 MsgStart；解码拒绝非零 PH 保留位。字节格式可表达四个 FDId，桥接收端及存储响应端只接受 FDId=0。

## 4. credit 编码

credit 表示接收端可用的消息缓冲容量，1 credit 对应 1 个 5 字节粒度。每个 3 位编码分别代表 `{0,1,4,8,16,32,64,128}` 个 credit。WRESP 只用 2 位，最大可表示 8 credit。剩余 credit 留在待归还计数中，分次发布，不向上取整授予容量。

帧头 `MsgCredit` 使用以下位段：

| 位段 | 内容 |
|---|---|
| 2:0 | WREQ credit 编码 |
| 5:3 | RREQ credit 编码 |
| 8:6 | WDATA credit 编码 |
| 11:9 | RDATA credit 编码 |
| 13:12 | WRESP credit 编码 |
| 15:14 | RP |

专用 `CrdtGrant` 消息依次写 Misc 类型 4 位、操作码 `100` 共 3 位，再按 WREQ、RREQ、WDATA、RDATA 类型顺序，分别写 RP0～RP3 的 3 位 credit 字段；之后写 RP0～RP3 的 2 位 WRESP 字段，末尾补 17 个零位，共 80 位。未启用 RP 的字段为零。

操作码 `000` 的激活消息长度可被解析器识别为 1 粒度，但桥与响应端不执行激活状态机。credit 初始值、消耗与回收时刻见[设计文档](design.md)。

## 5. 物理帧布局

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

链路选择 `FlitFormat::AouFormat6`，命令行名称为 `aou256`。FDI 的 `payload.size()` 与 `valid_bytes` 都必须为 250，包括只有 credit 信息的帧。

FH 字节 0 保存序号低 8 位，字节 1 的 bit0 表示重放标志。两段校验使用 CRC16-CCITT，初值 `FFFF`、多项式 `1021`；C0 覆盖物理字节 0～125，C1 覆盖 128～253，校验值高字节先写。这里描述的是链路行为模型的帧头和校验实现。

## 6. 定界与接收错误

`MsgStart[i]=1` 表示粒度 i 是一条新消息的起点，长度由该消息首字节确定。每个接收方向单独保存一条未完成消息及已接收粒度数。存在续传时先从下一帧 G0 补齐，不在 G0 设置新起点，再扫描后续起点。

不存在续传且 MsgStart 为零时，载荷没有消息；空粒度的字节值可以非零。存在续传时不能插入纯 credit 帧来中断消息，credit 可随续传帧的帧头传递。

解析器检查重叠起点、保留长度编码及未知类型或操作码，通过结构检查后才提交续传状态。桥解包器在完整结构检查后处理 credit 和响应；credit 事件队列未排空时保持帧占用并撤销接收就绪。解析异常由计数、异常或致命报告定位；链路 CRC 和重放由链路处理。

## 7. 信号与 FIFO 连接

| AXI2Flit/链路信号 | Adapter 端口 | 通道类型 |
|---|---|---|
| `Axi2Flit.flit_out` | `tx` | `sc_signal<FlitTransfer>` |
| `Axi2Flit.flit_ready` | `tx_ready` | `sc_signal<bool>` |
| `Axi2Flit.flit_in` | `rx` | `sc_signal<FlitTransfer>` |
| `Axi2Flit.flit_in_ready` | `rx_ready` | `sc_signal<bool>` |
| `UcieLink.soc_tx_in` | `fifo_tx` | 同一个 `sc_fifo<FdiFlit>` |
| `UcieLink.soc_rx_out` | `fifo_rx` | 同一个 `sc_fifo<FdiFlit>` |
| `UcieLink.link_state` | `link_state` | `sc_signal<unsigned>`，编码为 LinkState |
| AXI 时钟与复位 | `clk/rst_n` | `sc_signal<bool>` / `sc_clock` |

发送端按寄存就绪信号预约 FIFO 槽位，在握手沿直接写入；接收端从 FIFO 取出后保持至最快下一个 AXI 上升沿。发送就绪一旦公布须兑现，链路状态切换时可能完成一次已预约的传输。

存储侧直接绑定以下接口：

| 响应端端口 | 连接对象 | 公共类型 |
|---|---|---|
| `link_rx` | `UcieLink.mem_rx_out` | `sc_fifo<FdiFlit>` |
| `link_tx` | `UcieLink.mem_tx_in` | `sc_fifo<FdiFlit>` |
| `mem_req` | `SimpleBurstMemory.request` 或后端请求入口 | `sc_fifo<SimpleMemRequest>` |
| `mem_rsp` | `SimpleBurstMemory.response` 或后端响应出口 | `sc_fifo<SimpleMemResponse>` |

这些 FIFO 本身表达接受和背压。成功写入仅表示该项被队列接收；内存响应表示后端达到约定的完成点，随后还要经过链路和 AXI 响应握手。后端保持同方向、同 RP、同 ID 的完成顺序，读响应聚合整笔突发。上表后端入口必须提供相同 C++ 类型，不同模型的 Request/Response 需经包装层转换。

链路状态为 Reset/Training 时停止接受新帧，Active/Degraded 时允许传输。启动时训练完成后释放桥和响应端复位。链路不提供运行期间的统一复位接口，因而不支持单端热复位。配置、对外 AXI 字段和存储语义见[设计文档](design.md)，交付路径设置见[接入说明](../integration/README.md)。
