> 本目录独立维护 AXI2Flit 桥；公共帧格式在 `../../common/interfaces`。
> UCIe 接线、响应端及全链路测试由 [AXI 集成层](../../axi_StorageStacked/README.md) 维护。
> 下文的 SimpleBurstMemory 用于桥自身的独立测试。

# AXI2FLIT

AXI2FLIT 是使用 C++17 和 SystemC 实现的双向协议桥及联合仿真环境。桥将 SoC 的 AXI 请求编码为固定字节格式的消息，经 UCIe 行为链路送到存储侧响应端；响应端访问存储后，将状态和读数据送回 AXI B/R 通道。

```text
AXI 激励 ⇄ Axi2Flit ⇄ UcieAouAdapter ⇄ UcieLink ⇄ AouTarget ⇄ SimpleBurstMemory
         AXI 信号       帧握手          帧 FIFO             突发事务 FIFO
```

## 能力与边界

- SoC 侧：AXI 五通道信号接口，64 位字节地址、10 位 ID、16 位 USER，数据宽度支持 256/512/1024 位。
- 传输：支持按 SIZE 对齐、不跨 4KB 的 INCR 突发、字节选通和窄访问；每笔最多 256 拍，同时受 4KB 边界限制。
- 桥内部：1～4 个资源平面、分类型 credit 流控、跨帧消息续传和双向背压。
- 存储侧：整笔突发请求/响应 FIFO。简单内存保存实际字节数据，支持读写、掩码和错误响应。
- 全链路：实例化 UcieLink，包含链路训练、行为物理层、CRC 和重放；生成 AXI 波形、事务记录和自动检查结果。

本目录是时钟驱动桥、事件驱动链路和事务级存储的联合行为仿真。桥本身不包含 RTL 实现、DRAM 引脚时序、独占访问执行、原子操作或运行期间单端热复位。项目级计算端使用 CoralNPU RTL 生成模型。链路帧头和 CRC 的具体行为见字节格式文档。

## 主机与存储接入入口

| 接入方 | 当前可连接的边界 | 首先阅读 |
|---|---|---|
| SoC/主机开发者 | `Axi2Flit` 的 AXI 从机端口：AW/W/AR 输入，B/R 输出 | [SoC 侧接口对接表](docs/SoC侧接口对接表.md) |
| 存储开发者 | `AouTarget.mem_req/mem_rsp`，整突发 `SimpleMemRequest/Response` FIFO | [存储侧接口对接表第 1～3 节](docs/存储侧接口对接表.md) |
| 联合仿真顶层开发者 | 项目主程序位于 `../../axi_StorageStacked/examples/axi_storage.cc`；跨组件全链路测试见 [`tests/link`](../../axi_StorageStacked/tests/link/) | [联合仿真接入指南](../../axi_StorageStacked/docs/全链路联合仿真接入指南.md) |

项目级适配已实现：`../../axi_StorageStacked/examples/axi_storage.cc` 将文件输入转为 AXI256；外部处理器驱动通过公开信号端口接入，`../../axi_StorageStacked/storage_axi/memsim_backend.cc` 连接在线 mem_sim。`UcieAouAdapter` 是桥到 UCIe 的内部适配。SimpleBurstMemory 仅用于本目录组件测试，不能用于项目计算和访存验收。

## 构建与运行

需要 g++、make 和 C++17 构建的 SystemC。默认安装路径为 `$HOME/.local/systemc-2.3.4-cxx17`，可通过 `SYSTEMC_HOME` 覆盖。波形页面生成需要 Python 3 标准库，GTKWave 可用于查看原始 VCD。

从仓库根目录执行链路集成的 `./axi_StorageStacked/run.sh run` / `test`。在本组件目录执行：

```bash
make CXX="$CXX" SYSTEMC_HOME="$SYSTEMC_HOME" preflight
make -C systemc report
```

全链路测试从仓库根目录执行：

```bash
./axi_StorageStacked/run.sh link-test preflight
./axi_StorageStacked/run.sh link-test full-link-wave
```

组件产物位于 `systemc/sim/`；跨组件产物位于 `axi_StorageStacked/tests/link/sim/`。
`perf_report.txt` 是简化链路下的桥性能报告，不能代替全链路持续带宽测量。

## 目录与文档

| 路径 | 内容 |
|---|---|
| `systemc/include/`、`systemc/src/` | 接口、消息编码、 credit 管理及桥实现 |
| `../../axi_StorageStacked/storage_axi/link/` | 跨组件链路适配器、响应端与简单测试存储 |
| `systemc/tb/` | 桥、边界配置与性能测试 |
| `../../axi_StorageStacked/tests/link/` | 全链路测试与波形工具 |
| `systemc/sim/` | 可再生的构建与仿真产物 |

- [设计文档](docs/systemc/design.md)：模块职责、握手、容量和顺序规则。
- [字节格式与接口](docs/systemc/wire_contract.md)：消息、帧头、物理布局和接线约定。
- [SoC 侧接口对接表](docs/SoC侧接口对接表.md)：端口方向、字段、窄访问、握手和主机接入职责。
- [存储侧接口对接表](docs/存储侧接口对接表.md)：请求/响应字段、功能、单 RP 简化及外部存储模型映射。
- [验证文档](docs/systemc/verification.md)：测试范围、判据、性能口径和验证边界。
- [全链路使用说明](../../axi_StorageStacked/docs/UCIe全链路仿真计划与使用.md)：场景配置、产物和波形判读。
- [全链路联合仿真接入指南](../../axi_StorageStacked/docs/全链路联合仿真接入指南.md)：CoralNPU 原生接口、独立 SystemC 调度、在线 mem_sim 与验收。
- [接入与交付说明](../../axi_StorageStacked/storage_axi/link/README.md)：依赖安装和路径迁移。

## 当前目录与开发入口

### 职责

AXI 与 AoU Flit 编解码组件。

### 对外接口

见 [接口说明](docs/存储侧接口对接表.md)；跨组件连接位于 `axi_StorageStacked/storage_axi/link`，源码与检查器路径由 `storage_manifest.json` 描述。

### 构建与测试

在本目录执行；先按 [原生开发文档](../../docs/development.md) 准备工具链及兼容的 SystemC。

```bash
make CXX="$CXX" SYSTEMC_HOME="$SYSTEMC_HOME" preflight
```

### 文档目录

见 [模块文档](docs/systemc/verification.md)。

### 负责人

以仓库维护者及当前模块协作分工为准。

## 组件验收证据

从本组件目录执行，复用调用方选定的 SystemC：

```bash
source ../../common/toolchain/env.sh
export SYSTEMC_HOME=$(../../axi_StorageStacked/run.sh systemc-prefix)
export SYSTEMC_CXX_STANDARD=${STORAGE_CXX_STANDARD:-17}
make CXX="$CXX" SUMMARY_DIR="$PWD/build/verification" preflight
```

`SUMMARY_DIR/summary.json` 同时记录实际子测试数、期望数、命令、退出状态与程序自己输出的 PASS/FAIL 计数；少跑或缺少明确检查输出均失败。期望数由 Makefile 的位宽和场景列表推导。历次日志和结论保存在其 `runs/` 子目录。
