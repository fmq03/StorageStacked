# result 文件导读

[返回文档目录](README.md)。本篇介绍 CoralNPU 的 `user/<项目>/result/`，采用整理后的分类格式。
同工作区 Vortex 的结果格式仍以其自身文档为准。

## 1. 先找到一次运行

`smoke`、`llm`、`burst` 各自保存结果，不共用一个结果目录。
先打开所属项目的 `result/README.md`，表中列出场景、验收状态、L0 开关及 ITCM/DTCM 容量。

- [SMOKE 结果索引](../user/smoke/result/README.md)
- [TinyLLM 结果索引](../user/llm/result/README.md)
- [AXI burst 结果索引](../user/burst/result/README.md)

单次运行可以直接放在 `result/<运行编号>/`；批量验收通常使用
`result/<批次>/<场景>/`。例如 [L0 开启的 SMOKE](../user/smoke/result/l0-20261021/enabled/report.md)。
矩阵名称中的 `i8-d32-l01` 表示 ITCM 8 KiB、DTCM 32 KiB、L0 开启；`l00` 表示关闭。
目录名只用于查找，实际参数应核对配置和设备返回值。

## 2. 每个场景的目录结构

```text
<一次运行>/
├── report.md                  人读报告
├── summary.json               最终状态和检查汇总
├── config/                    本次配置、实际配置和环境记录
├── logs/                      编译、仿真、校验日志
├── build/                     可执行程序与模型构建参数
├── traces/                    原始请求、波形和内存记录
├── checks/                    计算、协议、链路、内存和波形检查结果
└── views/                     可视化网页及配套资源
```

不同负载和失败阶段产生的文件数量不同。例如编译失败时可能还没有 `traces/` 或 `views/`。
运行中间阶段可能暂时出现尚未归类的文件；运行器退出时自动整理。
旧结果也已按此结构分类，历史失败记录保留原状态。

## 3. 推荐阅读顺序

1. 看 `report.md`：确认输入、输出、实际模型参数和验收结论。
2. 看 `summary.json`：只有 `passed: true` 才表示整次验收通过。
3. 看 `config/input.json` 和 `config/npu_model.json`：核对本次用户输入与实际加载模型。
4. 看 `checks/smoke_summary.json` 或 `checks/llm_summary.json`：查看独立数值校验。
5. 需要分析访存时，再打开 `views/` 网页并对照 `traces/` 原始记录。

`checks/completion.json` 中的完成标志和 mailbox 说明设备执行结束；最终验收还要求
计算结果、AXI 五通道、VCD、Flit、在线内存返回数据全部通过。
只有编译成功、设备退出或网页存在，都不足以表示整次运行通过。

## 4. config：这次到底用了什么参数

| 文件 | 内容及用途 |
|---|---|
| `input.json` | 本次运行固定下来的用户配置；以后修改项目的 config.json 不会改变此快照 |
| `resolved.json` | 转换后的架构、负载和 ELF 配置；其中 kernel 路径相对于本次运行目录 |
| `npu_model.json` | 实际设备库返回的 L0、ITCM、DTCM、RVV/浮点和地址参数 |
| `config.json` | 仿真入口实际使用的设备、AXI、时钟等设置 |
| `ucie_config.json` | 实际 UCIe 参数 |
| `memsim_config.json` | 实际内存标准、通道、队列和时序设置 |
| `environment.json` | 存储依赖版本、程序和库的位置及文件大小 |

例如，核对 L0 是否真正打开，应同时检查 `input.json` 中的
`npu.fetch_l0_enabled` 和 `npu_model.json` 中的 `fetch_l0_enabled`。
运行入口会拒绝配置与已加载 RTL 模型不一致的情况。

## 5. logs 与 build：构建或执行失败时看这里

