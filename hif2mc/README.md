# hif2mc 第一版

`hif2mc` 是按时钟交互的 C++20 HIF 接口行为模型，把 Bridge 的 HIF 命令、
写数据和读响应连接到现有 `mem_sim`（MC + 可选行为级 PHY + Mem Stack）。
它不重新实现 DRAM 调度，不要求 MC RTL，也不依赖当前 Bridge RTL 的内部信号。

```text
HIF 主机 / 后续 Bridge RTL
          ↕ HIF 信号
Adapter：握手、pointer、credit、token、RMW、数据拍
          ↕ 请求 / 发射事件 / 完成响应
MemSimBackend → mem_sim::MemorySystem → MC → PHY → Mem Stack
```

## 构建和运行

在 StorageStacked 仓库根目录先执行 `cd hif2mc`，再运行下列命令。
本模块直接使用同仓库的 `mem_sim`，无需下载另一份后端或安装 SystemC；
现有 `env/build.sh`、`env/build_xpu.sh` 不会自动构建本模块。

在本目录执行；默认从相邻 `../mem_sim` 源码重新构建库，不复用其旧二进制：

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel 4
ctest --test-dir build -R '^hif2mc_' --output-on-failure
mkdir -p results
./build/hif2mc_example results/roundtrip.csv
```

成功标志是 `HIF2MC ROUNDTRIP PASS`。示例使用完整 HBM4 模型和行为级 PHY，
验证 64B 写回读、53bit token、RMW 更新后回读，并导出逐拍握手 CSV。
CSV 的一行是该上升沿采样到的输入/输出，不是采样后的新值。
更换后端源码位置：`cmake -S . -B build -DMEM_SIM_DIR=/path/to/mem_sim`。

CTest 中可能包含相邻后端定义的测试；这里使用 `-R '^hif2mc_'` 只运行本模块的测试。

## 可选：真实 RTL 信号联仿示例

安装 Verilator 后，启用以下目标：

```bash
cmake -S . -B build -DHIF2MC_VERILATOR_DEMO=ON
cmake --build build --target hif2mc_verilator_demo --parallel 4
./build/hif2mc_verilator_demo results/hif_demo.vcd
ctest --test-dir build -R '^hif2mc_' --output-on-failure
```

成功标志是 `HIF2MC VERILATOR PASS`。`examples/HifDemoMaster.sv` 是独立、
自检查的 HIF 流量源，不是现有 Bridge。它发出写命令，等待 pointer，发送两拍数据，
收到写 credit 后发出同地址读，并检查每个字节、token、end 和错误信号。
此路径已连接真实 `mem_sim`；不意味着完整 gem5/UCIe/Bridge 链路已完成集成。

## 支持范围

- 普通读写、逐字节 mask、独立命令/数据反压。
- 多笔在途、父子事务拆分、后端拒收重试、读响应乱序配对。
- 写 pointer 最少两拍返回，返回后最少一拍再接收数据；写数据按通知顺序、不交织。
- 可启用的真实逻辑 RMW（读旧值、合并、写回），分别管理 LPR 和 WR credit。
- 同地址依赖保护：较晚请求不会越过尚未收齐数据或尚未完成的较早写/RMW。
- HIF 读返回不提供 ready；接收命令时预留完整上下文，完成数据不丢弃。
- 地址错误、不可纠正 ECC 映射；不支持的控制、错误或热复位会明确报错。

`Config{}` 故意不能直接运行。调用方必须提供接口合同，或显式选择
`Config::reference_64b()`。该参考配置是项目选择，不是原厂唯一配置。
完整端口、参数来源、时序定义、错误规则与限制见 [接口契约](docs/interface_contract.md)。

## 如何接入后续 Bridge

1. Bridge 顶层输出 HIF 命令和写数据，接收本模型的 stall、pointer、credit 和读响应。
2. 每个上升沿之前稳定输入，调用 `evaluate(inputs)` 得到本沿要采样的输出。
3. 双方在同一个上升沿采样一次，只调用一次 `tick(inputs)`；它准备下一沿的输出。
4. `evaluate()` 不推进时间。Verilator 组合求值可能执行多次，但不能因此重复 tick。
5. 停止发命令后继续推进，直到 `idle()`，再调用 `finish()` 汇总后端统计和保存存储。

`include/hif2mc/verilator_binding.hpp` 提供无状态逐端口连接；它要求顶层端口名称及
宽度与本契约一致。不同命名应增加明确的包装层，不读取 RTL 内部对象。
可直接参考 `examples/verilator_main.cpp` 的采样/求值顺序。若其他设计存在
valid 与 stall 的组合依赖，连接层还必须在上升沿前完成组合收敛，不能照搬一次求值。

后端 API 接受实际 `DramSpec` 和 `MemorySystemOptions`，因此可接入现有配置装配结果。
示例用 `config::build_model("hbm4")` 加上显式 PHY/队列选项，不声称加载了整个
`configs/hbm.cfg`；不复制 CLI 的全部配置装配逻辑。

## 代码入口

| 文件 | 作用 |
|---|---|
| `include/hif2mc/hif_signals.hpp` | 两个方向的 HIF 信号快照 |
| `include/hif2mc/config.hpp` | 必需参数、具名参考配置与信用返还策略 |
| `src/adapter.cpp` | 信号行为、请求生命周期、RMW 和错误检查 |
| `src/memsim_backend.cpp` | 实际 mem_sim 请求、地址映射和响应转换 |
| `examples/hif_master.hpp` | 示例/测试用的有 credit 计数的 C++ HIF 主机 |
| `tests/adapter_tests.cpp` | 独立时序后端测试与真实 mem_sim 集成测试 |

可选内存检查：

```bash
cmake -S . -B build-asan -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_COMPILER=clang++-18 -DHIF2MC_SANITIZE=ON
cmake --build build-asan --target hif2mc_tests hif2mc_example --parallel 4
ctest --test-dir build-asan -R '^hif2mc_' --output-on-failure
```

原始资料、另一对话和开源实现参考见 [来源与验证记录](docs/sources_and_validation.md)。
