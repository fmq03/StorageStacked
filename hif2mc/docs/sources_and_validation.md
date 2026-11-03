# 来源与验证记录

## 资料与决策边界

本版使用本项目对话“hif2mc 疑问解答”及“ HIF接口整理 ”的讨论；后一对话的
读、普通写、RMW 波形解释已核对。核心要求包括：

- valid/stall 是沿采样的接收条件，停顿不重复接收。
- pointer 返回不是写完成；最小两拍通知、通知后最小一拍传数据。
- 写数据没有逐拍 pointer，因此必须维护通知顺序。
- 读允许按 token 乱序返回；end 是数据边界，不是 credit。
- credit 在所代表资源释放时返回，不应固定每隔 N 拍或随机返回。
- RMW 消耗 LPR+WR，内部读不作为普通读输出；两类资源分别释放。

原始 HIF WPS 与两张端口截图本轮已确认可访问；文件 SHA256：

| 文件 | SHA256 |
|---|---|
| HIF接口.wps | `23d14e09b8b2860bd32a9af1af3de698748e16cb8b800c147042df2d86f0f1b0` |
| HIF接口list1.jpg | `c95631caacdc0161deb7d149e420c52ed4b58e9fc19fe9bb52280025cbfdc5c5` |
| HIF接口list2.jpg | `4ed3f165d325d295d75ce8704131924ac381fb69cc07fb7b1224c9a19f1b6152` |

未将现有 Bridge RTL 的 mc_req/mc_rsp 当作 HIF 规范，也未修改它。
参考配置和无法证明为厂商规定的行为均在 [接口契约](interface_contract.md) 中标注。

## 开源工程参考

本模块独立编写，没有把这些工程的 AXI 信号改名后直接当作 HIF。

| 来源 | 本轮检查版本 | 借鉴的设计职责 |
|---|---|---|
| [PULP dram_rtl_sim](https://github.com/pulp-platform/dram_rtl_sim) | `a764d5200a19` | RTL 协议前端、软件内存后端、独立时间推进分层 |
| [Vortex dram_sim.cpp](https://github.com/vortexgpgpu/vortex/blob/50cb174546ec/sim/common/dram_sim.cpp) | `50cb174546ec` | 待提交队列、后端拒收重试、父子事务汇集、时钟余量 |
| [Xilinx axi2tlm-bridge.h](https://github.com/Xilinx/libsystemctlm-soc/blob/788e8b24b5d6/tlm-bridges/axi2tlm-bridge.h) | `788e8b24b5d6` | 信号转事务、字节使能、读写通道分离、顺序保护 |

PULP 的已说明运行环境为 Questa/ModelSim，不把它当作已有的 Verilator HIF 方案。
Vortex 包装器会在后端接纳后自行产生写回调，本模块不沿用这个完成语义。
这些参考均不能提供本项目专用的 HIF pointer、LPR/HPR credit 和 RMW 契约。

## 当前可重复验证入口

`tests/adapter_tests.cpp` 包含两组测试：

1. 独立时序内存后端：命令发射与完成时间分离、可拒收、可乱序，检查接口状态机。
2. 当前源码构建的真实 mem_sim：Direct、行为级 PHY、双 stack；核对真实数据与统计。

测试覆盖缺失契约拒绝、未支持控制报错、53bit token、pointer 最小延迟、
真实逻辑 RMW、内部读不外泄、两类 credit 分别返还、读 credit 早于数据、
写 credit 早于完成、同沿返还与接收、最后一拍停顿、子事务重试无重复、
响应乱序、地址/ECC 错误、热复位拒绝、空闲复位保留数据、组合求值不推进时间和非整数时钟比例。

`examples/roundtrip.cpp` 使用完整 HBM4 基准与行为级 PHY，验证写→读、RMW→读。
`examples/HifDemoMaster.sv` 通过 Verilator 产生并检查真实 HIF 信号，验证相同适配器接线。
它不是 Bridge，也不代替后续完整链路回归或第三方 MC 接入验证。

2026-11-02 验证：Clang 18.1.8 Debug 构建，`hif2mc_` 前缀 CTest 4/4 通过；
包含独立协议后端的 9 个场景、真实 mem_sim 的 3 个配置、C++ 回读示例和 Verilator 示例。
Clang ASan/UBSan 构建的同前缀 CTest 3/3 通过（不含 Verilator）。
Verilator 版本 5.048。回读示例为 HIF 58 周期 / 后端 464 tick，
2 笔读、1 笔普通写、1 笔 RMW，共 10 个子事务；PHY 完成读 6 笔、写 4 笔。
Verilator 示例 31 周期完成；这些是该例的功能运行记录，不是性能标定。

验收命令见 README；只运行 `hif2mc_` 前缀测试，不把未执行的后端
全部回归、厂商 HIF 合规、综合/时序/PPA 或完整链路性能视为已验证。

## StorageStacked gmx 移植验证（2026-11-03）

源码来自 `/home/guxing/project/UCIe/hif2mc`，移入 StorageStacked 的普通目录
`hif2mc/`，未复制旧构建与结果。适配器、契约、测试、示例及 CMake 保持原样；
补充仓库使用入口与本记录。源目录本身没有独立 Git 仓库，因此不标注源提交号。

后端使用目标仓库原有 `mem_sim/`（移植前主仓库提交 `81b3788b`），未替换后端源码、
在线接口或外部子模块，也未改变 CPU/GPU/NPU 原生链路。

在目标仓库重新构建，Clang 18.1.8 Debug + Verilator 5.048 的
`ctest --test-dir hif2mc/build -R '^hif2mc_' --output-on-failure` 为 4/4 通过；
Clang ASan/UBSan 构建的同前缀测试为 3/3 通过。
显式运行回读示例得到 `HIF2MC ROUNDTRIP PASS`，58 HIF 周期 / 464 后端 tick；
Verilator 示例得到 `HIF2MC VERILATOR PASS cycles=31`。
本地 CSV、VCD 保存在 `hif2mc/results/`，不提交到 Git。

此次验证限于独立 hif2mc 模块与目标 mem_sim 的连接，未运行后端全部回归或完整系统仿真。
