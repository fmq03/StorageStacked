# 参考论文与本项目的设计映射

参考文件：王瑞泰，《面向 MoBA 算法门控运算的存算一体异构芯粒系统设计》，
电子科技大学专业学位硕士论文，2026。用户提供本地 PDF，共 115 页。
以下页码是正文页码；物理 PDF 页码通常为正文页码加 14。

| 参考位置 | 设计思想 | 本项目落点 |
|---|---|---|
| 第 3.1 节，27–30 页 | 门控任务卸载；特征提取、打分、筛选闭环 | 移入堆叠存储的 Logic Die，直接访问本地 DRAM |
| 第 3.2 节，30–36 页 | K 阶段池化驻留，Q 阶段重复查询；GQA 共享 | PREPARE / QUERY 描述符、epoch 匹配、单 KV 头驻留复用 |
| 第 3.3.1 节，37–39 页 | SRAM 保存 Kmean，BF16 符号/指数/尾数计算 | 四个数字权重 bank 和 BF16→FP32 MAC 数据流 |
| 第 3.3.2 节，39–41 页 | 浮点保序映射、基数式 Top-K | 逐位竞争网络；确定性的同分规则与因果块掩码 |
| 第 4.3 节，59–67 页 | 共享存储、主处理器/加速器解耦 | 主机与本地 DMA 的有界调度；独立完成响应窗口 |
| 第 5 章，78–88 页 | 分层验证与系统评估 | RTL 独立对拍、在线系统闭环、DC 综合、原始数据出图 |

## 明确差异

原论文的 SRAM 计算 Cell 使用文献 [95] 的宏；3.4.3 节明确把宏作为黑盒，其指标来自
引用而非标准单元综合。本项目只有论文，没有该宏的电路、版图或表征数据，因此实现
可综合的数字行为和权重存储接口，以实测标准单元结果报告开销。不能宣称复刻该物理宏。

原论文基准是 32 个 chunk、每头 128 维、chunk_size=4096、Top-K=12。
本项目保持块数、维度和 K 的能力，运行缩放的确定性输入来验证集成；没有运行
Llama 3.1-8B 端到端推理，不继承原论文的 0.262 ms、105.3 TFLOPS/W 或 1.24× 结论。

原论文文字同时描述“逐 bit 扫描”和“每周期选出一个胜者”。本项目把时序定义为
一个 bit 一个时钟，32 个时钟选一个胜者，并报告实际周期数。这个实现与原文的
12 周期 Top-12 不是同一个时序假设。

论文中的 Group / Cluster / Cell 三层大规模网络用于独立加速器芯粒。
本项目的主要工作点是 Logic Die：四 bank 的本地计算与共享 DRAM 仲裁，未实现该
三层网络的物理布局，也未把其 BookSim 或能效数值作为本项目证据。

## 外部算法与算术来源

- MoBA 原始论文：[Lu et al., arXiv:2502.13189](https://arxiv.org/abs/2502.13189)。
- BF16 扩展到 FP32、标准 FP32 加/乘复用
  [Berkeley HardFloat](https://www.jhauser.us/arithmetic/HardFloat-1/doc/HardFloat-Verilog.html)。
- VORTEX 实现来源：[vortexgpgpu/vortex](https://github.com/vortexgpgpu/vortex)，版本由锁文件固定。
- 格式采用 [IEEE 官方会议论文模板说明](https://conferences.ieeeauthorcenter.ieee.org/write-your-paper/authoring-tools-and-templates/)
  对应的 IEEEtran conference 文档类；中文字体由 CTeX 支持。
