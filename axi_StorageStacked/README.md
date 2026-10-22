# axi_StorageStacked

阅读入口：[文档目录](docs/README.md) · [逐文件中文讲解](https://github.com/hy2581/StorageStacked-docs)。

独立的 **AXI256 信号接收 → AXI2Flit → 双向 UCIe → 在线 mem_sim** 项目。
接收 AW/W/AR 与 BREADY/RREADY，返回 AWREADY/WREADY/ARREADY、B 写响应和 R 读数据。
读写数据、字节使能、响应状态和延迟均沿实际模型链路传递。

```text
vortex_StorageStacked：gem5 + Vortex → TLM → AXI256 ─┐
                                                  ├→ 本项目 → 在线内存
coralnpu_StorageStacked：CoralNPU RTL → AXI256 ──────┘           │
                            ← B/R 数据与完成响应 ←─────────────┘
```

本项目不依赖两个驱动，也没有处理器私有代码。两个驱动引用本项目的公开端口和源码；
不复制本项目，不使用 Git submodule。两个驱动均在各自仓库根目录执行 `./build.sh --storage ../axi_StorageStacked` 保存相对路径。
每个驱动有独立构建目录，两个仿真进程分别拥有自己的内存状态。

## 单独构建与输入输出

Linux x86-64，C/C++ 编译器（C++20）、CMake ≥ 3.20、make、Python 3 和 Boost 头文件。
SystemC 2.3.1 源码直接存放于 `third_party/systemc`，不需要安装全局 SystemC。

```bash
git clone https://github.com/hy2581/axi_StorageStacked.git
cd axi_StorageStacked
./run.sh setup
./run.sh build
./run.sh smoke
./run.sh run --input examples/roundtrip.json --output results/roundtrip
./run.sh test --output results/acceptance
```

`setup` 只检查工具和源码；`smoke` 执行固定的六笔真实 AXI 读写；`run` 执行自定义事务并校验，
`test` 执行完整验收。每次使用新的结果目录，成功以 `summary.json` 中 `passed: true` 为准；
SMOKE 还要求 `smoke_passed: true` 和 `smoke_summary.json` 中 `passed: true`。

详细文档：

- [输入说明](docs/INPUTS.md)：五通道输入、JSON 字段、配置范围、字节排列和 SMOKE 输入。
- [输出说明](docs/OUTPUTS.md)：AXI 响应、结果文件、SMOKE 实测结果及本次源码／运行状态改动。
- [从零添加 SMOKE 测试](docs/03-从零添加SMOKE测试.md)：项目参数表、修改位置，以及添加测试、运行和验收的操作步骤。

输入 JSON 由 `config` 和 `transactions` 构成。命令行入口把事务转换为真实
SystemC 五通道握手，适合独立试验；处理器集成直接使用下面的信号端口。

```json
{
  "config": {"scale": 1},
  "transactions": [
    {"command": "write", "id": 1, "address": 2415919104,
     "size": 2, "data": ["29000000"], "strobe": [15]},
    {"command": "read", "id": 1, "address": 2415919104,
     "size": 2, "beats": 1, "expected": ["29000000"]}
  ]
}
```

`size` 为 AXI AxSIZE（每拍字节数的 log2，0～5）；`beats` 为 1～256。
`data` 每个字符串对应一拍，按地址从低到高列出字节。
`strobe` 每拍一个整数，bit 0 对应该拍第一个字节；省略表示全部字节有效。
程序自动放到相应 AXI 总线字节通道。`expected` 可选，仅适用于读。
所有读返回还会与独立的字节级参考模型比较，未写入地址初值为零。
合法但超出存储窗口的请求可用 `expected_response: 3` 检验 DECERR。

`responses.json` 按输入顺序输出每笔操作的地址、ID、AXI response、读数据、开始和完成时间（fs）。
结果目录还包括输入快照、AXI VCD/五通道 CSV、两端 UCIe Flit、内存桥接日志、
DRAM 命令、DFI、最终内存镜像，以及从原始波形和 Flit 独立解码的验收结果。
`--scale 4` 放慢内存，`--replay` 注入链路错误并检查重放。

配置完整默认值和范围见 `scripts/run.py` 的 `DEFAULTS` / `normalize`。
默认窗口为 `0x90000000` 起、`0x30000000` 字节；AXI 周期 2 ns，时间分辨率 1 fs。
支持配置内存标准、通道数、队列、在途槽位、额外响应等待、资源平面和 watchdog。

## 处理器集成

公开头文件为 `storage_axi/axi_signals.hh`、`storage_axi/storage_config.hh`、
`storage_axi/aou_backend.hh`；详细契约见 [INTERFACE.md](INTERFACE.md)。

```cpp
sc_core::sc_set_time_resolution(1, sc_core::SC_FS); // 在构造任何模块前
storage_axi::Signals wires;
storage_axi::StorageConfig config;
config.trace_dir = "results/my-run"; // 事先创建目录
storage_axi::AouBackend memory("memory", config);
memory.clk(clock);
memory.resetn(resetn);
memory.axi.bind(wires);
driver.axi.bind(wires);
```

独立 SystemC 应用通过 `add_subdirectory` 和 `StorageStacked::axi` 构建/链接，
可设置 `STORAGE_BUILD_CLI=OFF`。CoralNPU 驱动提供完整用例。
gem5 通过 `EXTRAS=<本项目>/storage_axi`，把相同的公共源码编译到 gem5 自带 SystemC 中；
Vortex 驱动在 `integration/` 保留 gem5/TLM 类型转换，公共存储实现不引用 gem5 头文件或符号。
两个驱动均提供 `user/run.sh smoke` 和 `user/run.sh llm`，配置和结果分别位于各项目目录内。
gem5 集成可通过内部 `STORAGE_LINK_CONFIG` 指向 UCIe 参数 JSON；SCons 只为相关源文件设置
编译定义，未设置时继续采用公共默认值。日常用户在驱动的项目 JSON 中修改 UCIe 参数即可。
不要把两种 SystemC 内核装入同一个进程。

## 验收与边界

`./run.sh smoke [--output 新目录]` 验证初始零值、两拍 256 位写回读、窄拍掩码写和 B/R 反压，
并核对 VCD、Flit、在线内存子请求、DRAM/DFI 和最终镜像。它不自动构建，先执行 `./run.sh build`。

`./run.sh test` 包含 SMOKE，并检查原生内存测试、C ABI、完整字节回读、多拍、窄拍、部分写入、
ID 重用、4 KiB 页末访问、B/R 反压、内存延迟反馈、重放、DECERR，
并拒绝非法输入和被篡改的读返回。

AXI 为对齐、非独占的 INCR 子集；有效 ID 为 1～1023，单 burst 不跨 4 KiB。
文件驱动串行发射事务；两个处理器驱动自行管理多笔在途请求。
当前仅支持启动时复位；运行中取消事务、checkpoint 和跨处理器一致性未实现。
mem_sim 的 PHY/DFI 为行为模型，HBM4 预设含临时时序项，验收范围为当前模型功能与时序反馈。

源码版本/来源在 `VERSION` 和 `config/sources.json`，验收入口见 [ACCEPTANCE.md](ACCEPTANCE.md)。
