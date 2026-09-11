# `src` 目录源码说明

本目录包含可复用的 UCIe SystemC 双向行为级链路模块、测试平台和单元测试。

| 文件 | 主要职责 | SystemC 依赖 |
| --- | --- | --- |
| `ucie_common.h` | 配置与 CLI、Flit/CRC、确定性 Payload、正反向统计结构 | 否 |
| `ucie_fdi.h` | 公开 `FdiFlit`、事务类型和链路状态定义 | 是 |
| `ucie_phy.h` | lane striping、PAM4/NRZ、信道损伤、判决、deskew 和 CDR | 否 |
| `ucie_link.h` | 独立双向 `UcieLink`、内部 Adapter/PHY/反馈连接 | 是 |
| `ucie_systemc_main.cpp` | 外部文件输入、测试端点、汇总和 `sc_main()` | 是 |
| `ucie_unit_tests.cpp` | CRC、编码、lane 映射、序号、CDR、deskew 黄金断言 | 是 |

依赖关系和业务数据流为：

```text
ucie_common.h <── ucie_phy.h <── ucie_link.h
       ^                            ^
ucie_fdi.h ─────────────────────────┘
       ^                            ^
       └──── ucie_systemc_main.cpp ─┘

Request:
FdiSource -> soc_tx_in -> UcieLink/FWD -> mem_rx_out -> MemoryResponder

Response:
MemoryResponder -> mem_tx_in -> UcieLink/REV -> soc_rx_out -> FdiSink
```

## 1. `ucie_common.h`

### `Config`

`Config` 是整个模型的配置中心，包含：

- 内部事务数、外部 `payload_file`、响应输出文件和存储响应延迟；
- Standard 256B/Compact 68B 格式；
- lane 数、单 lane 速率、PAM4/NRZ；
- FDI FIFO 与 Retry Buffer 深度；
- TX/RX/信道/反馈/训练时延；
- AWGN、jitter、ISI、skew、deskew 和额外错误注入；
- CDR 失锁与重新锁定参数；
- 随机种子、watchdog、CSV 和详细日志。

主要派生关系为：

```text
bits_per_ui = PAM4 ? 2 : 1
serialize_ui = ceil(flit_bits / (num_lanes * bits_per_ui))
ui_fs = 1e6 / lane_rate_gtps
flit_efficiency = payload_bytes / flit_bytes
```

`parse_args()` 解析命令行；`require_valid_config()` 检查 lane、速率、Flit 数和 Retry Buffer 半窗口约束。

### Flit 与 CRC

`build_flit()` 接收调用者提供的 Payload，加入 2B 链路 Header、预留字段和 CRC：

- Header byte 0：序号低 8 位；
- Header byte 1 bit 0：replay 标志；
- Standard256：236B Payload，两组 CRC-16/CCITT；
- Compact68：64B Payload，一组 CRC-16/CCITT。

调用者提供的 Payload 不得超过当前格式容量。外部短 Payload 会先补零到固定容量，再传给 `build_flit()`。

`check_flit()` 校验长度和 CRC，并解析 8-bit 序号及 replay 标志。顺序判断、重复包处理和 ACK/NAK 由 `RxAdapter` 完成。

`fill_payload()` 仍用于默认内部测试数据；外部文件模式不会用它替换调用者的数据。

### 统计

`LinkStats` 保存单个业务方向的 TX、Replay、ACK/NAK、错误、Retry Buffer 和 PHY 指标。`Stats` 同时保存：

- `forward` 与 `reverse` 两组 `LinkStats`；
- 请求到达、响应生成和响应交付；
- 完成的有效 Payload 字节；
- 正向、反向和 round-trip 延迟；
- 正向/反向及聚合完整性错误。

## 2. `ucie_phy.h`

`BehavioralPhy` 是不依赖 SystemC 的纯 C++ Flit 级 PHY。每个业务方向各实例化一个 PHY，反向 PHY 使用由主种子派生的独立随机流。

`transmit()` 的处理步骤为：

1. 字节 `i` round-robin 映射到 lane `i % num_lanes`；
2. PAM4 Gray 编码或 NRZ 编码；
3. 为各 lane 生成随机 skew；
4. 施加两阶后游标 ISI、AWGN 和 jitter；
5. PAM4 三门限或 NRZ 零门限判决；
6. de-stripe 重新组成字节；
7. 根据 deskew/额外注错配置翻转 bit；
8. 统计 symbol error 和 bit error。

