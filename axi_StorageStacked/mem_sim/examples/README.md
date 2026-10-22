# 示例

本目录保存可直接运行的示例脚本和示例 trace，用来展示常见 HBM/LPDDR 仿真方式。

当前示例重点覆盖：普通 trace、timed trace、真实 payload trace、初始 memory
image、final memory txt/CSV 导出、DFI beat/signal trace 以及 storage model 参数。
`configs/` 中的两个主配置是权威模板；`examples/configs/` 中四份 cfg 是由主配置复制得到的
自包含 Demo。不要把长 trace 或后端大文件放在 examples 中。

主要文件：

- `run_hbm4_stream.sh`：运行 HBM4 stream workload。
- `run_hbm3_random.sh`：运行 HBM3 random workload。
- `run_lpddr6_trace.sh`：运行 LPDDR6 trace workload。
- `run_lpddr5_trace.sh`：运行 LPDDR5 trace workload。
- `configs/hbm.cfg`：HBM 单 Stack 完整 cfg Demo。
- `configs/lpddr.cfg`：LPDDR 单器件完整 cfg Demo。
- `configs/hbm_nstacks.cfg`：HBM 多 Stack 完整 cfg Demo。
- `configs/lpddr_nstacks.cfg`：LPDDR 多器件完整 cfg Demo。
- `sample.trace`：简单 trace 输入样例，格式为 `R|W ADDRESS`。
- `timed.trace`：带显式注入 cycle 的 trace 输入样例，格式为 `CYCLE R|W ADDRESS`。
- `memory_image.txt`：稀疏真实存储区初始内容样例。
- `data_check.trace`：带 `data=`、`expect=` 和 `mask=` 的数据正确性 trace 样例。
- `qos_priority.trace`：同一源注入时刻、同 Stack 的多优先级 trace，用于对照
  `fcfs` 与 `strict_priority` 的完成顺序。
- `multistack_demos/`：四种标准的多 Stack 工程脚本、共享数据闭环 trace 和使用说明。

修改建议：

- 新增重要配置或协议行为时，建议同步补一个最小示例。
- 示例脚本应保持短小，只演示一种重点能力；复杂回归应放在 `tests/`。
- 示例输出字段来自 `src/stats/`，不要在脚本里重新解释统计口径。

## 运行方式

当前 Clang/CMake 环境先从项目根目录构建：

```bash
cmake -S . -B build-clang-debug -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_COMPILER=/usr/bin/clang++-18 \
  -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build-clang-debug --parallel
```

示例脚本是 Makefile 兼容入口，内部固定调用 `./build/hbm_sim`。直接运行这些
脚本前应先执行：

```bash
make clean
make CXX=/usr/bin/clang++-18 -j"$(nproc)"
bash examples/run_hbm4_stream.sh
bash examples/run_hbm3_random.sh
bash examples/run_lpddr6_trace.sh
bash examples/run_lpddr5_trace.sh
```

使用 VS Code/CMake 主构建目录时，不需要复制构建产物，直接把脚本中的命令参数交给
`./build-clang-debug/hbm_sim`；下面的人工检查命令均采用这种方式。

## 示例 trace 格式

`sample.trace` 是最小访存 trace：

```text
R 0x0
W 0x40
```

`timed.trace` 带显式注入周期：

```text
0 R 0x0
10 W 0x40
```

解析规则：

- `R` 表示读，`W` 表示写。
- 地址可以是十进制或 `0x` 十六进制。
- 空行和 `#` 注释会被忽略。
- `requests = 0` 或 `--requests 0` 在 trace 模式下表示读完整个 trace。

## 示例和测试的区别

示例用于展示“怎么运行”，测试用于保证“行为没坏”。因此：

- 示例脚本可以短，不要求覆盖所有边界。
- 如果需要断言输出字段，请放到 `tests/smoke.sh`。
- 如果需要检查命令序列和状态机，请放到 `tests/sequence_tests.cpp`。

## data_check.trace

`data_check.trace` 是最小真实存储区和数据正确性示例：

