# AXI2FLIT SystemC 模型设计

## 1. 设计范围

本模型是 AoU initiator 侧的双向协议桥：

```text
发送：AXI AW/W/AR → AoU WriteReq/WriteData/ReadReq → FDI
接收：FDI → AoU WriteResp/ReadData → AXI B/R
```

实现目标是验证消息打包、FDI 背压、per-message-type credit 和多 Resource Plane 隔离。当前 AXI 数据宽度为 256bit，完整链路激活状态机与跨 Flit 消息续传不在本阶段范围内。

## 2. 模块与数据流

```text
                         Axi2Flit
 ┌─────────────────────────────────────────────────────────────────┐
 │ AW/W/AR 线程 → per-RP TX FIFO → FlitPacker → flit_out       │
 │                                     ↑ credit_update          │
 │                                     ↓ MsgCredit/CrdtGrant    │
 │ flit_in → FlitUnpacker → per-RP RX FIFO → B/R 线程          │
 │                                            └→ credit_return  │
 └─────────────────────────────────────────────────────────────────┘
```

### 2.1 FlitPacker

- 每个 RP 拥有独立的 RREQ/WREQ/WDATA 输入队列。
- `sc_fifo` 无 peek 接口，因此每个 `[RP][类型]` 设置一个 staging slot；只有 credit 足够时才会把 staging 消息放入 Flit。
- 一条消息开始打包时，一次扣除其全部 granule credit。
- 消息类型优先级保留为 `ReadReq > WriteReq > WriteData`；同类型内按 RP 轮询。
- 水位、满载或 4 周期超时触发 flush。
- `flit_ready=0` 时保持 `flit_out.valid`、Protocol Header 和 payload 不变。

### 2.2 FlitUnpacker

- 使用一个 Flit holding register 解耦 FDI 接收与内部 FIFO 时序。
- 解析 `MsgCredit`，将对端 grant 送入 Packer 的 Tx credit counter。
- 根据 `MsgStart[47:0]` 切分消息，并按 RP 分流到 RDATA/WRESP FIFO。
- 解析 Misc/CrdtGrant；initiator 侧未实现的入站 WREQ/RREQ/WDATA 被视为协议异常。

### 2.3 AXI 五通道线程

- AW/AR：使用 `AxQOS % RP_COUNT` 映射 RP。
- W：AXI4 无 WID，通过 AW 顺序路由队列继承 RP，并检查 AWLEN/WLAST 是否一致。
- B/R：解码后保持 valid 和数据，直到 AXI `valid && ready`；握手后才释放 FIFO 对应的 granule credit。

## 3. Credit 模型

### 3.1 计数维度

Credit counter 的索引为：

```text
[RP][WREQ | RREQ | WDATA | RDATA | WRESP]
```

1 credit 等于指定类型的 1 个 5B granule 接收空间。Misc 不消耗 credit。`WriteData` 和 `WriteDataFull` 共用 WDATA credit pool。

### 3.2 接收 FIFO 与初始 credit

| 类型 | 每 RP FIFO 深度 | 单消息 granule | 初始 credit/RP |
|---|---:|---:|---:|
| ReadData256 | 4 | 8 | 32 |
| WriteResp | 4 | 2 | 8 |

两类 FIFO 为定长消息，因此 `FIFO entry 数 × 单消息 granule` 可与 credit 容量一一对应。如果后续在同一 RDATA FIFO 中混用 256/512/1024bit，必须改为 granule 容量管理，或对不同长度分池，不能继续用固定乘法。

### 3.3 Credit 分发

1. 复位释放后发送 2-granule `CrdtGrant`，公布各 RP 的初始 RDATA/WRESP 容量。
2. 平时优先在业务 Flit 的 `MsgCredit[15:0]` 中捎带归还。单个 header 只服务一个 RP，多 RP 间轮询。
3. 无业务 Flit 可捎带时，2 周期后发 dedicated `CrdtGrant`。
4. Table 17 的 grant 值为 `0/1/4/8/16/32/64/128`。如待归还值为 2，会分两次各归还 1。

## 4. RP 参数化

```cpp
Axi2Flit single_rp("bridge");     // 默认 RP_COUNT=1
Axi2Flit qos_bridge("bridge", 2); // 启用 RP0/RP1
```

`RP_COUNT` 范围为 1～4。发送 FIFO、接收 FIFO、credit counter 和 Packer/Unpacker 端口数量均由该参数构造。

对于单一 HBM 交互场景，RP_COUNT=1 通常足够。RP 不是读/写通道编号；RP0 内部仍然有五套独立 credit，因此读写消息不会因共用 RP0 而共用同一 credit pool。

## 5. 文件划分

| 文件 | 职责 |
|---|---|
| `aou_types.h` | AoU 消息、Flit 和协议常量 |
| `credit_manager.h` | credit counter、MsgCredit/CrdtGrant 编解码 |
| `msg_builder.h` / `msg_decoder.h` | AXI 与 AoU 消息互转 |
| `flit_packer.*` | 多 RP 调度、打包、credit 消耗与 FDI 发送 |
| `flit_unpacker.*` | FDI 接收、header 解析、R/B 分流 |
| `axi2flit.*` | 顶层连接和 AXI 五通道握手 |
| `tb_axi2flit.cpp` | 双向自检测试与波形跟踪 |

## 6. 已知边界

- 只实现 initiator 侧所需的入站 RDATA/WRESP；若要建模 responder 侧，需对称增加入站 WREQ/RREQ/WDATA 处理。
- 当前 Packer 不拆分消息，放不下时先 flush；Unpacker 也不接收跨 Flit 续传消息。
- 字段排布仍是用于性能建模的可读子集；RTL 实现前必须按冻结版 AoU 规范逐 bit 对齐。

