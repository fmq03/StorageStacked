# `storage_chain/`：AXI4 观察边界参考

`storage_chain/` 现在只验证一个明确边界：XPU master 的 AXI4 五通道穿过透明 passthrough，
checker 检查反压稳定性，被动 monitor 输出与 HETTrace v2 对应的字段。

```text
xpu_axi_master ──AW/W/B/AR/R──> storage_chain_top ──> testbench memory BFM
                                      │
                                      ├─ axi_subset_checker
                                      └─ axi_hettrace_monitor
```

运行：

```bash
make -C storage_chain test lint
```

测试覆盖三个 `AxUSER` 来源、六笔读写事务、全写/部分 WSTRB、ID、RESP、LAST、地址/数据通道
背压，以及每源 `seq`、`txn` 关联。成功时输出：

```text
AXI TRACE PASS: 6 transactions, 15 AW/W/B/AR/R events, 3 sources
PASS boundary is transparent; WSTRB, ID, USER, RESP and backpressure preserved
```

| 模块 | 职责 |
|---|---|
| `xpu_axi_master.sv` | 单 outstanding command→AXI4 示例 master |
| `axi_subset_checker.sv` | 检查 VALID 等待 READY 时 payload 保持稳定 |
| `axi_hettrace_monitor.sv` | 旁路生成 HETTrace 对应事件字段 |
| `storage_chain_top.sv` | 透明连线并组合 checker/monitor |
| `tb_storage_chain.sv` | memory BFM 与 scoreboard；延迟仅为测试激励 |

参考 monitor 有意受限为每方向单 outstanding，并要求同周期最多一个通道握手；它不是完整 AXI
VIP。正式 gem5 路径使用 C++ `HetAxiMonitor`，支持多个 packet outstanding。RTL 和 HETTrace
都不保存 WDATA/RDATA，所以数据正确性由 testbench scoreboard 或真实 workload 自检证明。

这个目录不再放置固定延迟的链路、memory controller 或 PHY 占位模型。若未来要接 UCIe/DFI，
应作为独立、标准明确且有协议级验证的新项目加入，不能把简单 delay queue 当作实现或性能模型。
