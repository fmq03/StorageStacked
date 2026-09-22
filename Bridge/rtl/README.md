# Bridge RTL

本目录实现 `AouTarget` 的链路终结职责，以及原 `MemSimBackend` 中与 burst
展开、lane/mask 转换相关的协议适配职责。窗口检查、mem_sim C ABI 和时间推进仍由
SystemC/C++ transactor 承担，不属于 RTL。当前代码已经形成可由 Verilator 执行的
256-bit 功能闭环：

```text
250B FDI PLP
  -> fdi_rx_unpack
  -> aou_msg_decode
  -> bridge_req_buffer
  -> bridge_scheduler
  -> bridge_read_fsm / bridge_write_fsm
  -> mc_req -> bridge_outstanding -> mc_rsp
  -> bridge_reorder_buffer
  -> bridge_response_gen
  -> fdi_tx_packer
  -> 250B FDI PLP
```

`bridge_transaction_engine` 连接并编排上述事务模块；`bridge_credit_mgr` 与数据通路
并行管理每 RP credit。最初实施方案列出的 15 个文件均已落地，另增加
`storage_bridge_rx.sv` 和 `bridge_transaction_engine.sv` 两个层次组织模块。

## 当前模块

当前实现的顶层连接、事务引擎展开、缓冲容量和 credit 时机见
[RTL 内部详细结构图](../doc/RTL_STRUCTURE.md)。附 SVG/PNG 和可编辑图源。

| 文件 | 作用 |
|---|---|
| `bridge_pkg.sv` | AoU 消息类型、粒度、错误码和固定宽度 |
| `fdi_rx_unpack.sv` | PLP 头解析、MsgStart 扫描、跨 PLP 消息重组 |
| `aou_msg_decode.sv` | 请求字段、WDATA 和逐字节 mask 解码 |
| `storage_bridge_rx.sv` | RX 解包与消息解码组合层 |
| `bridge_credit_mgr.sv` | 每 RP 初始 credit、归还、header/CrdtGrant 接收和响应准入 |
| `bridge_fifo.sv` | 参数化 ready/valid 同步 FIFO |
| `bridge_req_buffer.sv` | 每 RP Request/WData FIFO 和队首字段选择 |
| `bridge_scheduler.sv` | 请求分配、读写发射、WData 配对及响应 RP 轮询 |
| `bridge_read_fsm.sv` | 组合计算读 burst 的下一笔 32B 边界事务 |
| `bridge_write_fsm.sv` | 写事务生成、lane/mask 提取和窄 beat chunk 聚合 |
| `bridge_outstanding.sv` | child tag 分配、在途计数和 MC completion 匹配 |
| `bridge_reorder_buffer.sv` | 紧凑读数据散布、逐 beat 数据与完成位存储；响应顺序由每 RP order 队列保证 |
| `bridge_transaction_engine.sv` | parent/order 状态、MC issue 寄存器和各子模块编排 |
| `bridge_response_gen.sv` | 将已完成 parent 编码为 ReadData 或 WriteResp |
| `bridge_assertions.sv` | MC/response 稳定性、容量、事务边界和 tag 唯一性断言 |
| `fdi_tx_packer.sv` | 多消息聚合、跨 PLP spill、credit 捎带或 credit-only PLP |
| `storage_bridge_top.sv` | 固定的 FDI 与内存控制器 req/rsp 顶层接口 |

## 已验证行为

当前基线为 `rtl-lint` 0 warning、`rtl-test` 174 项 `[PASS]`、`rtl-memsim-test`
6 项 `[PASS]`；`rtl-diff` 的 12 个场景均与 C++ 参考路径计数完全一致。

