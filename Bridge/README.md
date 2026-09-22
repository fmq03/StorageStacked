# Bridge：UCIe 链路 ↔ 堆叠存储之间的协议转换与适配层

本目录同时保留 Bridge 的 RTL 实现、SystemC/C++ 联仿适配层和 C++ 黄金参考。
当前正式 RTL 路径是：

```
CPU/GPU/NPU -> gem5 原生 TLM -> AXI256 -> AXI2Flit -> UCIe Link
                                                     │ 250B FDI PLP
                                                     ▼
                                            storage_bridge_top RTL
                                                     │ MC req/rsp
                                                     ▼
                                       RtlAouTarget / 联仿 transactor
                                                     │ SimpleMemRequest
                                                     ▼
                                             MemSimBackend -> mem_sim
```

RTL Bridge 是 **UCIe/AoU 协议域到内存控制器事务域**的终结与转换层。它负责 FDI
PLP 收发、AoU 消息编解码、credit、请求与写数据配对、burst 展开与 32B 边界事务
生成、lane/mask 转换、tag/outstanding/reorder，以及响应重新打包。RTL 下游是固定的
MC `valid/ready` 请求/响应流，不是 AXI，也不包含 DRAM 调度、DFI、PHY 或物理链路
CRC/重放。

`RtlAouTarget`/联仿 transactor 把 MC 信号转换成现有 `SimpleMemRequest`，复用
`MemSimBackend` 完成地址窗口检查、C ABI 调用和 mem_sim 时间推进。这一层是仿真
接缝，不属于最终 Bridge RTL。原 `AouTarget + MemSimBackend` 路径继续保留为 C++
黄金参考；`Gem5AxiAgent` 和 AF_UNIX socket 路径只用于独立验证，不是正式 gem5
原生链路的一部分。

RTL 连续实现位于 [`rtl/`](rtl/README.md)，总体路线和阶段状态见
[`RTL_IMPLEMENTATION_PLAN.md`](RTL_IMPLEMENTATION_PLAN.md)。当前 RTL 已完成
Verilator/SystemC 下的 AXI/UCIe/mem_sim 联合闭环，并已接入 gem5 原生 SystemC
进程；现有 C++ 路径继续作为黄金参考。

当前代码对应的顶层连接、事务引擎展开和端口说明见
[Bridge RTL 内部详细结构图](doc/RTL_STRUCTURE.md)，附 SVG/PNG 和可编辑图源。

## RTL 模块边界

```text
storage_bridge_top
  storage_bridge_rx
    fdi_rx_unpack -> aou_msg_decode
  bridge_transaction_engine
    bridge_req_buffer -> bridge_fifo
    bridge_scheduler -> bridge_read_fsm / bridge_write_fsm
    bridge_outstanding -> bridge_reorder_buffer -> bridge_response_gen
    bridge_assertions
  bridge_credit_mgr
  fdi_tx_packer
```

`bridge_transaction_engine` 是编排层，仍持有 parent 表、每 RP 顺序队列、credit
归还时机和单项 MC issue 寄存器；其余主要数据通路和状态已经拆入独立模块。各模块
寄存器归属、容量和握手时机见 [RTL 内部详细结构图](doc/RTL_STRUCTURE.md)。

## 独立性

**上游三个仓库（axi2flit / ucie-model / gem5）全程只读**，不写回、不推送。
依赖由 `scripts/vendor.sh` 收集到 `vendor/` 下，并对其中的 ucie-model 副本应用
上游提供的 AOU 接入补丁。`vendor/` 可随时删除重建：

```bash
make -C Bridge vendor      # = Bridge/scripts/vendor.sh
```

## 构建与运行

以下命令均从主仓库根目录执行：

```bash
make -C Bridge vendor        # 1. 收集依赖并打 AOU 补丁
make -C Bridge mem-sim       # 2. 编译 mem_sim 静态库（hbm_sim_core）
make -C Bridge all           # 3. 编译全部测试程序
make -C Bridge test          # 4. 运行单元测试
make -C Bridge full-link-all # 5. 全链路：3 位宽 x RP 1/2/4 + 压力重放
make -C Bridge mock-chain    # 6. 配 mock gem5 的整条链路（无需真 gem5）
bash Bridge/scripts/run_gem5_chain.sh # 7. 真 gem5 C++ 参考链路

make -C Bridge rtl-lint        # RTL lint
make -C Bridge rtl-test        # RTL 定向回归
make -C Bridge rtl-memsim-test # RTL MC 端口直接连接真实 mem_sim
make -C Bridge rtl-full-link   # AXI -> UCIe -> RTL Bridge -> mem_sim -> AXI
make -C Bridge rtl-diff        # RTL 与 C++ 路径确定性对拍
RTL_BRIDGE=1 bash Bridge/scripts/run_mock_chain.sh

# 原生 gem5 进程内的 RTL Bridge 路径
bash env/build.sh
gem5/build/AXI/gem5.opt --outdir=results/rtl-gem5 \
  --listener-mode=off gem5_axi/configs/run.py --mode tester \
  --backend aou --bridge-impl rtl --memory-backend memsim --planes 1
```

