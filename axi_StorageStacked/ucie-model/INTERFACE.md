# UcieLink SystemC 事务级接口说明

## 1. 模块边界

`UcieLink` 定义在 `src/ucie_link.h`，是可独立实例化的双向 UCIe 行为级链路：

```text
soc_tx_in  -> FWD TxAdapter -> FWD PHY -> FWD RxAdapter -> mem_rx_out
mem_tx_in  -> REV TxAdapter -> REV PHY -> REV RxAdapter -> soc_rx_out
```

正向和反向链路分别维护序号、Retry Buffer、ACK/NAK、信道随机流和 CDR 状态。完整链路
`Frame`、CRC、replay 标志和反馈消息均为模块内部实现，不跨越公开端口。

## 2. 公开端口

端口方向均以 `UcieLink` 为参照：

| 端口 | SystemC 类型 | 方向 | 含义 |
| --- | --- | --- | --- |
| `soc_tx_in` | `sc_fifo_in<FdiFlit>` | 输入 | SoC 侧向存储侧发送的事务 |
| `mem_rx_out` | `sc_fifo_out<FdiFlit>` | 输出 | 存储侧收到的事务 |
| `mem_tx_in` | `sc_fifo_in<FdiFlit>` | 输入 | 存储侧向 SoC 侧返回的事务 |
| `soc_rx_out` | `sc_fifo_out<FdiFlit>` | 输出 | SoC 侧收到的返回事务 |
| `link_state` | `sc_out<unsigned>` | 输出 | `LinkState` 数值编码 |

四个数据端口统一使用 `FdiFlit`，不存在此前 TX 为 `FdiFlit`、RX 为内部 `Frame` 的不对称。

## 3. FdiFlit 定义

定义位于 `src/ucie_fdi.h`：

```cpp
struct FdiFlit {
    std::vector<std::uint8_t> payload;
    std::size_t valid_bytes;
    std::uint64_t transaction_id;
    std::uint8_t vc;
    BusinessKind kind;              // Request / Response
    sc_core::sc_time transaction_start;
    sc_core::sc_time fdi_time;
};
```

字段约束：

- `payload.size()` 必须等于当前格式的固定 Payload 容量：Standard256 为 236 B，Compact68 为 64 B；
- `valid_bytes <= payload.size()`，短事务的其余字节补零；
- `transaction_id`、`vc`、`kind` 和时间戳是事务级仿真元数据；
- 当前只有 `payload` 被封装进 `Frame.bytes` 并经过 PAM4/NRZ 和信道损伤；
- RX 完成 CRC、序号检查及必要重传后，去除链路 Header/CRC，并重新输出 `FdiFlit`。

## 4. 背压和流控

公开接口使用有界 `sc_fifo<FdiFlit>`。FIFO 满时 `write()` 阻塞，或 producer 等待
`data_read_event()`；这等价于事务级 ready/valid 背压。默认 SoC TX 和 Memory TX 的 FIFO 深度由
`fdi_queue_size=64` 配置。链路内部 Retry Buffer 默认 64 Flit、最大 127 Flit；缓存满时 TX 停止
读取对应输入 FIFO，背压逐级返回上游。

`fdi_backpressure_events` 统计测试源因公开 FIFO 满而暂停的次数；
`retry_buffer_full_events` 和 `max_retry_buffer_occupancy` 描述链路可靠性缓存压力。

## 5. 链路状态

`link_state` 输出以下 `LinkState` 数值：

| 数值 | 状态 | 定义 |
| ---: | --- | --- |
| 0 | `Reset` | SystemC 初始 delta-cycle 状态 |
| 1 | `Training` | 初始 `train_ui` 训练窗口 |
| 2 | `Active` | 训练完成，可正常传输 |
| 3 | `Degraded` | 任一方向观察到 CDR 失锁或 deskew 失败 |
| 4 | `Failed` | Watchdog 到期，业务未完成 |

状态输出使用 `unsigned` 是为了兼容标准 SystemC trace；含义由 `LinkState` 枚举唯一规定。

## 6. 实例化示例

```cpp
sc_core::sc_fifo<FdiFlit> soc_tx(64), soc_rx(16);
sc_core::sc_fifo<FdiFlit> mem_tx(64), mem_rx(16);
sc_core::sc_signal<unsigned> state;

Stats stats;
Config cfg;
sc_core::sc_time ui(cfg.ui_fs(), sc_core::SC_FS);
UcieLink link("ucie_link", cfg, &stats, ui);

link.soc_tx_in(soc_tx);
link.soc_rx_out(soc_rx);
link.mem_tx_in(mem_tx);
link.mem_rx_out(mem_rx);
link.link_state(state);
```

本接口是链路性能验证使用的事务级 FDI，并非 UCIe 规范逐信号 `lp_data/pl_data/irdy/trdy`
接口；如后续进行 RTL 对接，需另加信号级 transactor，但不会改变这里的链路内部模型。