- 同一个 PLP 中连续多条消息；
- 从 granule 45 开始、跨两个 PLP 的 WriteData 重组；
- ReadReq/WriteReq 的 user、ID、size、len、QoS 和 64-bit 地址解码；
- WriteDataFull 的 256-bit lane 顺序和 32-bit byte mask；
- 16B 窄读到紧凑 MC 请求、读回数据到 AXI lane 的反向散布；
- 两拍读的地址推进、RDATA 和 RLAST；
- 32B 写事务和 WriteResp；
- 8 个 parent outstanding、8 个 MC child inflight、唯一 tag 和乱序完成重排；
- 独立 16-entry WData FIFO，可在对应 WriteReq 尚未到达时先接收数据，避免 RX
  队头死锁；
- 每 RP 4-entry Request FIFO，先吸收链路请求，再轮询分配共享 parent 槽；
- FDI RX、MC request、响应消息及 FDI TX 的 valid/ready 稳定性断言；
- 初始 WREQ/RREQ/WDATA credit、归还和响应 credit 门控。
- 请求 credit 在首笔 MC 握手后归还，WriteData credit 在数据落入 parent chunk 后归还；
- RP0/RP1 独立队列、credit 和响应调度，受阻 RP 不阻塞另一个 RP；
- 连续响应聚合到同一 PLP，以及响应跨 PLP 的 spill/continuation；
- 8 个相邻 4B beat 合并成一笔 32B MC transaction；
- RTL 请求真实进入 HBM4 mem_sim 后完成写回读，并逐字节一致；
- AXI2Flit/UCIe/RTL Bridge/mem_sim 的 12 项联合回归全部通过；
- `rtl-diff` 与 C++ 参考路径的 MC 请求、父错误和 AXI 五通道计数完全一致。

运行：

```bash
make -C Bridge rtl-lint
make -C Bridge rtl-test
make -C Bridge rtl-memsim-test
make -C Bridge rtl-full-link
make -C Bridge rtl-diff
RTL_BRIDGE=1 bash Bridge/scripts/run_mock_chain.sh
```

`rtl-test` 使用 Verilator C++ testbench，不链接 SystemC，因此不会引入第二套
SystemC 内核。生成目录为 `Bridge/build/obj_rtl_rx` 和 `obj_rtl_top`。

## 当前限制

当前版本是连续实施方案 P0-P7 的 256-bit 功能实现，尚不是最终综合版本：

- 固定支持 256-bit AoU data 和 32B mem_sim transaction；512/1024-bit 尚未实现；
- 默认 RP_COUNT=1，定向回归覆盖 RP_COUNT=2；gem5 模型按 RP_COUNT=4 构建并按运行时
  `planes` 使用，四 RP 系统压力回归尚未完成；
- 窗口 base/size 由 SystemC transactor 检查，4KB/对齐错误由 RTL 合成响应；
- 只验证启动复位，不支持有在途事务时的热复位；
- CommMonitor socket 入口已有 RTL 切换并通过 mock server；主仓库 gem5 原生
  SystemC `gem5_axi::AouBackend` 也已通过 `--bridge-impl rtl` 跑通 tester + mem_sim。
  双 RP NPU、GPU、三源及四倍 mem_sim 时间尺度场景也已通过完整验收。

XPU 验收结果位于 `results/acceptance-xpu-rtl-final-20260922`：四个场景均排空，
0 个 CRC/顺序/DFI 错误，mem_sim 子事务提交与返回分别为 320、9673、9949、9949。
GPU/三源重负载会因 FDI 接收背压触发 UCIe timeout replay；离线链路校验确认这些帧
全部是重复帧、没有重复业务消息或丢失事务。

gem5 原生 transactor 保持 1/2/4/8/16/32B chunk 的原地址和长度；仅对 AxSIZE 无法
表示的 12/20/24B 等紧凑 chunk 使用对齐 32B `SimpleMemRequest` 容器，实际有效范围由
lane、bytes 和写 strobe 保存。这个容器是模型复用细节，不改变顶层 MC 端口契约。

后续扩展必须保持 `storage_bridge_top.sv` 的 FDI 和 MC req/rsp 端口兼容。实现顺序
继续以 [RTL_IMPLEMENTATION_PLAN.md](../RTL_IMPLEMENTATION_PLAN.md) 为准。
