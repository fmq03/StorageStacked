# AXI4 → HETTrace 参考边界

这个目录保留一个很小的 RTL 接口回归：`xpu_axi_master` 发出的 AXI4 请求穿过完全透明的
`storage_chain_top`，旁路 monitor 将已握手的 AW/W/B/AR/R 暴露为 HETTrace v2 字段。checker
同时验证五个通道在背压时保持 `VALID` 和 payload。

这里不再包含 UCIe、memory controller、DFI 或内存时序占位模型。那些固定延迟模块容易被误解
成目标协议/性能实现；当前项目的存储时序路径是：

```text
XPU/gem5 → 统一 memory-side AXI4 HETTrace
         → hettrace convert --preset memsim
         → 外部 mem_sim/hbm_sim
```

## 运行

```bash
make -C storage_chain test lint
# 或在仓库根目录
make test-storage-chain
```

成功输出：

```text
AXI TRACE PASS: 6 transactions, 15 AW/W/B/AR/R events, 3 sources
PASS boundary is transparent; WSTRB, ID, USER, RESP and backpressure preserved
```

回归包含 host/Vortex/CoralNPU 三个 `AxUSER` 来源、全写和部分 `WSTRB`、读回、ID/RESP/LAST、
地址/数据通道背压，以及每源 `seq` 和 `txn` 关联。

## 文件与边界

| 文件 | 作用 |
|---|---|
| `rtl/xpu_axi_master.sv` | 单 outstanding 的参考 XPU command→AXI4 master |
| `rtl/axi_subset_checker.sv` | 五通道 ready/valid payload 稳定性检查 |
| `rtl/axi_hettrace_monitor.sv` | 被动输出 HETTrace v2 事件字段；不记录 WDATA/RDATA |
| `rtl/storage_chain_top.sv` | 透明 AXI4 passthrough + checker + monitor |
| `tb/tb_storage_chain.sv` | 功能 BFM 与字段/计数 scoreboard；BFM 延迟不代表存储性能 |

参考 monitor 当前只覆盖单 outstanding read、单 outstanding write，并要求同一周期最多一个通道
握手；这是明确受限的集成样例，不是完整 AXI VIP。生产路径的 gem5 `HetAxiMonitor` 支持多个
packet outstanding，并将 host/Vortex 的推导字段标为 `SYNTH`。HETTrace 没有 WDATA，因此功能
正确性依然要靠真实数据通路/scoreboard，外部 hbm_sim 只给固定请求流的 open-loop 存储时序。
