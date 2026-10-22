# axi_StorageStacked

先读 README.md、INTERFACE.md。本项目只负责 AXI 信号接收到在线访存及原路响应。
公共实现不得依赖 gem5、Vortex、CoralNPU、TLM 父请求或处理器 ELF。
独立程序使用本仓库的一套 SystemC；gem5 使用自身的一套 SystemC 编译相同公共源码。
全局时间分辨率 1 fs。不得用测试 RAM、伪造 trace、退出码或文件存在代替数据与链路验收。
统一入口 ./run.sh build / run / test。新运行使用新结果目录。
禁止 submodule；引用本项目的驱动用 STORAGE_STACK_ROOT 声明独立依赖。
交付核对使用版本、提交号、文件名/大小和实际运行验收，不生成摘要清单。
