> 本目录现为 StorageStacked 主仓库普通源码，AoU与观察接口直接在这里维护。
> 公共帧格式由 `../protocol/include` 提供，不再由其他内部模块打补丁。
> 整机运行见[统一环境](../env/README.md)；下文保留独立链路仿真的说明。

# UCIe 双向高速互联仿真模型（SystemC）

本模型对应项目研究成果 3，使用 SystemC 建模 UCIe D2D Adapter、事务级 FDI、双向业务链路和行为级 PHY。它支持 24 GT/s 及以上速率、Standard 256B/Compact 68B Flit、PAM4/NRZ、串并转换、CRC、ACK/NAK、重传，以及 AWGN、ISI、jitter、lane skew 和 CDR 抽象。

需求追踪、公式、验证方法和可信范围详见 [`MODELING_AND_VERIFICATION.md`](MODELING_AND_VERIFICATION.md)，源码分工见 [`src/README.md`](src/README.md)。

## 1. 双向架构

可复用的 `UcieLink` 顶层包含彼此独立的请求和响应数据方向。每个方向都有自己的 TX/RX Adapter、PHY、序号、Retry Buffer 和 ACK/NAK 反馈路径：

```text
请求方向（SoC -> Memory）

FdiSource -> soc_tx_in -> UcieLink FWD -> mem_rx_out -> MemoryResponder
                  ^                            |
                  +------ FWD ACK/NAK ----------+

响应方向（Memory -> SoC）

MemoryResponder -> mem_tx_in -> UcieLink REV -> soc_rx_out -> FdiSink
                       ^                            |
                       +------ REV ACK/NAK ----------+
```

`MemoryResponder` 是用于链路验证的行为级测试端点：检查正向请求，在 `--memory-delay` 指定的流水线延迟后生成 echo response。它不是完整的 Memory Controller；全链路集成时应由实际 Protocol Bridge/MC 模型替换。

## 2. 事务级 FDI

`UcieLink` 的四个数据端口统一使用包含真实 Payload 的 `FdiFlit`：

```cpp
struct FdiFlit {
    std::vector<std::uint8_t> payload;
    std::size_t valid_bytes;
    std::uint64_t transaction_id;
    std::uint8_t vc;
    BusinessKind kind;          // Request / Response
    sc_time transaction_start;
    sc_time fdi_time;
};
```

Payload 由 FDI producer 提供，TX Adapter 只负责添加链路序号、replay 标志和 CRC，RX Adapter 校验后重新输出 `FdiFlit`。`transaction_id`、VC、方向和时间戳是当前 SystemC 行为模型中的事务元数据；真正经过 PHY 损伤的是构造后的内部 Flit 字节数组。完整端口契约见 [`INTERFACE.md`](INTERFACE.md)。

有限深度 `sc_fifo<FdiFlit>` 和 Retry Buffer 提供背压。当 FDI FIFO 或 Retry Buffer 无法继续接收时，上游 producer 会停止注入。

## 3. 内部数据与外部 Payload 两种模式

### 3.1 内部生成模式

不指定 `--payload-file` 时，模型根据 transaction ID 生成可复现的伪随机 Payload：

```bash
./build/ucie_sc_sim --flits 2000
```

Payload 会依次经过正向链路、MemoryResponder 和反向链路，最终由 SoC 侧 Sink 完成 ID、VC、顺序和逐字节校验。

### 3.2 外部 Payload 文件模式

使用 `--payload-file` 输入外部业务数据。文件中的每个非空、非注释行表示一个事务，支持两种格式：

```text
HEX_PAYLOAD
TRANSACTION_ID,VC,HEX_PAYLOAD
```

示例：

```text
# 自动分配 transaction_id，VC=0
0011223344556677

# 显式 transaction_id 和 VC
42,3,deadbeefcafebabe
```

规则：

- 十六进制 Payload 必须包含偶数个数字；空格和下划线会被忽略；
- Payload 不得超过当前格式容量，Standard256 为 236B，Compact68 为 64B；
- 短 Payload 会在 Flit 中补零，但 `valid_bytes` 保留实际长度；
- 文件内 transaction ID 必须唯一；
- 外部文件中的有效记录数会覆盖 `--flits`。

运行及导出响应：

