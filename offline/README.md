# 三项目离线 Docker 交付

交付包包含完整 Linux amd64 镜像、三项目源码、编译工具、设备仿真器、Bazel 依赖、TinyLLM 模型和中文文档。宿主机只需已安装并启动 Docker，用户无需安装 Python、CMake、SystemC、Bazel、交叉编译器或配置环境变量。

## 用户使用

GitHub 下载入口：[offline-20261009-burst](https://github.com/hy2581/zhongxing-storagestacked-offline/releases/tag/offline-20261009-burst)。412 个分块，每块 25,000,000 字节，最后一块较小。克隆 [发布仓库](https://github.com/hy2581/zhongxing-storagestacked-offline)后执行 `bash download.sh`，自动下载并解包；已有全部分块、`PARTS.tsv`、`extract.sh` 与四个启动补丁文件（见兼容说明）时执行 `bash extract.sh`。核对使用文件名、大小和实际导入结果。 下载默认单连接，每 5 秒显示分块进度；连续 60 秒低于 1 字节/秒或单次请求超过 900 秒会重试，每个文件最多尝试 6 次。按 Ctrl+C 可停止下载并保留续传文件；更新脚本后重新运行即可继续。 下载已用实际 Bash 4.2 + curl 7.29 和 Bash 5.2 + curl 8.5 测试；旧版 curl 不支持的 `--http1.1` 参数会自动省略。制作方最新复核见 [CLIENT_VALIDATION.json](https://github.com/hy2581/zhongxing-storagestacked-offline/blob/main/CLIENT_VALIDATION.json)，其中区分制作方验证与客户机器实际验收。

把交付目录复制到本机，进入目录执行：

```bash
bash run.sh
```

首次使用会自动从同目录的 `zhongxing-storagestacked-offline.tar.gz` 导入镜像，然后执行 AXI 完整存储验收，以及 Vortex、CoralNPU 各自的 SMOKE 和 TinyLLM。后续运行直接使用本地镜像。每次生成新的 `results/<时间>-<编号>/`；总报告 `summary.json` 的 `passed` 必须为 `true`。

默认验收保留完整波形并生成视图，通常需要数十分钟；运行时间随机器变化。只检查环境可执行 `bash run.sh check`，单独运行某个设备可使用下面的命令。

正式 `run.sh` 已修复旧 Docker 下的 `posix_spawn failed: Operation not permitted`：
保留 Docker seccomp，追加容器内兼容过滤器，使编译器回退到可用的进程创建方式。
下载和解包会自动安装最新版 `run.sh` 与 `docker-compat-entrypoint.py`；
已解包的用户只需更新启动补丁，无需重下镜像。
操作与验证范围见 [Docker 兼容说明](DOCKER_COMPATIBILITY.md)。
`run-seccomp-check.sh` 仅保留为显式关闭 seccomp 的诊断入口。

启动固定使用 `--pull=never --network=none`。不执行联网安装，不拉取镜像，不使用宿主机项目源码、工具链或许可证。镜像内部保留构建时的固定路径，用户把交付目录放在任意路径即可。

```bash
bash run.sh check              # 工具、动态库与仿真器检查，不代替计算验收
bash run.sh smoke              # 两个设备的 SMOKE：41 → 42
bash run.sh llm                # 两个设备的 TinyLLM："red " → "blu"
bash run.sh axi               # 默认 AXI 文件读写与链路检查
bash run.sh vortex smoke       # 单独运行 Vortex SMOKE
bash run.sh coralnpu llm       # 单独运行 CoralNPU TinyLLM
bash run.sh test               # AXI + 两个设备的完整回归，耗时和结果较大
bash run.sh shell              # 进入镜像，源码位于 $STORAGE_WORKSPACE
```

自选配置无需配置环境变量，传入 JSON 即可：

```bash
bash run.sh vortex smoke --config ./my-config.json
bash run.sh coralnpu llm --config ./my-llm.json
bash run.sh axi --input ./transactions.json --scale 4 --replay
```

配置格式见镜像内两项目的 `user/<项目>/config.json` 和 `docs/`。配置由入口复制到项目内并随结果保存。修改硬件参数可能触发离线增量编译；更换为镜像未包含的新依赖需要另行制作镜像。

默认输出在启动脚本旁边的 `results/`，可选 `STORAGE_RESULTS_DIR` 改变位置，`STORAGE_IMAGE` 选择镜像版本。不同运行互不覆盖。容器退出后会删除，导出的报告、输入快照、实际回读、波形和链路证据保留在宿主机。交互 shell 中的修改随容器删除；需要保存的文件请写入 `/results`。

## 手动导入与运行

推荐使用 `run.sh` 自动处理旧 Docker 兼容。以下手动命令使用镜像原始入口，适用于已更新运行时的环境：

```bash
docker load --input zhongxing-storagestacked-offline.tar.gz
mkdir -p results
docker run --rm --init --pull=never --network=none \
  --mount "type=bind,source=$(pwd)/results,target=/results" \
  zhongxing-storagestacked:offline-20261009-burst
```

适用于 Linux x86-64；其他平台需要能执行 Linux amd64 容器的 Docker 环境。

压缩包、镜像的精确大小见交付包中的 `DELIVERY.json`；默认五项验收会生成数 GiB 的结果，导入镜像还需要临时空间。 分块下载、解包和 Docker 导入的峰值占用需要分别计入；若下载、结果和 Docker 数据目录在同一磁盘，建议预留至少 100 GB 可用空间。镜像和波形结果较大，请以交付清单的字节数预留存储空间。SMOKE 和 TinyLLM 不需要 GPU、FPGA 或商业 EDA 许可证。TinyLLM 为项目自带的小型 FP32 功能模型。

## 制作与复验

Docker 制作入口在 `offline/`。修改源码后，制作方执行：

```bash
./offline/build.sh
./offline/validate.sh
./offline/export.sh
```

`build.sh` 从当前已准备好的工作区复制源码、工具和缓存，以本地 `ubuntu:20.04` 镜像构建；Docker 构建阶段也关闭网络。镜像内重新编译 AXI、Vortex、CoralNPU 平台及用户项目，构建缓存只用于增量编译。它不承担首次下载依赖的职责。镜像内部使用准备好的固定目录，避免工具链和构建缓存因改路径而失效。历史结果、Git 元数据、旧 Bazel 工作区、临时 sandbox 和运行状态不进入交付镜像；必要依赖缓存完整保留。

本包包含打包时工作区中的源码与未提交修改：CoralNPU 使用 RVV 核心与 RVV FP32 线性层；Vortex 的默认 TinyLLM 线性层使用 8 个 SIMT 线程、2 个 warp。两个设备的回归各执行一次，默认 SMOKE 和 TinyLLM 各运行一次，同时保留原生、C ABI 和错误输入检查。
CoralNPU 的 AXI256 接入支持可配置的 INCR burst：`axi.burst_beats` 可取 1、2、4、8、16，默认 4。
每个原生 16 字节请求默认转换为 4 拍、每拍 4 字节；AWLEN/ARLEN 为 3，末拍才置 WLAST/RLAST。
完整回归还执行五种拍数的字节掩码、总线 lane、4 KiB 边界附近访问、反压和重放检查。
这项改动提供窄拍 burst 转换，多个原生请求的合并与宽拍带宽优化尚未实现。

导出目录为 `offline/dist/zhongxing-storagestacked-offline-20261009-burst/`。镜像和归档是源码快照，继续修改源码后需要重新制作；已经运行的容器不会自动更新。若需保留同标签的上一版，先为旧镜像添加备份标签并移动已有交付目录，再执行上述命令。

`SOURCE_VERSIONS.json` 记录打包时三个仓库的源码提交与未提交修改，`DELIVERY.json` 记录文件名、精确字节数和验收证据。`ACCEPTANCE.json` 是最终归档导入后的默认验收摘要，`VALIDATION.json` 内嵌三套回归摘要并记录本次断网运行证据。交付核对以版本、文件名/大小、实际 `docker load` 和新生成的运行报告为准。
