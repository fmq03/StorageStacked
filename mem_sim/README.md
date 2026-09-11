# hbm_sim

`hbm_sim` 是面向 HBM3、HBM4、LPDDR5、LPDDR6 的 C++20 内存仿真器。
项目由仿真平台和堆叠存储模型组成；堆叠存储模型明确包含可选行为级
Mem PHY 与 Mem Stack，支持控制器时序研究、真实数据读写、
物理坐标、功耗与热事件，以及可审计的命令和 DFI 事件。

## 文档

- [堆叠存储模型交付手册](堆叠存储模型交付手册.md)：项目总入口，覆盖总体设计、模块、构建、使用、验证、参数和输出指标。
- [项目指南](文档/项目指南.md)：架构、构建、运行、后端、热模型和测试入口。
- [cfg指南](文档/cfg指南.md)：详细配置项、可修改范围、参数联动与使用方法。
- [输出指南](文档/输出指南.md)：普通报告、完整诊断、产物与性能分析。
- [审计](文档/审计.md)：验证证据、来源及器件校准边界。

交付手册保留完整使用路线与必要原理；四份指南按职责提供深入说明和参数/指标查表。
目录内的 `README.md` 只说明本目录。

## 主要能力

日常运行默认打印英文精简报告（MODEL / PARAMETERS / RESULTS）。默认 `summary` 使用该报告；
机器工具读取 `--stats-json` 生成的 schema 2 结果，不解析人读报告。内部计数排障使用
`--stats-view diagnostic`。详细配置仍可通过 `--dump-resolved-config` 保存。

### 仿真平台

- 流式合成流量和 trace 输入。
- 单控制器、单 stack 多 channel，以及每 stack 一套 channel controller 的主动多-stack `MemorySystem`。
- FCFS、FR-FCFS 和可配置行策略。
- 表驱动时序、bank 状态、刷新、RFM/PRAC 和低功耗入口。
- HBM 行列双总线、边沿配对和 SID/伪通道组织。
- LPDDR 分裂激活、WCK/CAS、DVFS 和链路保护入口。

### 堆叠存储

- `MemoryImage` 管理真实数据、初始化标记、行缓冲和存储语义。
- `MemoryBackend` 提供内存稀疏表和两种文件后端。
- 地址映射覆盖 channel、pseudo-channel、SID、rank、bank group、bank、
  row、column。
- 派生 stack、die、layer、tile、grid、subarray、mat、cell、microbump
  坐标，不预分配完整物理阵列。
- 支持逐字节掩码、期望值比较、golden image 和 SECDED shadow。
- 支持命令能量、行为级稀疏三维热状态和 TSV 耦合入口。

### 审计与验证

- 命令 CSV 和离线命令验证器。
- 在线 Behavioral Mem PHY、HBM/LPDDR 协议适配、DFI-oriented command/data beat 和 signal-like CSV。
- 实际读写数据、初始化掩码、写掩码和数据来源审计。
- 请求提供 `expect=` 时，独立检查读数据。
- 存储镜像、CSV、错误报告、热图和时序表导出。

DFI 输出和行为级 PHY 是 DFI 6.0/6.0.1-oriented 研究模型，不是完整 pin-level DFI 实现或
合规认证。完整 trace 会保留命令和数据事件，只适合有限调试窗口。

## 开发与构建环境

当前 Linux 开发环境以 Clang 18 为主线，已验证的组合是 Ubuntu 26.04 LTS、
Clang/clangd/LLDB 18.1.8、CMake 4.2、Ninja 1.13、ccache 4.12 和 Python 3.14。
项目只要求 C++20，不依赖第三方 C++ 库；Clang 默认配合 Ubuntu 的
`libstdc++`，当前不额外切换到 `libc++`。

安装开发工具：

```bash
sudo apt update
sudo apt install clang-18 clangd-18 lldb-18 lld-18 \
  clang-format-18 clang-tidy-18 cmake ninja-build make ccache
```

推荐使用独立的 Clang Debug 构建目录，避免和早期 G++/Make 构建缓存混用：

```bash
cmake -S . -B build-clang-debug -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_COMPILER=/usr/bin/clang++-18 \
  -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build-clang-debug --parallel
ctest --test-dir build-clang-debug --output-on-failure
```

`compile_commands.json` 会生成在 `build-clang-debug/`，供 clangd 使用。CMake
会缓存编译器，因此切换 G++/Clang 时应使用不同构建目录，不要在旧目录上直接覆盖。

Makefile 保留为兼容入口。因为 Make 的内建 `CXX` 默认是 `g++`，使用 Clang 时要
显式传入版本化路径。Make 会记录编译器、编译/链接参数并在它们变化时重建 object；
Clang 和 G++ 仍建议使用不同目录：

