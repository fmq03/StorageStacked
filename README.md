# StorageStacked: VORTEX + Logic Die MoBA

VORTEX 执行 RISC-V 工作负载并发出真实访存请求，经 AXI256、AXI2Flit 和 UCIe 到达
堆叠存储的 **Logic Die**。Logic Die 在存储侧执行 BF16 块均值池化、驻留 Kmean、
FP32 门控打分与因果 Top-K 筛选，VORTEX 读取紧凑结果后继续执行。

| 目录 | 内容 |
|---|---|
| vortex | VORTEX SimX 外部存储适配与固定版本补丁 |
| systemc | 独立 SystemC 宿主、AXI 主机和存储侧 DMA 调度 |
| logic_die | 可综合门控 RTL、独立参考测试、DC 综合脚本 |
| axi2flit / ucie-model / protocol | AXI256 与双向 Flit 链路 |
| mem_sim | 在线堆叠存储控制器、DRAM 和 DFI 行为模型 |
| workloads | VORTEX 门控控制内核 |
| paper | IEEE 双栏论文、图片、测量数据及复现脚本 |

```bash
bash env/bootstrap.sh
bash env/build.sh
make acceptance
```

系统连接和寄存器见 [架构说明](docs/architecture.md)，验收证据和范围见
[验证说明](docs/verification.md)，参考论文的设计映射见 [来源说明](docs/reference-design.md)。

VORTEX、Berkeley HardFloat/SoftFloat 为复用的开源组件；门控控制、驻留管理、
数据流和系统适配在本项目实现。SRAM 数字行为与物理存算宏的边界在论文中单独说明。

## 论文与实测结果

[IEEE 双栏论文 PDF](paper/main.pdf) · [论文源码与复现](paper/README.md) ·
[原始测量摘要](paper/data/measurements.json) · [综合摘要](paper/data/synthesis.json)

- 24 个 RTL 用例、72 次正常查询、8 个存储测试、AXI/UCIe 回归和负向控制通过。
- 10 个主实验：32 块、128 维、Top-12、4 次查询，块大小为 1/2/4/8/16。
  门控模式的 VORTEX 请求跨度为 6 KiB，总时间为 124.116–484.548 μs。
- DC 映射面积约 0.321366 mm²；4 ns 约束下建立裕量为 +0.313 ps，
  保持裕量为 −51.082 ps，保持时间未闭合。

软件对照是 VORTEX 单活跃 lane 的标量实现。当前权重存储综合成寄存器，
尚未绑定物理 SRAM-CIM 宏；上述结果不代表商用 GPU 加速比、完整 LLM 或物理 signoff。