`--bridge-impl cpp` 保留原 `AouTarget`，`--bridge-impl rtl` 切换为 Verilated
`storage_bridge_top`。两条路径共用原 `MemSimBackend`，因此内存模型、C ABI、统一
1fs 时间轴和输出格式不变。`env/build.sh` 先生成 Verilator C++ 模型，再由 gem5
SCons 使用同一编译器编译；模型不启用 Verilator SystemC 模式，不会引入第二套
SystemC 内核。

`make help` 列出全部目标。构建参数可在命令行覆盖：

| 变量 | 默认 | 说明 |
|---|---|---|
| `AXI_DATA_WIDTH_CFG` | `512` | C++ 参考/全链路 testbench 的最大 AXI 位宽；RTL 当前固定 256bit |
| `SYSTEMC_HOME` | `/usr` | 本机是 SystemC 3.0.2 的发行版包 |
| `MEM_SIM_DIR` | `../mem_sim` | mem_sim 仓库位置 |
| `FULL_LINK_ARGS` | 空 | 传给全链路 tb 的运行参数，如 `--rp 2 --stress` |

## 验证结果

| 阶段 | 内容 | 结果 |
|---|---|---|
| **P0a** | vendored 依赖自检（Config 契约 + 链路训练到 Active） | 10/10 |
| **P0b** | 纯 C++ 后端自证：参考字节数组对拍、定向 `decoded` 塌缩探测 | 14/14，2000 次随机读写 0 处不一致 |
| **P1** | Bridge 单元测：lane 旋转、三类预校验错误、byte_mask 语义、多拍突发 | 23/23 |
| **P2** | 全链路替换：上游 `tb_full_link` 的 12 用例 | **144/144**（12 配置 × 12 用例） |
| **P3a** | gem5 在线事务协议回环（假 server、纯 C++） | 29/29 |
| **P3b** | 整条链路配 mock gem5（自带字节级参考模型） | 15/15，4KB 突发逐字节一致 |
| **P3b** | **真 gem5**（X86/SE, v25.1.0.1）：gemm 算子，779 笔访存 | 全部经 UCIe 进入 mem_sim，0 越界、0 错误 |
| **RTL P0-P4** | 15/15 计划模块、FDI/AoU、credit、8 parent/8 child、tag/reorder、WData FIFO | lint 0 warning，Verilator 174 项定向检查通过 |
| **RTL P5** | RTL MC 端口连接真实 HBM4 mem_sim | 写回读逐字节一致 |
| **RTL P6** | AXI2Flit/UCIe/RTL Bridge/mem_sim 联合仿真 | 12/12，用例共 55 写、66 读 |
| **RTL P7a** | CommMonitor socket + mock gem5 + RTL Bridge | 15/15，278 个子事务，逐字节一致 |
| **RTL P7b** | gem5 原生 SystemC tester + RTL Bridge + mem_sim | 17/17，176 个 MC 请求，`drained=true`，0 顺序错误 |
| **RTL P7c** | 双 RP NPU/GPU/三源/慢时标 + RTL Bridge | 4/4，全部计算、AXI/UCIe、DRAM/DFI、内存镜像校验通过 |

P7b 中 176 个 MC 请求是长 burst 按 32B 边界聚合后的事务；2 个 memory error 是越界测试的
预期 DECERR。gem5 在最后一个数据响应到达后立即退出，纯 credit 控制帧可能尚在链路
中，因此 `target_idle=false` 不表示数据事务未排空。

P2 的压力重放场景确实走通了错误路径：CRC 错误 71–78 次、实际重传 222–245 次、
`max_outstanding=8` 打满、credit_stalls 数千次；1024 位宽还触发了跨帧续传
（`rx_spanning`/`tx_spanning` 非零）。

### P0b 的负向对照

一个不会失败的测试没有价值。把 `mem_backend.cpp` 里的
`req.decoded = mapper.decode(addr)` 改成 `req.decoded = {}` 后重跑，
P0b 立刻报出 **92 处数据不一致**。这正是它要抓的缺陷：

`MemorySystem::localize_request` 在 `stack_count == 1` 时执行
`req.storage_decoded = req.decoded`（mem_sim `src/core/system.cpp:432`），**原样
拷贝调用方的坐标、不按地址重新解码**。留空（全 0）会让所有 cache line 塌缩到
同一个 `(bank,row,column)`，行缓冲返回上一条线的字节——读回错误数据且不抛异常。

### 地址窗口必须与 gem5 对齐（一个已经踩过的坑）

Bridge 有一个 `[base, base+size)` 的地址窗口，落在窗口外的访问会被预校验直接
拒掉（DECERR），**不进入 mem_sim**。如果窗口与 gem5 实际访问的物理范围不重叠，
结果是：链路机械上完全跑通、gem5 也正常退出，但 `submitted_txns == 0`——
mem_sim 一个字节都没动。在 `--axi-direct-shadow` 下 gem5 仍从 SimpleMemory 拿到
正确响应，程序照常跑完，**表面看完全正常**。

