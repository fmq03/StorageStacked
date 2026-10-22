# 当前离线 Docker 交付验收

本轮镜像运行源码冻结在 `fea1810797a570ae10515916f806bf66d44c3ffe`，制作快照为 `fa821376`，后续运行源码改动见镜像内 `SOURCE_VERSIONS.json`。分支后续提交更新发布脚本、说明和验收记录。
镜像 `zhongxing-storagestacked:offline-20261022` 为 Linux amd64；实际制作方 Docker 为 `29.1.3`。
[发布下载](https://github.com/fmq03/StorageStacked/releases/tag/offline-20261022)；[下载与运行方法](README.md)。

## Benchmark 结果

| 平台 | Benchmark | 输出 | 设备周期 | 原生存储测试 |
|---|---|---|---:|---:|
| gem5 + Vortex SimX | SMOKE | 42 | 866 | 19/19 |
| gem5 + Vortex SimX | TinyLLM | blu | 228085 | 19/19 |
| CoralNPU RTL/Verilator | SMOKE | 42 | 308 | 19/19 |
| CoralNPU RTL/Verilator | TinyLLM | blu | 222187 | 19/19 |

Vortex 当前配置为每 warp 4 线程、每核最多 4 个 warp；模型维度为 8，工作组实际使用 8 个线程、2 个 warp，在一个 GPU 核上执行。配置容量与实际工作组用量分别核对，详见 `VALIDATION.json` 的 `vortex_simt`。

AXI 完整验收、在线 C ABI、配置检查、错误拒绝以及数值、AXI/VCD、Flit、在线内存检查通过。
设备周期是各模型在对应配置下的计数，不构成跨平台性能或硬件实现结论。

### ARM/GPU 缓存 SMOKE

AArch64 ELF、4 个实际活跃 CPU，GPU L1I/L1D 为 2 路、3 周期；L2 开启、8 路、6 周期。
输出 `42`，2,305 笔事务的存储链路检查通过，内存错误为 0。
原镜像的 ARM SE 设备预取遗漏导致 glibc `memcpy` 的 `PRFM` 卡住；修复后同一程序通过，实际记录 5 次设备预取拒绝。
验收脚本要求该计数大于 0。源码集成补丁保存在 gem5 的 `patches/arm_se_device_prefetch.patch`。

### CoralNPU 配置矩阵

矩阵属于 SMOKE/TinyLLM 的 RTL 配置变体，没有独立 TCM benchmark。六种 RTL 模型均从当前源码构建。

#### SMOKE

| ITCM / DTCM (KiB) | L0 | 输出 | NPU 周期 | 数值、波形与链路 |
|---|---|---|---:|---|
| 8 / 32 | 关 | 42 | 308 | PASS |
| 8 / 32 | 开 | 42 | 307 | PASS |
| 512 / 512 | 关 | 42 | 308 | PASS |
| 512 / 512 | 开 | 42 | 307 | PASS |
| 1024 / 1024 | 关 | 42 | 308 | PASS |
| 1024 / 1024 | 开 | 42 | 307 | PASS |

#### TinyLLM

| ITCM / DTCM (KiB) | L0 | 输出 | NPU 周期 | 数值、波形与链路 |
|---|---|---|---:|---|
| 8 / 32 | 关 | blu | 222187 | PASS |
| 8 / 32 | 开 | blu | 223375 | PASS |
| 512 / 512 | 关 | blu | 222187 | PASS |
| 512 / 512 | 开 | blu | 223375 | PASS |
| 1024 / 1024 | 关 | blu | 222187 | PASS |
| 1024 / 1024 | 开 | blu | 223375 | PASS |

错误加载的 TCM 几何与 L0 模型均被拒绝；AXI burst `1/2/4/8/16` 拍检查通过。
另通过只读挂载外部配置的 `fetch_l0_enabled=true` SMOKE，输出 `42`。

## 归档、分块与用户入口

- Docker 镜像：44,358,168,460 字节；镜像归档：16,509,120,312 字节。
- 连续 tar 分块：661 块，总计 16,509,255,680 字节；每块上限 25,000,000 字节。
- 已移除交付镜像标签，通过交付 `run.sh` 自动导入归档，并重新执行默认五项：AXI、Vortex SMOKE/TinyLLM、CoralNPU SMOKE/TinyLLM。
- 本次主机保留了其他镜像层，未宣称在空 Docker 数据目录中导入。计算容器使用 `--pull=never --network=none`，只挂载结果目录、只读兼容入口及显式配置文件，不挂载宿主项目源码或工具链。
- 真实分块已通过 `extract.sh` 完整还原，所有交付文件与已验收原归档逐字节一致。
- `DELIVERY.json`、`ACCEPTANCE.json`、`VALIDATION.json` 随解包后的交付目录提供；本目录的 `PACKAGE.json`、`PARTS.tsv`、`SPLIT_ACCEPTANCE.json` 保存发布包信息。

## Docker 兼容专项

5 个预期结果均符合：正常环境原入口成功；模拟旧 `clone3=EPERM` 时原入口失败；修复入口成功；正常环境修复入口成功；无关 `getpriority=EPERM` 拒绝仍然保留。
这是当前 Docker 上对旧限制的模拟，没有把它表述为客户 Docker 20.10 主机验收。详情见 `DOCKER_FIX_VALIDATION.json`。

## 范围

Vortex 结果来自 gem5 CPU + Vortex SimX；CoralNPU 使用 RTL 生成的 Verilator 模型。以上不包含 FPGA/板卡、综合布局布线、芯片时序或功耗验收；客户主机以实际运行生成的 `summary.json.passed=true` 为准。
