# axi_StorageStacked

先读 README.md、INTERFACE.md。本项目只负责 AXI 信号接收到在线访存及原路响应。
公共实现不得依赖 gem5、Vortex、CoralNPU、TLM 父请求或处理器 ELF。
SystemC 按宿主选择：独立程序默认构建本项目 third_party/systemc，也可用 STORAGE_SYSTEMC_PREFIX 指向匹配的安装；gem5 使用自身的内核编译相同公共源码；每个进程只用一套内核。
全局时间分辨率 1 fs。不得用测试 RAM、伪造 trace、退出码或文件存在代替数据与链路验收。
统一入口 ./run.sh setup / build / run / test / smoke；跨组件链路测试用 ./run.sh link-test，组件自测选 SystemC 用 ./run.sh systemc-prefix。新运行使用新结果目录。
源码、include 与检查器路径只在 storage_manifest.json 维护一份，平台通过它读取，不再硬编码本项目内部子目录。
禁止 submodule；引用本项目的驱动用 STORAGE_STACK_ROOT 声明独立依赖。
交付核对使用版本、提交号、文件名/大小和实际运行验收，不生成摘要清单。