| 文件 | 用途 |
|---|---|
| `logs/build.log` | 程序编译、RTL/设备库构建和运行器构建日志 |
| `logs/run.log` | 设备执行、异常、超时和结束信息 |
| `logs/validation.log` | 校验入口的输出 |
| `logs/verification.log` | 各底层检查器的详细输出 |
| `build/program.elf` | 实际执行的用户程序；TinyLLM 的独立参考模型快照随 ELF 保存 |
| `build/program.dis` | TinyLLM 反汇编，供检查实际 RVV 指令 |
| `build/model_parameters.h` | 本次 RTL 的生成参数头 |
| `build/rtl.json` | 所选 RTL 变体、地址布局、构建目标和产物记录 |
| `build/simulator.path` | 运行器的相对位置，由脚本维护 |

先看最早出现的真实错误。比如 TCM 太小导致 ELF 链接失败，应从 build.log 处理，
不能跳过编译后用旧程序的结果作为本次成功证据。
`build/simulator.path` 和 environment.json 引用内部缓存；移动整个结果目录后，
报告及原始数据仍可阅读，但重新执行程序需要恢复或重建对应运行环境。

## 6. traces、checks 与 views：从结论追到证据

| 检查对象 | 原始记录（traces/） | 检查结果（checks/） |
|---|---|---|
| 设备请求与返回 | `npu_requests.csv`、`transactions.csv` | `smoke_summary.json`、`llm_summary.json`，以及顶层 summary.json |
| AXI 协议与实际波形 | `axi_events.csv`、`axi_wave.vcd` | `check_summary.json`、`protocol_summary.json`、`wave_audit/summary.json` |
| AXI 到 Flit 及链路 | `aou_events.csv`、`ucie_soc.csv`、`ucie_mem.csv`、`ucie_flits.csv`、`flit_pairs.csv` | `aou_check_summary.json`、`link_check_summary.json` |
| 在线内存 | `memsim_bridge.csv`、`memsim_commands.csv`、`memsim_dfi_signals.csv`、`memsim_image.csv` | `memsim_check.json`、`memsim_core.json`、`memsim_bridge_summary.json` |

SMOKE 的计算结果为加一后的输出字；默认输入 41 时输出 42。
TinyLLM 的 `checks/llm_summary.json` 包含生成文本、token、数值误差、KV 检查及阶段时间；
默认负载生成 `blu`。这些是功能验收负载，不代表完整大模型精度评估。
burst 使用自定义输出字，需看顶层 summary.json 的 benchmark/检查结果。

`views/trace_view.html` 查看 AXI/UCIe 路径，`views/memsim_view.html` 查看内存过程。
`views/axi_view.gtkw` 是 GTKWave 信号列表，引用 `traces/axi_wave.vcd`。
HTML 同目录下的 JS、SVG 和 `*_data/` 是网页资源；分享网页时应保留整个运行目录结构。
网页链接已随分类更新，不必把 CSV/VCD 复制回顶层。

## 7. 直接查看现有示例

在本项目的 `user/` 下执行：

```bash
cat smoke/result/l0-20261021/enabled/report.md
cat smoke/result/l0-20261021/enabled/summary.json
cat smoke/result/l0-20261021/enabled/config/input.json
cat smoke/result/l0-20261021/enabled/config/npu_model.json
```

创建一次新运行仍使用原入口，输出目录必须尚不存在：

```bash
./run.sh smoke --output result/my-smoke-01
./run.sh llm --output result/my-llm-01
```

修改业务参数请编辑 `user/<项目>/config.json`，然后创建新运行。
不要修改旧结果中的配置或原始数据来制造参数对比。

## 8. 结果整理是怎样接入运行器的

仿真和共享存储检查器沿用原来的文件接口；校验及退出阶段由
[result_layout.py](../coralnpu/runtime/result_layout.py) 统一分类和修正展示链接，
[session.py](../coralnpu/runtime/session.py) 更新项目的结果索引。
对分类后的结果重新校验时，检查入口会临时恢复原接口，结束后再次整理。

现有 benchmark 的结果使用上述分类结构，报告和网页资源链接均需保持有效。
整理后的 SMOKE 结果重新校验通过，
默认完整回归的 SMOKE=42、TinyLLM=blu、19 项原生测试及五种 burst 场景均通过。
相关方法、整理核查和完整回归结果统一见 [验证说明](08-验证说明.md)。
这些是对应批次的记录；后续运行仍需检查各自的 summary.json。
