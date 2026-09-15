# 论文数据口径

`collect_results.py` 检查完整 10 个主实验、24 个 RTL 用例、4 个快速系统用例、
原生存储测试、独立在线/离线检查和负向控制后导出数据。它还比较运行源文件、
内核二进制与 DC RTL 哈希，缺失或失配即失败。

- `measurements.json`：C=32、D=128、Top-12、四次查询，B=1/2/4/8/16，
  Logic Die 和 VORTEX 标量软件各运行一次。时间不含 loader。
- `vortex_read_bytes + vortex_write_bytes`：内核期间末级请求的跨度总和；每条为 64 B。
  部分字节使能的写请求仍计完整跨度。包含指令、配置、完成、结果，
  不等于有效应用字节或包含帧头的物理链路字节。
- `local_dma_bytes`：实际本地读数据。准备读取 K 一次，每次 QUERY 只读 256 B Q。
- `prepare_cycles` 与每次 QUERY 的 `cycles`：RTL 命令内部周期，乘 4 ns 换算。
  总时间减去命令时间的差额包含处理器控制、链路和交接，不能解释为单一链路延迟。
- `system_checks.json`：实际字节、独立 CRC、AXI/VCD 握手与原生存储时序检查。
- `synthesis.json`：本机标准单元综合结果；权重存储映射为寄存器。
  `setup_met`、`hold_met` 分列，`physical_signoff=false`；不提供工作负载能效结论。
- `system_inputs.json` / `synthesis_inputs.json`：测量输入版本与哈希。
- `trace_hashes.json`：本地完整证据的哈希。VCD、CSV 和综合网表可由脚本重新生成，
  不随 Git 分支分发。只有摘要与哈希时，第三方仍需复跑以独立检查完整证据。
- `verification.json` / `rtl.json`：验收覆盖与结果，不宣称穷举正确性。
- `clean_build.json`：功能提交 `232e252` 的独立干净 checkout 构建、RTL 和在线冒烟验证。
  后续提交只调整验收记录和论文，不改变该项验证覆盖的运行实现。
- `tool_versions.json`：本机工具版本；绘图环境版本另见 `figures/provenance.json`。

本地完整证据目录为 `results/experiments-final`、`results/dc-final`、
`results/acceptance`。参考学位论文 SHA-256 为
`f7a130aa333d57d1ba99cdb72782b1cd324c3d3e5317b63538dce95be16b5490`。

重新生成：

```bash
make acceptance
python3 scripts/experiments.py --out results/experiments-final
python3 logic_die/syn/run.py --library /path/to/standard_cells.db --out results/dc-final
python3 paper/collect_results.py
make -C paper
```

已有实验目录可用 `--resume` 继续，前提是源代码与二进制哈希一致。新结果不会自动
覆盖论文数据；必须再次通过收集脚本的完整检查。更换时钟、库、输入、缓存或并发配置
后应使用新目录并重新运行，不能混合旧结果。
