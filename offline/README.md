# 三项目离线 Docker 交付

沿用原 `hy2581/zhongxing-storagestacked-offline` 的交付方式，将镜像、源码与制作入口更新到
`fmq03/StorageStacked` 的 `project-update-20261022` 分支。新版本为 `offline-20261022`；
旧 `offline-20261009-burst` 是历史快照，不会随源码更新。

## 下载与运行

宿主机需要 Linux x86-64、Git、Bash、curl、GNU tar/coreutils，以及当前用户可访问的 Docker。
无需安装 Python、SystemC、Bazel、交叉编译器、GPU 驱动或商业 EDA 软件。

```bash
git clone --branch project-update-20261022 https://github.com/fmq03/StorageStacked.git
cd StorageStacked
bash download.sh
bash run.sh
```

下载入口为 [offline-20261022 Release](https://github.com/fmq03/StorageStacked/releases/tag/offline-20261022)。
`download.sh` 下载分块和启动文件，按 `PARTS.tsv` 中的文件名、大小和顺序核对并解包到
`downloads/zhongxing-storagestacked-offline-20261022/`。默认单连接，支持续传，每 5 秒显示进度；
停滞 60 秒或单次请求超过 900 秒会重试。每个文件最多尝试 6 次，Ctrl+C 后保留已下载文件。
`STORAGE_DOWNLOAD_JOBS=2 bash download.sh` 可增加并发。代理必须允许 GitHub Release 的下载跳转。

`run.sh` 在新镜像标签不存在时自动导入归档，随后执行 AXI 完整验收、Vortex SMOKE/TinyLLM、
CoralNPU SMOKE/TinyLLM 五项。SMOKE 输出 `42`，TinyLLM 输出 `blu`；完整通过还要求数值、
AXI/VCD、Flit 和在线内存检查全部通过。结果保存为 `results/<时间>-<编号>/summary.json`，
其中 `passed` 必须为 `true`。单纯镜像导入或 `check` 成功不能代替计算验收。

下载目录可整体复制到内网主机。在解包后的交付目录内执行 `bash run.sh`，不依赖源码仓库路径。
已有全部分块、清单、`extract.sh` 和四个启动文件时，也可直接执行 `bash extract.sh`。

```bash
bash run.sh check
bash run.sh smoke
bash run.sh llm
bash run.sh axi
bash run.sh vortex smoke
bash run.sh coralnpu llm
bash run.sh test
bash run.sh shell
```

`test` 执行 AXI 完整验收、两个计算平台的原生/配置/负向检查、SMOKE/TinyLLM、NPU 五种 AXI
burst 拍数，以及三种 TCM 预设与 L0 开关组合的 12 个 SMOKE/TinyLLM 场景。没有独立 TCM benchmark。
默认验收保留完整波形并生成视图，耗时与结果大小随主机变化。

每次容器使用 `--pull=never --network=none`，不拉取镜像、不联网安装，不挂载宿主机项目源码或工具链。
默认保留 Docker seccomp，并追加 `clone3 → ENOSYS` 兼容过滤器，处理旧 Docker 的编译进程创建问题。
详见 [Docker 兼容说明](DOCKER_COMPATIBILITY.md)。`run-seccomp-check.sh` 仅用于显式诊断。

## 用户配置

```bash
bash run.sh coralnpu smoke --config ./my-config.json
bash run.sh vortex llm --config ./my-llm.json
bash run.sh axi --input ./transactions.json --scale 4 --replay
```

配置文件只读挂载到容器，随后复制为运行输入快照。格式见各项目 `user/<项目>/config.json`。
NPU `fetch_l0_enabled` 可选 `true` 或 `false`，默认 `false`；ITCM/DTCM (KiB) 组合为
`8/32`、`512/512`、`1024/1024`。镜像包含六种 RTL 模型、对应的 Chisel/Verilator 构建依赖和设备库，
按配置选择或增量重建。AXI `burst_beats` 支持 `1/2/4/8/16`，默认 `4`。
CPU 的 ARM/x86 配置与 GPU 各级缓存参数沿用当前项目实现，具体约束见对应 `docs/`。

NPU 每个场景顶层保留 `report.md`、`summary.json`，内容按 `config/`、`logs/`、`build/`、
`traces/`、`checks/`、`views/` 分组；Vortex 保留自身结果格式。配置、执行报告和实际模型信息随结果导出。
可用 `STORAGE_RESULTS_DIR` 更改宿主机结果位置，`STORAGE_IMAGE` 指定镜像标签。
交互 shell 的修改随容器删除，需要保留的内容写到 `/results`。

## 制作与复验

制作方需要已准备好工具链和依赖缓存的当前工作区。首次依赖获取由三个项目自身构建入口完成，
Docker 制作脚本不承担下载依赖的职责。使用本地 `ubuntu:20.04` 或 `STORAGE_BASE_IMAGE` 指定的本地镜像。

```bash
bash offline/build.sh
bash offline/validate.sh
bash offline/export.sh
bash offline/split-release.sh
python3 offline/verify-split.py offline/dist/zhongxing-storagestacked-offline-20261022-25MB --output offline/results/split-restored-20261022
```

构建阶段同样关闭网络；从当前工作区复制源码、工具和可复用缓存，在镜像内重新编译 AXI、
Vortex、CoralNPU 和六种 RTL 配置。历史运行结果、Git 元数据和临时 Bazel server/sandbox 不打包。
Vortex 工具环境限制在子进程内，NPU 构建使用与容器运行时一致的独立环境；构建容器退出后，`finalize-image.sh` 在新容器中清除临时服务、worker 和 sandbox 状态。
`SOURCE_VERSIONS.json` 记录工作区提交、未提交改动与各子项目版本，不能用旧结果证明新镜像通过。

`validate.sh` 用新镜像执行 `check` 和一次 `test`，并行验证 ARM 主机与自定义 GPU 缓存的 SMOKE，检查 AArch64 ELF、实际参与 CPU、输出及实际发生的不可缓存设备预取拒绝；`export.sh` 保存归档，移除交付标签后通过用户
`run.sh` 自动重新导入，再执行默认五项。只有这些检查全部通过才生成 `DELIVERY.json` 和 `VALIDATION.json`。
新导出目录为 `offline/dist/zhongxing-storagestacked-offline-20261022/`。已存在的交付包拒绝覆盖。
`split-release.sh` 将整个目录打成连续 tar 分块，每块 25,000,000 字节；最后一块较小。

核对使用源码提交号、文件名与精确字节数、实际解包、Docker 导入和新生成的验收结果。
`PACKAGE.json` 记录分块和文件大小，`ACCEPTANCE.json` 记录首次导入后的默认验收，
`VALIDATION.json` 保留制作方完整回归与 RTL 配置检查；客户主机仍以自己的实际运行结果为准。
镜像与分块不放入 Git，通过本仓库 Release 分发。建议为下载、解包、Docker 导入和波形结果预留至少 150 GB 空间，
各文件精确大小以本次 `PACKAGE.json`、`DELIVERY.json` 为准。

发布方可用 `upload-release.py` 上传已验收的分块目录。脚本按远端文件名、大小与状态恢复上传，
并保存上传请求的时间记录；超过每小时额度时分时继续，恢复时使用相同的 `--report` 路径。
上传完成还需核对附件集合，最后公开 Release。