```bash
# 与上面的 CMake/Clang 目录分开：Make/G++ 使用默认 build/
make CXX=g++ -j"$(nproc)"
make CXX=g++ test

# 查看或一次删除根目录下 build、build-*；不会删除 outputs
make list-builds
make clean-build

# 独立目录中的 Clang ASan/UBSan 构建与测试
make sanitize
make sanitize-test
```

`build/` 是 Make 构建目录，`build-clang-debug/` 是当前 CMake/VS Code 主构建
目录；这些目录都不是源码。

### VS Code

请直接以本目录作为 workspace 打开：

```bash
code /path/to/hbm_sim
```

项目的 `.vscode/` 已配置：

- `tasks.json`：Clang 18 + Ninja + ccache 的构建和 CTest 任务。
- `launch.json`：HBM4、LPDDR6、存储后端和 sequence tests 的 CodeLLDB 调试入口。
- `settings.json`：clangd、CMake 构建目录和 compilation database；Microsoft
  IntelliSense 被禁用，避免与 clangd 重复诊断。
- `extensions.json`：推荐 clangd、CodeLLDB 和 CMake Tools。

首次打开后运行一次默认构建任务，或执行上面的 CMake 命令，使
`build-clang-debug/compile_commands.json` 可用。若扩展没有立即刷新，执行
`Developer: Reload Window` 或 `clangd: Restart language server`。

## 快速运行

```bash
./build-clang-debug/hbm_sim --config configs/hbm.cfg --standard hbm4 --requests 1024
./build-clang-debug/hbm_sim --config configs/hbm.cfg --standard hbm3 --pattern random --requests 1024
./build-clang-debug/hbm_sim --config configs/lpddr.cfg --standard lpddr6 --trace examples/sample.trace --requests 0
./build-clang-debug/hbm_sim --config configs/hbm.cfg --standard hbm4 \
  --stack-count 6 --requests 1024
```

四类标准入口：

```text
hbm3
hbm4
lpddr5
lpddr6
```

配置目录按用途分为四层：

```text
configs/hbm.cfg、configs/lpddr.cfg          两个标准家族主配置
configs/validation/{hbm3,hbm4,lpddr5,lpddr6}.cfg  四个验证配置
examples/configs/{hbm,lpddr,hbm_nstacks,lpddr_nstacks}.cfg  四个自包含 cfg Demo
experiments/local/*.cfg                       可直接运行的自包含研究模型示例
```

日常建模直接使用两个主配置；四个 cfg Demo 展示单实例和多实例的完整副本。建立长期自定义
模型时复制对应主配置，并在副本末尾 `[override]` 写差异。验证配置只用于固定共同参数面，内部仍可通过
`[meta] extends` 继承主配置。未知算法和不存在的 preset 会直接报错。配置分层、复制修改
方法、逐项物理含义和检查命令见 [configs/README.md](configs/README.md)。

四标准多 Stack 工程 demo：

```bash
bash examples/multistack_demos/hbm3_nstack.sh
bash examples/multistack_demos/hbm4_nstack.sh
bash examples/multistack_demos/lpddr5_nstack.sh
bash examples/multistack_demos/lpddr6_nstack.sh
```

定制 bank/row-column/refresh 架构趋势实验：

```bash
python3 experiments/architecture_sweep/run.py
```

脚本使用定向 trace 激发目标结构，输出 `results.csv`、`checks.csv`、`summary.md` 和
离线 `trends.html`；检查不通过时返回非零。详细口径见
[experiments/architecture_sweep/README.md](experiments/architecture_sweep/README.md)。

## Trace

普通请求：

```text
0 R 0x1000
1 W 0x1000 data=00112233
200 R 0x1000 expect=00112233
```

多 stack trace 可增加 `stack=N`（显式局部地址）与 `qos=N`：

```text
0 W 0x1000 stack=5 qos=7 data=00112233
200 R 0x1000 stack=5 qos=7 expect=00112233
```

突发请求：

```text
0 BW 0x2000 256 pattern=inc
200 BR 0x2000 256 check=last_write
```

大量请求默认通过 `TrafficStream` 逐步生成，不先构造完整请求数组。

## 可视化

项目提供了离线、零第三方依赖的验证仪表盘：它复用既有 command/DFI trace、性能曲线和
thermal map，不改变仿真器的 C++ 核心架构。运行完整示例：

```bash
make visualize-example
```