```text
W 0x1000 data=00112233445566778899aabbccddeeff
R 0x1000 expect=00112233445566778899aabbccddeeff
W 0x1008 data=deadbeef
R 0x1000 expect=0011223344556677deadbeefccddeeff
W 0x1004 data=a1a2a3a4a5a6a7a8 mask=ff00ff00ff00ff00
R 0x1000 expect=00112233a155a377a5ada7efccddeeff
R 0x1004 expect=a155a377a5ada7ef
```

trace 数据扩展：

- `data=HEX`：写请求携带的真实 payload。
- `expect=HEX`：读完成时 expected/actual 比较目标；不匹配会增加
  `data_mismatches` 并默认返回 3。故意收集错误时可用 `--allow-data-mismatch`。
- `mask=HEX`：byte mask；非零 mask byte 才更新对应 payload byte。

同一条路径还会报告物理存储覆盖范围。常用字段包括
`storage_channels_touched`、`storage_banks_touched`、`storage_rows_touched`、
`storage_layers_touched`、`storage_subarrays_touched`、`storage_mats_touched`、
`storage_cells_touched`、`storage_microbumps_touched`、`storage_read_line_accesses`
和 `storage_write_line_accesses`。

Row buffer 建模由命令驱动：ACT 打开 row buffer，RD/WR 访问它，PRE 或 auto-precharge
关闭它，dirty row 会写回稀疏 `MemoryImage`。常用字段包括 `rowbuf_activations`、
`rowbuf_reads`、`rowbuf_writes`、`rowbuf_dirty_writebacks`、`rowbuf_hits` 和
`rowbuf_misses`。

同一批命令事件也会进入 floorplan/power/thermal 模型。常用字段包括
`floorplan_tiles_touched`、`power_energy_pJ`、`thermal_peak_temp_C`、
`thermal_hotspot_layer`、`thermal_hotspot_x`、`thermal_hotspot_y`、
`thermal_lateral_transfers`、`thermal_vertical_transfers`、`thermal_tsv_transfers`、
`dfi_read_beats` 和 `dfi_write_beats`。

人工检查建议：

```bash
./build-clang-debug/hbm_sim --standard hbm4 \
  --memory-image examples/memory_image.txt \
  --trace examples/data_check.trace --requests 0 \
  --dump-memory-image outputs/final_memory.txt \
  --dump-memory-csv outputs/final_memory.csv \
  --mismatch-report outputs/mismatch_report.txt
```

`outputs/mismatch_report.txt` 只有表头表示读写校验通过。`outputs/final_memory.txt` 中的
`data=`、`init=`、`version=`、`last_writer=`、`ch/bank/row/col` 和
`subarray/mat/cell/microbump` 字段可以直接人工检查，也可以按空格分隔导入 Excel。
`outputs/final_memory.csv` 是更适合 Excel/WPS 的表格版本，首列是 address，后面是
data、init mask、version、last writer、写入周期、checksum、ECC shadow、bank/row/column、
layer/tile、thermal grid、subarray/mat/cell/microbump。

也可以把最终存储区保存成稀疏二进制 checkpoint，用于后续 golden 验证：

```bash
./build-clang-debug/hbm_sim --standard hbm4 \
  --trace examples/data_check.trace --requests 0 \
  --dump-memory-image outputs/golden_memory.bin

./build-clang-debug/hbm_sim --standard hbm4 \
  --memory-image outputs/golden_memory.bin \
  --requests 0 \
  --verify-golden outputs/golden_memory.bin
```

`--verify-golden` 会同时比较 payload 和逐 byte `init` mask，因此未写入区域即使默认值和期望值相同，也不会被误判为正确写入。

## memory_image.txt

`memory_image.txt` can seed the sparse runtime `MemoryImage` before simulation:

```text
0x1000 ch=0 pc=0 sid=0 rank=0 bg=0 bank=0 row=8 col=0 data=0011223344556677
```

Run with text checkpoint/report files:

