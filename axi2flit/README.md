> 本目录现为 StorageStacked 主仓库普通源码；同级 `ucie-model` 直接维护链路实现，
> 公共帧格式在 `../protocol/include`。整机使用在线 mem_sim，见[统一环境](../env/README.md)。
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

这是时钟驱动桥、事件驱动链路和事务级存储的联合行为仿真。项目不包含 RTL 实现、DRAM 引脚时序、独占访问执行、原子操作或运行期间单端热复位。链路帧头和 CRC 的具体行为见字节格式文档。

## 主机与存储接入入口

| 接入方 | 当前可连接的边界 | 首先阅读 |
|---|---|---|
| SoC/主机开发者 | `Axi2Flit` 的 AXI 从机端口：AW/W/AR 输入，B/R 输出 | [SoC 侧接口对接表](doc/SoC侧接口对接表.md) |
| 存储开发者 | `AouTarget.mem_req/mem_rsp`，整突发 `SimpleMemRequest/Response` FIFO | [存储侧接口对接表第 1～3 节](doc/存储侧接口对接表.md) |
| 联合仿真顶层开发者 | 参考 `tb_full_link.cpp` 装配模块和监视器 | [联仿交接约定](doc/全链路联合仿真接入指南.md#11-联仿交接约定) |

gem5/Vortex/Ramulator 适配器、trace 文件回放器和独立的统一链路顶层尚未实现。文档中的精简存储接口属于可选设计方案，不能直接替代当前头文件；首轮集成使用现有类型，后端不匹配的部分由包装层转换。Adapter 是桥到 UCIe 的内部适配，不是主机接口。

## 构建与运行

需要 g++、make 和 C++17 构建的 SystemC。默认安装路径为 `$HOME/.local/systemc-2.3.4-cxx17`，可通过 `SYSTEMC_HOME` 覆盖。波形页面生成需要 Python 3 标准库，GTKWave 可用于查看原始 VCD。

在项目根目录执行：

```bash
# 桥功能与性能测试，可独立于 UCIe 模型运行。
make test-all
make -C systemc report

# 全链路测试需要按接入说明准备 UCIe 依赖和补丁。
make full-link
make full-link-all
make full-link-negative

# 从默认全链路测试生成的 VCD 创建波形页面。
make full-link-wave
```

链路模型默认位于 `reference/ucie-model`，安装方法见[接入说明](systemc/integration/README.md)。更换位置可执行：

```bash
make UCIE_DIR=/work/models/ucie-model full-link-all
make SYSTEMC_HOME=/work/systemc UCIE_DIR=/work/models/ucie-model preflight
```

`UCIE_DIR` 的相对路径以 `systemc/` 为基准。`preflight` 运行桥和链路组件回归，`full-link-all` 运行实际链路与存储的联合功能回归，两者需要分别执行。

产物位于 `systemc/sim/`。`perf_report.txt` 是简化链路下的桥性能报告；`full_link_*.log/.csv/.vcd` 是全链路功能结果。性能报告不能代替全链路持续带宽测量。

## 目录与文档

| 路径 | 内容 |
|---|---|
| `systemc/include/`、`systemc/src/` | 接口、消息编码、 credit 管理及桥实现 |
| `systemc/integration/` | 链路适配器、响应端、简单存储及依赖补丁 |
| `systemc/tb/` | 桥、组件、性能和全链路测试 |
| `systemc/scripts/` | 波形页面生成工具 |
| `systemc/sim/` | 可再生的构建与仿真产物 |

- [设计文档](systemc/doc/design.md)：模块职责、握手、容量和顺序规则。
- [字节格式与接口](systemc/doc/wire_contract.md)：消息、帧头、物理布局和接线约定。
- [SoC 侧接口对接表](doc/SoC侧接口对接表.md)：端口方向、字段、窄访问、握手和主机接入职责。
- [存储侧接口对接表](doc/存储侧接口对接表.md)：请求/响应字段、功能、单 RP 简化及外部存储模型映射。
- [验证文档](systemc/doc/verification.md)：测试范围、判据、性能口径和验证边界。
- [全链路使用说明](doc/UCIe全链路仿真计划与使用.md)：场景配置、产物和波形判读。
- [全链路联合仿真接入指南](doc/全链路联合仿真接入指南.md)：现有接口、gem5/Vortex/Ramulator 适配、时间同步及验收计划。
- [接入与交付说明](systemc/integration/README.md)：依赖安装和路径迁移。