所以 `tb_gem5_full_chain` 里有一条专门的判据：`completed > 0 且 submitted_txns == 0`
即判失败。默认窗口取 `[0, 2 GiB)`，与 `soc_axi.py --memory-size` 的默认值一致
（mem_sim 的 HBM4 单 channel 容量恰好也是 2 GiB）。窗口可用 `--base`/`--size` 覆盖。

## 与 `SimpleBurstMemory` 的接口差异

`MemSimBackend` 的端口、构造签名与 `completed`/`error_responses` 计数器都与
`SimpleBurstMemory` 一致，可直接替换。**唯一差异是它多两个端口**：

```cpp
memory.clk(clk);      // SimpleBurstMemory 是纯 FIFO 驱动的行为模型，没有时钟
memory.rst_n(rst);    // Bridge 后面挂的是 mem_sim，时间轴由 tick 推进
```

时间比例：SystemC 时钟 2 ns（500 MHz），mem_sim HBM4 一个 tick 是 250 ps
（tCK 500 ps ÷ tick_multiplier 2），因此**每个时钟沿推进 8 个 tick**。

## 构建标准为什么是分裂的

两个方向的约束互相冲突，无法统一：

* 系统 `libsystemc` 3.0.2 是用 **C++17** 构建的（导出 `sc_api_version_..._cxx201703L`）。
  任何以 C++20 编译的 TU 链接它都会因 API 版本符号不匹配而失败——**语法检查能过，
  链接才暴露**。
* mem_sim 上游硬要求 **C++20**，但它的公开头在 C++17 下实测可编译，其静态库由
  自己的 CMake 以 C++20 构建。

于是划分成：

| TU | 标准 | 原因 |
|---|---|---|
| `src/mem_backend.cpp` | C++20 | 唯一包含 mem_sim 头的 TU |
| `src/mem_sim_backend.cpp`、`src/gem5_*.cpp`、各 tb | C++17 | 链接 SystemC |

`include/mem_backend.h` 保持 C++17 干净（不含任何 mem_sim 头），SystemC 侧只看到
这个纯 C++ 类。两边在链接期汇合。

## 已知限制

### RTL 路径

* 当前固定为 256-bit AoU data 和最大 32B MC transaction；512/1024-bit 尚未实现。
* 下游只有一组 MC req/rsp，不输出 channel/bank/row/column；地址窗口检查和 DRAM
  地址映射仍由 transactor 与 mem_sim 完成。
* RTL 保持每 RP 的响应顺序，不同 RP 可互相绕过；尚未完成 RP_COUNT=4 系统压力回归。
* 只验证启动复位，不支持有在途事务时热复位。
* 当前 64 KiB reorder 数据数组在复位时清零，尚未进行综合、RAM 推断、面积和时序收敛。
* **`UninitializedData` 映射为 AXI OK**。mem_sim 对从未写过的位置返回该状态；当前
  联仿将其视为零初始化读。这是仿真状态到 AXI 状态的有损映射。

### C++ socket 验证路径

以下限制只适用于 `Gem5AxiAgent`/AF_UNIX socket 脚手架，不适用于 gem5 原生
TLM -> AXI256 -> AoU -> RTL 路径：

* socket 在线事务协议不携带 WSTRB，因此部分写会被当成全宽写；RTL AoU 路径本身
  支持 32-bit WSTRB 到逐字节 mask 的转换。
* `directAxiSegment` 同步等待响应时会冻结 gem5 事件队列，使该路径的 MLP 恒为 1；
  gem5 侧 IPC、带宽和延迟直方图不能作为性能结果。
* socket 响应 `status != 0` 会令 gem5 `fatal()`，因此错误路径主要在 Bridge 本侧
  计数并由定向测试验证。
* C++ `MemSimBackend` 参考路径按其提交队列交付完成；RTL 路径使用 per-RP
  `response_order`，两者的性能排序策略不应混为一谈。

## 目录

```
Bridge/
├── include/mem_backend.h         # 堆叠存储后端接口（C++17 干净）
├── src/
│   ├── mem_backend.cpp           # 唯一包含 mem_sim 的 TU（C++20）
│   ├── mem_sim_backend.{h,cpp}   # C++ 存储适配参考：SC_MODULE
│   ├── gem5_axi_socket.{h,cpp}   # gem5 在线事务协议客户端
│   ├── gem5_axi_agent.{h,cpp}    # socket ↔ AXI 五通道的 SystemC 模块
│   └── rtl_bridge_systemc.{h,cpp}# Verilated RTL ↔ UCIe/mem_sim transactor
├── rtl/                          # storage_bridge_top 及拆分后的 17 个 RTL 源文件
├── rtl_tb/                       # Verilator 定向与 mem_sim testbench
├── tb/                           # P0a/P0b/P1/P2/P3a 各阶段测试
├── doc/RTL_STRUCTURE.md          # 当前 RTL 层次、状态归属、端口与结构图
├── RTL_IMPLEMENTATION_PLAN.md    # 接口基线、阶段计划和当前完成状态
├── scripts/vendor.sh             # 依赖收集 + AOU 补丁
└── vendor/                       # 生成物，可删除重建
```
