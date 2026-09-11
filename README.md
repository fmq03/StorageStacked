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
git submodule update --init --recursive
bash env/bootstrap.sh
bash env/build.sh
bash env/run_memsim.sh
# GPU/NPU工作流
bash env/bootstrap_xpu.sh
bash env/build_xpu.sh
bash env/run_xpu.sh
```

环境版本与操作见[env/README.md](env/README.md)，分支协作、历史追溯、源码归属见
[开发说明](docs/development.md)。外部版本在env/sources.lock.json，内部导入来源在
[env/internal_imports.json](env/internal_imports.json)。

已有AXI256验收结果位于results/axi256-20260911；HTML按需加载，复制整个用例目录查看。
结果/构建产物和integrate_doc本地交接资料不入库。原/mnt/d/storagestacked保留。
新提交说明使用中文；本轮只提交到本地，没有配置主仓库远端或执行push。
