# CoralNPU 内部维护

用户入口为根目录 build.sh 和 user/run.sh；用户配置仅在 user/<项目>/config.json。
coralnpu/ 是设备、工具和验证实现；integration/ 负责仿真入口、设备参数和 AXI 存储接入。
依赖同级 axi_StorageStacked 的公共源码，不加载其他处理器驱动源码或二进制。
全局只有一套独立 SystemC、1 fs 时基；保留真实异步响应、反压和同 ID 顺序。
不得用退出码或文件存在代替计算、AXI/VCD、Flit 与在线内存验收。
根目录 ./build.sh --test 执行完整回归；用户可执行 ./user/run.sh smoke --test 或 llm --test。
业务配置与结果中的项目路径使用相对路径；工具内部缓存可重建。
不生成任何摘要算法清单。不 push，除非用户明确要求。
