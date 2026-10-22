# AXI 存储、Vortex 与 CoralNPU

本工作区包含三个独立源码项目，共用同级 AXI 存储源码。

本仓库以一个源码树保存当前工作区的三个项目及公共文档，不使用子模块。
工具链、构建缓存、离线镜像和运行结果不入库；文档中的本地结果链接在执行对应验收后生成。

| 项目 | 职责 | 文档 |
|---|---|---|
| axi_StorageStacked | AXI256 → AXI2Flit → UCIe → 在线内存 | [使用说明](axi_StorageStacked/README.md) · [接口](axi_StorageStacked/INTERFACE.md) |
| vortex_StorageStacked | gem5 主机 + Vortex SimX，TLM → AXI | [使用说明](vortex_StorageStacked/README.md) · [用户文档](vortex_StorageStacked/docs/README.md) |
| coralnpu_StorageStacked | CoralNPU RVV RTL，原生异步接口 → AXI | [使用说明](coralnpu_StorageStacked/README.md) · [用户文档](coralnpu_StorageStacked/docs/README.md) |

两个计算项目独立编译公共存储源码，各自拥有内存状态。gem5 使用自身的 SystemC，CoralNPU 和 AXI 独立程序使用独立 SystemC；同一进程只加载一套内核。

## Docker 使用

Docker 镜像包含三个项目的源码、工具链、设备模型和 TinyLLM 数据。沿用原离线交付仓库的
分块下载、自动导入及断网运行方式。宿主机只需 Linux x86-64、Git、Bash、curl、tar 和 Docker。

```bash
git clone --branch project-update-20261022 https://github.com/fmq03/StorageStacked.git
cd StorageStacked
bash download.sh
bash run.sh
```

`download.sh` 从本仓库的 `offline-20261022` Release 下载 25,000,000 字节分块并解包，
支持续传。`run.sh` 自动导入新镜像，然后运行 AXI 完整验收与 Vortex、CoralNPU 各自的
SMOKE/TinyLLM；启动固定使用 `--pull=never --network=none`。
结果保存到根目录 `results/<时间>-<编号>/`，以新生成的 `summary.json` 中 `passed: true` 为准。
离线包是独立镜像快照，源码修改后需要重新制作镜像。

```bash
bash run.sh check           # 工具和动态库检查
bash run.sh smoke           # 两个设备的 SMOKE
bash run.sh llm             # 两个设备的 TinyLLM
bash run.sh test            # 三项目回归及 NPU 12 个 RTL 配置场景
bash run.sh coralnpu smoke --config ./my-config.json
bash run.sh shell
```

详细命令、NPU L0/TCM 配置、制作与验收方法见 [Docker 离线使用](offline/README.md)。
下载脚本及兼容机制沿用原 Docker 仓库，镜像和源码更新为本仓库当前版本。

## 原生构建与运行

在本目录执行：

```bash
./axi_StorageStacked/run.sh setup
./axi_StorageStacked/run.sh build
./vortex_StorageStacked/build.sh --storage ../axi_StorageStacked --jobs 12
./coralnpu_StorageStacked/build.sh --storage ../axi_StorageStacked --jobs 12
./accept.sh
```

`accept.sh` 运行 AXI 完整验收，并构建、验收两个计算平台。每次生成新的结果目录；以各项目 `summary.json` 的 `passed: true` 为准。验收包含计算结果、AXI/VCD、Flit、在线内存和错误拒绝检查，详细范围见各项目文档。

单独运行示例：

```bash
./axi_StorageStacked/run.sh run --input axi_StorageStacked/examples/roundtrip.json
./vortex_StorageStacked/user/run.sh smoke
./vortex_StorageStacked/user/run.sh llm
./coralnpu_StorageStacked/user/run.sh smoke
./coralnpu_StorageStacked/user/run.sh llm
```

默认 SMOKE 为 `41 → 42`，TinyLLM 为 `"red " → "blu"`。用户配置在两个计算项目各自的 `user/<项目>/config.json`；运行结果在 `user/<项目>/result/`，平台回归汇总位置由入口打印。AXI 独立结果在 `axi_StorageStacked/results/`。

## VS Code

打开 [zhongxing.code-workspace](zhongxing.code-workspace)，或直接打开当前 `zhongxing` 目录。跳转、索引刷新及验证方法见 [.vscode/README.md](.vscode/README.md)。

## 其他资料

- 逐文件中文讲解：[Vortex 集成](markdown/vortex_StorageStacked/integration/README.md)、[CoralNPU 集成](markdown/coralnpu_StorageStacked/integration/README.md)。
- [Docker 离线使用](offline/README.md)与[兼容说明](offline/DOCKER_COMPATIBILITY.md)。新包使用独立的 `offline-20261022` 版本，旧包保留原有标签。
- 验证方法和执行结果集中保存在 [CoralNPU 验证说明](coralnpu_StorageStacked/docs/08-验证说明.md)、[Vortex 验证说明](vortex_StorageStacked/docs/07-验证说明.md) 和 [AXI 输出与验证说明](axi_StorageStacked/docs/OUTPUTS.md)。

旧运行目录、重复离线包、Docker 临时构建上下文和历史发布记录已移出工作区，归档位于同级 `zhongxing-cleanup-archive-20261020-154702/`；其中 `manifest.json` 列出原相对路径，可按需恢复。编译工具链、正在使用的构建缓存和未提交源码修改保留。

2026-10-22 又将三个项目的历史验证结果目录移出项目树，内容整理为上述文档。
原始记录备份位于同级 `zhongxing-validation-archive-20261022-101904/`，其中 manifest.json 记录原路径与文件大小。
