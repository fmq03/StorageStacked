# 系统架构

公共链路的职责是接收 AXI256 事务，经 AXI2Flit 编解码和 UCIe 双向链路到达响应端，再由 MemSimBackend 转换为在线 mem_sim 请求；数据、状态与响应 ID 沿反向链路返回。

独立 AXI 驱动、Vortex/gem5 的 TLM/AXI 适配器、CoralNPU 原生请求/AXI 适配器使用相同的存储模型源码。处理器 ELF、设备请求和主机实现属于各平台，公共存储实现不依赖它们。

gem5 使用自身事件队列和内置 SystemC 调度公共链路；独立 AXI 与 CoralNPU 由所选的独立 SystemC 调度。多份安装允许共存，每个运行进程只使用一套内核，时间基准为 1 fs。

组件内部测试保留在组件中；同时依赖两个组件的胶水和测试归链路集成层。全链路 BFM + SimpleBurstMemory 测试与在线 mem_sim 集成测试分别报告，不能互相替代。