生成的 `outputs/visualization/example/dashboard.html` 可直接在浏览器打开；其中包含命令
时间线、bank lane 过滤、命令统计、DFI 活动、性能曲线和热图。详细的手工命令见
[tools/README.md](tools/README.md#离线可视化)。

## 存储后端

```bash
./build-clang-debug/hbm_sim --config configs/hbm.cfg --standard hbm4 \
  --memory-backend sparse

./build-clang-debug/hbm_sim --config configs/hbm.cfg --standard hbm4 \
  --memory-backend mmap_sparse \
  --memory-capacity-bytes 34359738368 \
  --memory-data-file outputs/hbm_data.bin

./build-clang-debug/hbm_sim --config configs/hbm.cfg --standard hbm4 \
  --memory-backend chunk_file \
  --memory-capacity-bytes 34359738368 \
  --memory-data-file outputs/hbm_chunks.bin
```

三种后端只改变宿主机上的数据保存方式，不改变控制器、时序、行缓冲、
ECC、功耗和热模型语义。选择建议见 [存储后端](文档/项目指南.md)。

## 数据与 DFI

```bash
./build-clang-debug/hbm_sim --config configs/hbm.cfg --standard hbm4 \
  --trace examples/data_check.trace --requests 0 \
  --stats-view summary --progress-interval 10000 \
  --dump-memory-image outputs/final_memory.txt \
  --dump-memory-csv outputs/final_memory.csv \
  --mismatch-report outputs/mismatch.txt \
  --response-trace outputs/host_responses.csv \
  --transaction-response-trace outputs/transaction_responses.csv \
  --response-delivery-mode both \
  --dfi-trace outputs/dfi.csv \
  --dfi-signal-trace outputs/dfi_signal.csv \
  --validate-cmd-trace \
  --validate-dfi-trace
```

带 `expect=` 的读不匹配时，程序默认完成统计和报告后返回退出码 `3`。
故障注入实验可用 `--allow-data-mismatch` 只记录错误。

示例持久产物统一放在 `outputs/`。CLI 自动创建输出父目录；用 `tee` 保存 stdout
前需先创建目标子目录。`make clean-outputs` 可统一删除产物并保留
`outputs/README.md`，不会影响下一次仿真。

## 测试

```bash
cmake --build build-clang-debug --parallel
ctest --test-dir build-clang-debug --output-on-failure
```

CTest 包含 CLI smoke、C++ 命令序列、PHY、Timing 边界、可视化、项目内一致性检查、
CI 规模性能曲线和敏感性/不确定性检查；准确入口数以 `ctest -N` 为准。VS Code 中
可运行 `hbm_sim: run all tests`；Make 兼容入口仍是
`make CXX=/usr/bin/clang++-18 test`。

需要本地 Ramulator2.1 的外部参考检查不属于默认回归：

```bash
python3 tools/ramulator2_differential.py \
  --binary ./build-clang-debug/hbm_sim \
  --ramulator-root /path/to/ramulator2
```

Ramulator2.1 差分只比较双方刻意对齐的共同命令面，不决定本项目的存储、
DFI、功耗、热或验证语义。

## 目录

```text
include/hbm_sim/  公共接口
src/              分层实现
configs/          运行、配置、校准和验证
examples/         示例脚本、trace 和存储镜像
tests/            smoke 与命令序列测试
tools/            验证和统计工具
文档/             项目、cfg、输出、审计四份指南
```

源码职责和请求路径见 [src/README.md](src/README.md)，公共接口见
[include/README.md](include/README.md)。

## 模型边界

- JEDEC 可见规则、项目推导值、研究默认值和厂商数据分别标注。
- 当前通过测试只能说明软件可运行、内部一致和共同面的参考一致，不能替代
  真实器件测量。
- RL/WL、部分行时序、RFM/RAS/ECC、训练、功耗、热参数和物理几何仍需
  目标厂商资料校准。
- HBM4 中 `line_size=64` 是 frontend/host 请求粒度；`dram_transaction_bytes=32`
  是单 PC 的 RD/WR payload 和存储后端块粒度。一个默认 host 请求拆成两个事务。
- LPDDR6 x24 由两个 x12 subchannel 构成；当前 16Gb/subchannel preset 使用
  65536 rows、64 columns、BL24 和 32B transaction，两个 subchannel 合计
  4GiB。64B host line 同样拆成两个 32B 事务。
- HBM4 默认每个 32B 事务计 16 bit 外部 ECC metadata；片上 ECC check bits
  属于器件实现数据，未当作 64 bit 外部 sideband。
- 显式短 `data=`/`expect=` 仍用于 byte-level 数据正确性调试；它不表示完整
  BL8 引脚占用。物理带宽实验应使用 32B 整数倍的请求。
- `system_cycles` 是并行多控制器的系统时间；`aggregate_ctrl_cycles` 是控制器
  周期之和，不能用于计算外部带宽。