```bash
./build/ucie_sc_sim \
  --format compact68 \
  --payload-file tests/external_payloads.hex \
  --response-file results/external_responses.csv \
  --csv results/external_metrics.csv
```

响应文件格式为：

```text
transaction_id,vc,payload_hex
```

## 4. Flit 格式

| 格式 | 字段布局 | Payload 容量 | 满载效率 |
| --- | --- | ---: | ---: |
| Standard 256B | 2B Header + 236B Payload + 4B DLLP + 10B Reserved + 4B CRC | 236B | 92.19% |
| Compact 68B | 2B Header + 64B Payload + 2B CRC | 64B | 94.12% |

Header 第 0 字节保存序号低 8 位，第 1 字节 bit 0 为 replay 标志。Standard 256B 使用两组 CRC-16/CCITT，Compact 68B 使用一组 CRC-16。

## 5. 编译、运行与测试

依赖 g++/C++17 和 SystemC 2.3.4：

```bash
cd /home/hy258/ucie/ucie_systemc
make
make run
make unit-test
make test
make sweep
make validate
```

常用运行方式：

```bash
# 24 GT/s、16 lane、PAM4、内部 Payload
./build/ucie_sc_sim --rate-gtps 24 --lanes 16 --flits 3000

# 提高噪声，分别观察正向和反向 NAK/Replay
./build/ucie_sc_sim --sigma 0.20 --verbose --csv results/noisy.csv

# NRZ
./build/ucie_sc_sim --mod nrz --rate-gtps 32

# 改变存储侧响应延迟
./build/ucie_sc_sim --memory-delay 128
```

全部选项见：

```bash
./build/ucie_sc_sim --help
```

当前包含 22 项确定性单元断言和 82 项 SystemC 系统断言，覆盖 CRC/PAM4/NRZ/lane 黄金向量、内部与外部 Payload、双向 PHY、序号回绕、重传、CDR、deskew、watchdog、24/32/48 GT/s、两种 Flit、背压、精确延迟、P50/P95/P99、确定性和端到端完整性。完整矩阵见 [`TEST_PLAN.md`](TEST_PLAN.md)。

## 6. 主要输出指标

终端和 CSV 同时提供：

- 请求到达数、响应生成数和响应交付数；
- 完成的有效 Payload 字节和 payload throughput；
- 正向、反向和完整 round-trip 延迟；
- 正向/反向首次发送、Replay、ACK、NAK、CRC 错误；
- 聚合 SER、BER、Flit 失败率；
- 正向/反向端到端完整性错误；
- Retry Buffer 占用、背压、deskew、CDR 和 watchdog 指标。

对于短外部 Payload，带宽利用率按 `valid_bytes` 计算，因此会低于满载 Flit 的理论效率。

## 7. 模型边界

- 当前 FDI 是 SystemC 事务级 `sc_fifo` 接口，不是规范逐信号 `lp_data/pl_data/irdy/trdy` 接口。
- `UcieLink` 已封装为独立模块，SoC/Memory 两端输入输出统一为 `sc_fifo<FdiFlit>`；内部 `Frame` 不再暴露。
- `transaction_id`、VC 和时间戳目前是仿真元数据，若进行信号级联合验证，应由上层协议编码到线上 Payload，或由 transactor 显式映射。
- `MemoryResponder` 仅提供 echo response 和固定延迟，不解析 AXI，也不模拟 MC/DRAM 时序。
- ACK/NAK 是每个业务方向内部的链路可靠性反馈，不是存储业务响应。
- PHY 是行为级模型，不能替代 SPICE、IBIS-AMI、S 参数或 UCIe 合规性测试。

项目级全链路仍应连接为：

```text
GEM5/AXI -> AXI2Flit -> UCIe SystemC Link
         -> Protocol Bridge/MC -> DRAMsim3/Ramulator
```

读数据、写响应和原子操作结果通过 UCIe 反向业务方向返回 SoC。

## 8. 目录结构

```text
ucie_systemc/
├── Makefile
├── README.md
├── src/
│   ├── ucie_common.h
│   ├── ucie_phy.h
│   └── ucie_systemc_main.cpp
├── tests/
│   ├── external_payloads.hex
│   └── external_payloads.expected
├── scripts/
│   ├── run_tests.sh
│   └── run_sweep.sh
└── results/
```
