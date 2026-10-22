# 独立 AXI 存储验收

在本项目目录执行，每次使用新结果目录：

```bash
./run.sh setup
./run.sh build
./run.sh test
```

入口自动生成 `results/acceptance-<时间>/`；也可用 `--output results/自定义新目录`。总摘要 `summary.json` 与所有场景均须为 `passed: true`。

完整验收检查：

- 19 项原生内存测试，以及在线 C ABI 的字节掩码、队列与大地址窗口。
- SMOKE、default、slow、backpressure、replay、decode_error、read_only、write_only 八个场景。
- 沿真实 AXI → AXI2Flit → 双向 UCIe → 在线内存链路逐字节回读，核对 VCD/CSV、Flit、内存子请求、DRAM/DFI 和最终镜像。
- B/R 反压、重放、窄拍与部分写入、ID 重用、4 KiB 页末访问，以及合法窗口外请求的 DECERR。
- 内存周期放大四倍后，相同首笔写操作的往返延迟增加。后续请求可能落入不同刷新窗口，不要求整个序列耗时单调。
- 拒绝六类非法输入，并拒绝被篡改的读返回；退出码和文件存在不能代替校验结果。

单独执行 `./run.sh smoke` 时，`summary.json.passed`、`summary.json.smoke_passed` 和 `smoke_summary.json.passed` 必须同时为 true。输入与输出格式见 [INPUTS.md](docs/INPUTS.md) 和 [OUTPUTS.md](docs/OUTPUTS.md)。

新增测试可参考 [SMOKE 教程](docs/03-从零添加SMOKE测试.md) 和 `examples/edge_cases.json`。边界示例不自动计入上述八场景验收。

模型范围与 AXI 子集见 [README.md](README.md) 和 [INTERFACE.md](INTERFACE.md)。PHY/DFI 为行为模型；验收覆盖当前仿真功能和时序反馈。

历史验证的关键数值与方法集中保存在 [输出与验证说明](docs/OUTPUTS.md)，历史结果目录已移出项目树。当前结论以本次新生成的结果为准。