信道等效式为：

```text
y[k] = h0*x[k] + h1*x[k-1] + h2*x[k-2]
       + AWGN + jitter_gain*jitter*(x[k]-x[k-1])
```

`observe()` 以 Flit 内 symbol error 比例更新 CDR 抽象；连续坏 Flit 达到门限后进入重新锁定窗口，未锁定期间 AWGN 被放大。

skew 影响 deskew 成败和 `PhyChannel` 的最大传播延迟，但不会真的在各 lane symbol 数组中插入空 UI。

## 3. `ucie_fdi.h` 与 `ucie_link.h`

### 事务类型

`FdiFlit` 是 `UcieLink` 四个公开数据端口统一使用的事务级 FDI 对象：

```cpp
struct FdiFlit {
    std::vector<std::uint8_t> payload;
    std::size_t valid_bytes;
    std::uint64_t transaction_id;
    std::uint8_t vc;
    BusinessKind kind;
    sc_time transaction_start;
    sc_time fdi_time;
};
```

`Frame` 是 `ucie_detail` 命名空间内的 Adapter/PHY 内部对象，除事务元数据外还包含完整 Flit `bytes`、链路序号和 replay 标志。只有 `bytes` 经过 `BehavioralPhy` 损伤，RX 校验后重新输出 `FdiFlit`。

`FbMsg` 只携带链路 ACK/NAK 和序号。正向与反向分别拥有独立反馈通道；它不是业务响应。

### 数据源

`make_workload()` 提供两种工作负载：

- 未设置 `--payload-file`：为每个 transaction ID 生成确定性 Payload；
- 设置 `--payload-file`：读取 `HEX` 或 `transaction_id,vc,HEX` 记录。

`FdiSource` 只负责按配置间隔把 `FdiFlit` 写入有限 FIFO。其他 SystemC producer 也可以按照同一类型和 FIFO 语义替换它。

### 单方向可靠链路模块

| 模块 | 职责 |
| --- | --- |
| `TxAdapter` | 接收真实 FDI Payload、分配序号、构造 Flit、维护 Retry Buffer、处理 ACK/NAK 和 timeout replay |
| `PhyChannel` | 调用行为级 PHY并增加 TX、信道、最大 skew、RX 流水线延迟 |
| `RxAdapter` | CRC/序号检查、重复包丢弃、NAK 抑制、按序 FDI 交付 |
| `FeedbackPath` | 无误码、固定延迟、可流水化的 ACK/NAK 返回路径 |

`UcieLink` 将这组模块实例化两次：forward 传输请求，reverse 传输响应。两组分别维护序号、Retry Buffer、CDR 和误码统计，并公开 `soc_tx_in/soc_rx_out/mem_tx_in/mem_rx_out/link_state`。

## 4. `ucie_systemc_main.cpp`

### 测试端点

`MemoryResponder` 在正向 RX 后：

- 校验请求 ID、VC、顺序与 Payload；
- 加入 `memory_delay_ui` 流水线延迟；
- 生成 Payload 不变的 echo response；
- 把响应写入反向 FDI FIFO。

`FdiSink` 在反向 RX 后验证 response，统计 reverse hop 和完整 round-trip 延迟，可通过 `--response-file` 输出 `transaction_id,vc,payload_hex`。

### 结束和输出

全部响应返回 SoC 后调用 `sc_stop()`。`summarize()`、`print_summary()` 和 `write_csv()` 输出有效 Payload throughput、单方向容量、延迟、正反向可靠性、PHY 和完整性指标。未收齐响应、完整性错误或参数/文件错误会返回非零状态。

## 5. 修改入口

- 新增配置：修改 `Config`、`print_help()` 和 `parse_args()`；
- 修改 FDI 事务属性：修改 `ucie_fdi.h` 中的 `FdiFlit`；
- 修改链路封装或 Adapter：修改 `ucie_link.h`；
- 修改 Flit 布局：同步修改 `build_flit()` 和 `check_flit()`；
- 修改存储侧响应：修改或替换 `MemoryResponder`；
- 修改信道：修改 `BehavioralPhy`；
- 新增指标：扩展 `LinkStats/Stats` 和三个汇总输出函数。

编译、外部文件格式和运行示例见上一级 [`README.md`](../README.md)。
