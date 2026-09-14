# StorageStacked

CPU / Vortex GPU / CoralNPU → 原生AXI256 → AXI2Flit → UCIe → 在线mem_sim的统一系统。

| 普通源码目录 | 职责 |
|---|---|
| gem5_new | 三源设备、观察器、工作负载及外部依赖适配 |
| gem5_axi | gem5原生TLM、AXI256 Master、在线内存桥及验证 |
| axi2flit | AXI与Flit转换 |
| ucie-model | UCIe链路、重放与观察接口 |
| mem_sim | 内存控制器与DRAM行为模型 |
| protocol/include | 两端共享的AoU帧格式 |

外部子模块仍为gem5、coralnpu、vortex-gpu/vortex（含Vortex递归依赖）。
内部五个目录已通过保留完整历史的导入合并成为主仓库源码，不再各自维护Git仓库。

```bash
git clone --recurse-submodules https://github.com/fmq03/StorageStacked.git
cd StorageStacked
bash env/bootstrap_xpu.sh
bash env/build_xpu.sh
bash env/run_memsim.sh results/acceptance-memsim
bash env/run_xpu.sh results/acceptance-xpu
```

新机器先按[主机条件](docs/setup.md#1-主机条件)安装基础工具。bootstrap_xpu/build_xpu
包含CPU基础环境与构建；GCC、Python等版本由锁文件指定，无需手动选择编译器。
仅验证CPU时可以使用env/bootstrap.sh、env/build.sh、env/run_memsim.sh。

首次安装、依赖包恢复和无依赖包配置见[配置与交接指引](docs/setup.md)。
环境版本与操作见[env/README.md](env/README.md)，分支协作、历史追溯、源码归属见
[开发说明](docs/development.md)。外部版本在env/sources.lock.json，内部导入来源在
[env/internal_imports.json](env/internal_imports.json)。

交接验收见[验收记录](docs/handoff-validation.md)：
完整构建、19项原生测试、7组在线内存、4组GPU/NPU、旧链路/RAM兼容，以及新目录恢复构建和闭环验证通过。
旧AXI256与monorepo报告继续保留；HTML交接请复制整个用例目录。
结果/构建产物和integrate_doc本地交接资料不入库。原/mnt/d/storagestacked保留。
运行结果不随Git克隆分发，需要在本机运行生成。HTML查看方式见配置指引；推荐通过本机HTTP服务打开。
主仓库：https://github.com/fmq03/StorageStacked 。新提交说明使用中文。
