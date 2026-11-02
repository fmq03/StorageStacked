# UCIe 独立链路测试计划

## 1. 验证入口

```bash
make unit-test   # 协议与 PHY 原语黄金向量
make test        # unit-test + 双向 SystemC 系统回归
make sweep       # 速率/lane/噪声/jitter/seed 参数扫描
make validate    # 全部执行并生成 results/LINK_VALIDATION_REPORT.md
```

任一单元或系统断言失败时 `make test` 返回非零。`make validate` 仅在全部回归成功后生成报告。

## 2. 单元验证

`src/ucie_unit_tests.cpp` 当前包含 22 项确定性断言：

- CRC-16/CCITT-FALSE 标准字符串黄金值 `0x29B1`；
- Standard256/Compact68 字段长度、Header、CRC 覆盖和固定 bit 翻转；
- 线上 8-bit 序号 `254,255,0,1` 回绕；
- PAM4 Gray 编码、判决门限及 NRZ 零门限；
- 固定 byte 向量的 lane striping/de-striping；
- CDR 连续坏 Flit 门限、计数清零和 512 UI 重锁；
- deskew 超限可观测性；
- Retry Buffer 和公开 FDI 队列参数边界；
- `FdiFlit` Payload、有效长度、ID、VC 和事务类型。

## 3. 系统回归

`scripts/run_tests.sh` 当前包含 82 项断言：

| 场景 | 重点检查 |
| --- | --- |
| T1 | 24 GT/s clean path、超过 256 Flit 序号回绕、80 UI 单跳、192 UI round-trip、P50/P95/P99、SER/BER=0、利用率≥90% |
| T2 | 16 lane PAM4、24 GT/s、原始容量 768 Gbps |
| T3 | 双向 CRC 错误、ACK/NAK、Go-Back-N replay、数据完整性 |
| T4 | AWGN 下 SER/BER、重传及完整交付 |
| T5 | 32 GT/s NRZ、容量与利用率 |
| T6 | Compact68、64 B Payload、Flit 效率≥94% |
| T7 | Retry Buffer 满、公开 FDI FIFO 背压、最大占用 |
| T8 | 相同 seed 可复现、不同 seed 结果变化 |
| T9 | 32 GT/s PAM4 完整交付及利用率 |
| T10 | 外部 Payload、transaction ID、VC、有效长度逐字节往返 |
| T11 | 强制 CDR 失锁/重锁、Degraded 状态、重传恢复 |
| T12 | deskew 超限、重传恢复、完整性 |
| T13 | 48 GT/s、1536 Gbps 原始容量、双向满负载、利用率≥90% |
| T14 | watchdog 到期、非完整返回码、`Failed` 状态及无伪完整性错误 |

## 4. 参数扫描

`scripts/run_sweep.sh` 生成 `results/sweep/summary.csv`，共 36 个配置点：

- PAM4：24/32/48 GT/s × 6 组 AWGN sigma；
- lane：8/16/32/64；
- jitter：0/0.01/0.03/0.05 UI × seed 7/11/29；
- NRZ：24/32 GT/s。

每个点保存吞吐率、容量、利用率、平均/P95/P99 延迟、ACK/NAK、重传、SER、BER、Flit
失败率、CDR 失锁和完整性错误。扫描用于观察趋势；标准验收断言由 `make test` 中的确定性场景承担。