```bash
./build-clang-debug/hbm_sim --standard hbm4 --memory-image examples/memory_image.txt \
  --trace examples/data_check.trace --requests 0 \
  --dump-memory-image outputs/final_memory.txt \
  --dump-memory-csv outputs/final_memory.csv \
  --mismatch-report outputs/mismatch_report.txt \
  --dump-thermal-map outputs/thermal_map.txt
```

仿真过程中 RD/WR 仍然只操作内存中的稀疏镜像，不会每次访问都读写 txt 文件。txt 文件只作为初始 checkpoint、最终 dump 和错误报告。`thermal_map.txt` 会列出被触达的 floorplan tile 或 tile 内 thermal grid、累计能量、温度和物理坐标。
这类设计与常见闪存后端一样，都要求真实数据存储和可验证读回；区别是
HBM/LPDDR 容量更大，默认使用稀疏 `MemoryImage`，避免为未访问区域分配
数十 GB 空间。
如果输出文件名是 `.bin`，`--dump-memory-image` 会写入稀疏二进制 checkpoint；加载时 `--memory-image` 会通过文件头 `HBMS` 自动识别二进制格式，否则按文本 memory image 解析。

## storage model parameters

For sensitivity studies, the first-version floorplan/power/thermal model can be configured without recompiling:

```bash
./build-clang-debug/hbm_sim --standard hbm4 --requests 128 \
  --power-scale 1.25 \
  --thermal-ambient-c 37 \
  --thermal-cooling-per-cycle 0.0001 \
  --thermal-rise-c-per-pj 0.00003
```

The same keys can be placed in the `[override]` section of a copied HBM or LPDDR
configuration. The default values are research defaults; replace them with
device/vendor data before making numeric power or thermal claims.

For a DRAMsim3-style power calibration path, use:

```bash
./build-clang-debug/hbm_sim --config configs/hbm.cfg --standard hbm4 \
  --power-source dramsim3_idd \
  --thermal-grid-cols-per-tile 4 --thermal-grid-rows-per-tile 4
```

That example derives ACT/RD/WR/REF command energy from `VDD/IDD/timing` and
splits each bank tile into thermal grids. Its IDD values are a traceable
DRAMsim3 HBM2 template, not an HBM4 vendor table.

## DFI beat/signal trace example

DFI5.0 is currently modeled with two exported views derived from issued RD/WR
commands: a command/data beat trace and a DFI-like signal trace. This is enough
to check phase, latency, payload beat accounting, command select, address, data
valid windows and real data lanes before adding a complete pin-level DFI
implementation:

```bash
./build-clang-debug/hbm_sim --standard hbm4 --requests 16 \
  --dfi-trace outputs/hbm4_dfi.csv \
  --dfi-signal-trace outputs/hbm4_dfi_signal.csv \
  --validate-dfi-trace \
  --dfi-phase-count 2 \
  --dfi-data-lane-bytes 16
```

The beat CSV contains command events plus `READ_DATA`/`WRITE_DATA` beat events.
The signal CSV adds `dfi_reset_n`, `dfi_cs_n`, `dfi_cke`, `dfi_odt`,
`dfi_address`, `dfi_bank`, `dfi_rddata_en`, `dfi_wrdata_en`,
`dfi_rddata_valid`, `dfi_wrdata_mask`, `dfi_wrdata` and `dfi_rddata`. Online
simulation exports real write payloads as `payload_source=request_payload` and
real readback payloads as `payload_source=memory_image`; `synthetic_fallback`
is only used for offline commands without a payload snapshot. Use
`dfi_read_latency_nck` and `dfi_write_latency_nck` in a config file or CLI
override when matching a specific MC/PHY timing point. Vendor-specific PHY
lane wiring, training/update handshakes and exact CA pin packing remain
project-defined until real vendor data is supplied.

`--validate-dfi-trace` additionally checks command/data event pairing through
`issued_cycle`, beat order and width, phase, latency, signal windows, payload
source and masks. Reads carrying `expect=` are checked against that independent
expected payload. A validation failure returns a non-zero exit code. Because
the trace path retains commands, payloads and events, use it for representative
debug windows rather than default million-request throughput runs.
