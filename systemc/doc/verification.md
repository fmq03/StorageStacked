# AXI2FLIT SystemC 验证说明

## 1. 运行方法

```bash
cd /home/cfy/workspace/axi2flit/systemc
make clean
make
make run
```

测试是自检的：每个检查打印 `[PASS]` 或 `[FAIL]`，任一失败会使进程返回非 0。波形写入 `sim/waveform.vcd`。

## 2. 测试配置

| 项目 | 值 |
|---|---|
| 时钟 | 2ns |
| DUT RP_COUNT | 2（覆盖非默认参数） |
| RDATA FIFO/RP | 4 条 ReadData256 = 32 credit |
| WRESP FIFO/RP | 4 条 WriteResp = 8 credit |
| FDI 接口 | 双向 ready/valid |

Testbench 同时充当 AXI Master 和链路对端，因此能够控制对端 credit、FDI 背压及 AXI B/R 背压。

## 3. 覆盖场景

### TC1：初始 CrdtGrant

- 检查 DUT 复位后主动发送 CrdtGrant。
- 检查 RP0/RP1 均公布 32 RDATA credit 和 8 WRESP credit。
- 检查 initiator 侧未实现的入站 WREQ/RREQ/WDATA 不被授予接收 credit。

### TC2：AW/W 的 RP 继承

- 发送 `AWQOS=1` 的单 beat 写事务。
- 检查 WriteReq 进入 RP1，WriteDataFull 经 AW 路由队列继承 RP1。

### TC3：FDI 背压

- 将 `flit_ready` 拉低 3 周期。
- 检查 `flit_out.valid`、header 和 payload 全部保持。
- 拉高 ready 后只计数一次握手，防止重复发送。

### TC4：credit 耗尽和 RP 隔离

- RP0/RP1 的 RREQ credit 均为 0 时，请求只在本地队列等待。
- 只给 RP1 补充 4 credit，检查 RP1 可前进而 RP0 仍阻塞。
- 再给 RP0 补充 credit，检查其恢复发送。

### TC5：B/R 解包与 dedicated credit 归还

- 同一入站 Flit 携带 ReadData 和 WriteResp。
- `RREADY/BREADY=0` 时检查 valid 和数据保持。
- 握手后检查 8 RDATA credit 和 2 WRESP credit 完整归还。
- WRESP 的 2 credit 由于 Table 17 无编码 2，会以两个 grant=1 完成。

### TC6：MsgCredit 捎带

- 使业务 ReadReq Flit 与 R/B credit 释放时间重叠。
- 检查 RDATA credit 优先写入业务 Flit 的 `MsgCredit`，而不是必须额外发送 credit-only Flit。

## 4. 通过标准

一次完整运行应满足：

```text
errors: 0
ALL TESTS PASSED
```

同时编译器不应产生 warning。

## 5. 建议波形

| 分组 | 信号 |
|---|---|
| AXI 请求 | `axi.aw_*` / `axi.w_*` / `axi.ar_*` |
| AXI 响应 | `axi.b_*` / `axi.r_*` |
| FDI 发送 | `fdi.tx.valid` / `fdi.tx.msg_credit` / `fdi.tx_ready` |
| FDI 接收 | `fdi.rx.valid` / `fdi.rx.msg_credit` / `fdi.rx_ready` |

## 6. 尚需追加的验证

- 长时间随机 ready/valid 背压和随机 credit grant。
- 发送 credit 计数器饱和、复位中断事务和非法 RP 注入。
- 未来加入 512/1024bit 后的可变长度接收缓冲与 granule 容量检查。
- 实现跨 Flit 续传后的 MsgStart 首尾重组。

