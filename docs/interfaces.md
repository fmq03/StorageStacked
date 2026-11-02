# 接口与结果契约

AXI 信号、窗口、反压与时间单位见 [AXI 接口](../axi_StorageStacked/INTERFACE.md)。共享帧格式见 [aou_format6.h](../common/interfaces/aou_format6.h)。

## 路径清单

[storage_manifest.json](../axi_StorageStacked/storage_manifest.json) 的路径相对其所在 AXI 目录。CMake、SCons、两个处理器 runtime 和独立链路 Makefile 都读取这份清单。Python 读取器会检查条目存在；移动组件只需更新清单和组件自己的相对公共头路径。

SystemC 参数见 [开发说明](development.md#选择-systemc)。Vortex 编译公共源码时使用 gem5 内核；CoralNPU 与 AXI 的现有 target / 安装前缀二选一。编译器 ABI、C++ 标准、宏和 1 fs 时间分辨率必须匹配。

## trace 与结果目录契约

`common/verif/result_core.py` 的 `Layout` 实例只提供目录机制；AXI 的
`config/result_layout.json` 和各平台 runtime 的 `result_routes.json` 提供路由数据。
公共 AXI 路由表通过 manifest 的 `result_routes` 字段定位，所有 manifest 路径相对 manifest 所在目录解析。
`Layout.merged()` 合并路由时，同名文件的目标位置不同会立即报错；不允许后加载的表静默覆盖。
每份布局持有自己的路由，不修改模块全局状态。Vortex 的 `config.ini` 保持实际生效的 `config/config.ini`。


模拟器保持平铺写入；共同整理逻辑只移动原始文件，并修正 Markdown、HTML、GTKWave 的展示链接。原始 CSV、JSON、VCD 内容保留。summary.json 与 report.md 留在顶层，其余分入 config、logs、build、checks、views；原始 trace 按 AXI、Flit、UCIe、内存或处理器分层。

层与文件的路由由 [AXI 配置](../axi_StorageStacked/config/result_layout.json) 及处理器 wrapper 提供，common/verif 只负责目录机制。artifact() 兼容平铺、旧分组、新分层布局；working_result() 在重验后恢复分组，并保留失败结论。

以下由原生基线盘点得到。大小是 AXI default 与两个 TinyLLM 样本中的最大单文件字节数；分块视图用 *.js 合并展示。动态派生文件按负责导出的入口列出；这不是未执行场景的大小承诺。每个文件的生产者和消费者以代码为准，修改 trace 文件名或格式时须同步更新对应检查器与本节。

| 文件 / 类型 | 入口 | 层 | 生产者位置 | 检查器 | 最大 bytes | 目标位置 |
|---|---|---|---|---|---:|---|
| `axi256_handshakes.csv` | axi,coralnpu,vortex | axi | [axi_StorageStacked/checks/inspect_link.py:98](../axi_StorageStacked/checks/inspect_link.py#L98) | inspect_link | 2705920 | `traces/axi/axi256_handshakes.csv` |
| `axi_events.csv` | axi,coralnpu,vortex | axi | [axi_StorageStacked/scripts/run.py:121](../axi_StorageStacked/scripts/run.py#L121) | audit_wave, check_aou, report, run, smoke | 2459470 | `traces/axi/axi_events.csv` |
| `axi_first_200ns_cycles.csv` | axi,coralnpu,vortex | axi | [axi_StorageStacked/checks/inspect_link.py:99](../axi_StorageStacked/checks/inspect_link.py#L99) | inspect_link | 28231 | `traces/axi/axi_first_200ns_cycles.csv` |
| `axi_overview.svg` | coralnpu,vortex | axi | [axi_StorageStacked/checks/trace_view.py:43](../axi_StorageStacked/checks/trace_view.py#L43) | trace_view | 6120 | `views/axi_overview.svg` |
| `axi_view.gtkw` | coralnpu,vortex | axi | [axi_StorageStacked/checks/trace_view.py:16](../axi_StorageStacked/checks/trace_view.py#L16) | trace_view | 702 | `views/axi_view.gtkw` |
| `axi_wave.vcd` | axi,coralnpu,vortex | axi | [axi_StorageStacked/examples/axi_storage.cc:147](../axi_StorageStacked/examples/axi_storage.cc#L147) | check_aou_negative, check_link_negative, inspect_link | 150526279 | `traces/axi/axi_wave.vcd` |
| `config.json` | axi,coralnpu,vortex | axi | [coralnpu_StorageStacked/integration/main.cc:95](../coralnpu_StorageStacked/integration/main.cc#L95) | check, check_memsim, check_memsim_negative, run, verify | 117744 | `config/config.json` |
| `input.json` | axi,coralnpu,vortex | axi | [axi_StorageStacked/scripts/run.py:137](../axi_StorageStacked/scripts/run.py#L137) | check_negative, run, trace_policy | 1864 | `config/input.json` |
| `link_check_summary.json` | axi,coralnpu,vortex | axi | [axi_StorageStacked/checks/inspect_link.py:181](../axi_StorageStacked/checks/inspect_link.py#L181) | inspect_link, trace_view | 311 | `checks/link_check_summary.json` |
| `protocol_summary.json` | axi,coralnpu,vortex | axi | [axi_StorageStacked/storage_axi/axi_monitor.hh:24](../axi_StorageStacked/storage_axi/axi_monitor.hh#L24) | audit_wave, check, check_aou, verify | 409 | `checks/protocol_summary.json` |
| `responses.json` | axi | axi | [axi_StorageStacked/scripts/test.py:77](../axi_StorageStacked/scripts/test.py#L77) | run, smoke, test | 1080 | `traces/axi/responses.json` |
| `run.log` | axi,coralnpu,vortex | axi | [coralnpu_StorageStacked/integration/main.cc:127](../coralnpu_StorageStacked/integration/main.cc#L127) | run, verify, verify_compute, verify_host | 26774 | `logs/run.log` |
| `summary.json` | axi,coralnpu,vortex | axi | [axi_StorageStacked/scripts/run.py:134](../axi_StorageStacked/scripts/run.py#L134) | run, test | 123617 | `summary.json` |
| `wave_audit/first_48ns_edges.csv` | axi | axi | [axi_StorageStacked/checks/audit_wave.py:168](../axi_StorageStacked/checks/audit_wave.py#L168) | audit_wave | 2698 | `checks/wave_audit/first_48ns_edges.csv` |
| `wave_audit/handshakes_from_vcd.csv` | axi | axi | [axi_StorageStacked/checks/audit_wave.py:168](../axi_StorageStacked/checks/audit_wave.py#L168) | audit_wave | 1766 | `checks/wave_audit/handshakes_from_vcd.csv` |
| `wave_audit/summary.json` | axi,coralnpu,vortex | axi | [axi_StorageStacked/scripts/run.py:134](../axi_StorageStacked/scripts/run.py#L134) | run, test | 1009 | `checks/wave_audit/summary.json` |
| `build/model_parameters.h` | coralnpu | coralnpu | [coralnpu_StorageStacked/coralnpu/runtime/session.py:54](../coralnpu_StorageStacked/coralnpu/runtime/session.py#L54) | 构建 / 报告 / 视图 | 1540 | `build/model_parameters.h` |
| `build/rtl.json` | coralnpu | coralnpu | [coralnpu_StorageStacked/coralnpu/runtime/session.py:55](../coralnpu_StorageStacked/coralnpu/runtime/session.py#L55) | 构建 / 报告 / 视图 | 523 | `build/rtl.json` |
| `npu_model.json` | coralnpu | coralnpu | [coralnpu_StorageStacked/integration/main.cc:83](../coralnpu_StorageStacked/integration/main.cc#L83) | check_negative, report, verify | 144 | `config/npu_model.json` |
| `npu_requests.csv` | coralnpu | coralnpu | [coralnpu_StorageStacked/coralnpu/runtime/check_smoke_negative.py:16](../coralnpu_StorageStacked/coralnpu/runtime/check_smoke_negative.py#L16) | check_negative, check_smoke_negative, report, verify, verify_llm | 651313 | `traces/coralnpu/npu_requests.csv` |
| `transactions.csv` | coralnpu | coralnpu | [axi_StorageStacked/checks/memsim_view.py:16](../axi_StorageStacked/checks/memsim_view.py#L16) | check, check_negative, memsim_view, verify | 350281 | `traces/coralnpu/transactions.csv` |
| `aou_check_summary.json` | axi,coralnpu,vortex | flit | [axi_StorageStacked/checks/check_aou.py:98](../axi_StorageStacked/checks/check_aou.py#L98) | check_aou, check_negative, smoke | 434 | `checks/aou_check_summary.json` |
| `aou_events.csv` | axi,coralnpu,vortex | flit | [axi_StorageStacked/storage_axi/aou_backend.cc:58](../axi_StorageStacked/storage_axi/aou_backend.cc#L58) | check_aou, check_aou_negative, check_memsim, check_memsim_negative, inspect_link | 6201033 | `traces/flit/aou_events.csv` |
| `aou_messages.csv` | axi,coralnpu,vortex | flit | [axi_StorageStacked/checks/inspect_link.py:171](../axi_StorageStacked/checks/inspect_link.py#L171) | inspect_link | 16462164 | `traces/flit/aou_messages.csv` |
| `aou_summary.json` | axi,coralnpu,vortex | flit | [axi_StorageStacked/storage_axi/aou_backend.cc:269](../axi_StorageStacked/storage_axi/aou_backend.cc#L269) | check_aou, check_aou_negative, inspect_link | 272 | `checks/aou_summary.json` |
| `axi_flit_path.csv` | axi,coralnpu,vortex | flit | [axi_StorageStacked/checks/inspect_link.py:237](../axi_StorageStacked/checks/inspect_link.py#L237) | check_memsim, check_memsim_negative, inspect_link, trace_view | 5161044 | `traces/flit/axi_flit_path.csv` |
| `flit_pairs.csv` | axi,coralnpu,vortex | flit | [axi_StorageStacked/checks/inspect_link.py:170](../axi_StorageStacked/checks/inspect_link.py#L170) | inspect_link, trace_view | 9901703 | `traces/flit/flit_pairs.csv` |
| `memsim_bridge.csv` | axi,coralnpu,vortex | memsim | [axi_StorageStacked/storage_axi/memsim_backend.cc:10](../axi_StorageStacked/storage_axi/memsim_backend.cc#L10) | check_memsim, check_memsim_negative | 6425861 | `traces/memsim/memsim_bridge.csv` |
| `memsim_bridge_summary.json` | axi,coralnpu,vortex | memsim | [axi_StorageStacked/storage_axi/memsim_backend.cc:110](../axi_StorageStacked/storage_axi/memsim_backend.cc#L110) | check_memsim, check_memsim_negative | 121 | `checks/memsim_bridge_summary.json` |
| `memsim_check.json` | axi,coralnpu,vortex | memsim | [axi_StorageStacked/checks/check_memsim.py:177](../axi_StorageStacked/checks/check_memsim.py#L177) | check_memsim, check_negative, memsim_view, smoke | 341 | `checks/memsim_check.json` |
| `memsim_commands.csv` | axi,coralnpu,vortex | memsim | [components/mem_sim/integration/online.cpp:112](../components/mem_sim/integration/online.cpp#L112) | check_memsim, check_memsim_negative, memsim_view | 30454490 | `traces/memsim/memsim_commands.csv` |
| `memsim_config.json` | axi,coralnpu,vortex | memsim | [components/mem_sim/integration/online.cpp:54](../components/mem_sim/integration/online.cpp#L54) | check_memsim, check_memsim_negative, memsim_view, test, verify | 328 | `config/memsim_config.json` |
| `memsim_core.json` | axi,coralnpu,vortex | memsim | [components/mem_sim/integration/online.cpp:120](../components/mem_sim/integration/online.cpp#L120) | check_memsim, check_memsim_negative, check_online, run, smoke | 162 | `checks/memsim_core.json` |
| `memsim_dfi.csv` | axi,coralnpu,vortex | memsim | [components/mem_sim/integration/online.cpp:113](../components/mem_sim/integration/online.cpp#L113) | 构建 / 报告 / 视图 | 53263808 | `traces/memsim/memsim_dfi.csv` |
| `memsim_dfi_signals.csv` | axi,coralnpu,vortex | memsim | [components/mem_sim/integration/online.cpp:114](../components/mem_sim/integration/online.cpp#L114) | check_memsim, check_memsim_negative, memsim_view | 73001865 | `traces/memsim/memsim_dfi_signals.csv` |
| `memsim_image.csv` | axi,coralnpu,vortex | memsim | [components/mem_sim/integration/online.cpp:119](../components/mem_sim/integration/online.cpp#L119) | check_memsim, check_memsim_negative, check_online, memsim_view | 936089 | `traces/memsim/memsim_image.csv` |
| `memsim_journeys.json` | axi,coralnpu,vortex | memsim | [axi_StorageStacked/checks/check_memsim.py:178](../axi_StorageStacked/checks/check_memsim.py#L178) | check_memsim, memsim_view, verify_smoke | 52312019 | `checks/memsim_journeys.json` |
| `memsim_stats.txt` | axi,coralnpu,vortex | memsim | [components/mem_sim/integration/online.cpp:115](../components/mem_sim/integration/online.cpp#L115) | 构建 / 报告 / 视图 | 8096 | `traces/memsim/memsim_stats.txt` |
| `memsim_view.html` | coralnpu,vortex | memsim | [axi_StorageStacked/checks/memsim_view.py:79](../axi_StorageStacked/checks/memsim_view.py#L79) | memsim_view, report | 446581 | `views/memsim_view.html` |
| `ucie_config.json` | coralnpu,vortex | ucie | [vortex_StorageStacked/integration/storage.cc:122](../vortex_StorageStacked/integration/storage.cc#L122) | check_negative, verify | 60 | `config/ucie_config.json` |
| `ucie_flits.csv` | axi,coralnpu,vortex | ucie | [axi_StorageStacked/storage_axi/aou_backend.cc:61](../axi_StorageStacked/storage_axi/aou_backend.cc#L61) | check_link_negative, inspect_link, report, trace_view | 249844254 | `traces/ucie/ucie_flits.csv` |
| `ucie_mem.csv` | axi,coralnpu,vortex | ucie | [axi_StorageStacked/storage_axi/aou_backend.cc:63](../axi_StorageStacked/storage_axi/aou_backend.cc#L63) | check_link_negative, memsim_view, trace_view | 125001699 | `traces/ucie/ucie_mem.csv` |
| `ucie_soc.csv` | axi,coralnpu,vortex | ucie | [axi_StorageStacked/storage_axi/aou_backend.cc:62](../axi_StorageStacked/storage_axi/aou_backend.cc#L62) | check_link_negative, memsim_view, trace_view | 124842658 | `traces/ucie/ucie_soc.csv` |
| `build.log` | coralnpu,vortex | vortex | [coralnpu_StorageStacked/coralnpu/runtime/test_rtl.py:87](../coralnpu_StorageStacked/coralnpu/runtime/test_rtl.py#L87) | 构建 / 报告 / 视图 | 52055 | `logs/build.log` |
| `build/host.elf` | vortex | vortex | [vortex_StorageStacked/third_party/gem5/runtime/compile.py:57](../vortex_StorageStacked/third_party/gem5/runtime/compile.py#L57) | report, verify_host | 45416 | `build/host.elf` |
| `build/program.dis` | coralnpu,vortex | vortex | [vortex_StorageStacked/third_party/gem5/runtime/compile.py:48](../vortex_StorageStacked/third_party/gem5/runtime/compile.py#L48) | 构建 / 报告 / 视图 | 105444 | `build/program.dis` |
| `build/program.elf` | coralnpu,vortex | vortex | [vortex_StorageStacked/third_party/gem5/runtime/compile.py:57](../vortex_StorageStacked/third_party/gem5/runtime/compile.py#L57) | report, verify_host | 155204 | `build/program.elf` |
| `build/program.vxbin` | vortex | vortex | [vortex_StorageStacked/third_party/gem5/runtime/compile.py:57](../vortex_StorageStacked/third_party/gem5/runtime/compile.py#L57) | report | 12592 | `build/program.vxbin` |
| `build/programs.json` | vortex | vortex | [vortex_StorageStacked/third_party/gem5/runtime/compile.py:57](../vortex_StorageStacked/third_party/gem5/runtime/compile.py#L57) | report, verify_host | 348 | `build/programs.json` |
| `build/readback.json` | vortex | vortex | [vortex_StorageStacked/third_party/gem5/runtime/compile.py:58](../vortex_StorageStacked/third_party/gem5/runtime/compile.py#L58) | verify_compute | 392 | `build/readback.json` |
| `build/simulator.path` | coralnpu,vortex | vortex | [vortex_StorageStacked/third_party/gem5/runtime/session.py:61](../vortex_StorageStacked/third_party/gem5/runtime/session.py#L61) | 构建 / 报告 / 视图 | 77 | `build/simulator.path` |
| `build/sources/Makefile` | vortex | vortex | [vortex_StorageStacked/third_party/gem5/runtime/compile.py:50](../vortex_StorageStacked/third_party/gem5/runtime/compile.py#L50) | 构建 / 报告 / 视图 | 141 | `build/sources/Makefile` |
| `build/sources/host.cpp` | vortex | vortex | [vortex_StorageStacked/third_party/gem5/runtime/compile.py:50](../vortex_StorageStacked/third_party/gem5/runtime/compile.py#L50) | 构建 / 报告 / 视图 | 4460 | `build/sources/host.cpp` |
| `build/sources/kernel.cpp` | vortex | vortex | [vortex_StorageStacked/third_party/gem5/runtime/compile.py:50](../vortex_StorageStacked/third_party/gem5/runtime/compile.py#L50) | 构建 / 报告 / 视图 | 9259 | `build/sources/kernel.cpp` |
| `build/sources/llm_parallel.h` | vortex | vortex | [vortex_StorageStacked/third_party/gem5/runtime/compile.py:50](../vortex_StorageStacked/third_party/gem5/runtime/compile.py#L50) | 构建 / 报告 / 视图 | 409 | `build/sources/llm_parallel.h` |
| `build/sources/project_config.h` | vortex | vortex | [vortex_StorageStacked/third_party/gem5/runtime/compile.py:50](../vortex_StorageStacked/third_party/gem5/runtime/compile.py#L50) | 构建 / 报告 / 视图 | 455 | `build/sources/project_config.h` |
| `build/sources/tinyllm.h` | vortex | vortex | [vortex_StorageStacked/third_party/gem5/runtime/compile.py:50](../vortex_StorageStacked/third_party/gem5/runtime/compile.py#L50) | 构建 / 报告 / 视图 | 25635 | `build/sources/tinyllm.h` |
| `build/system.path` | vortex | vortex | [vortex_StorageStacked/third_party/gem5/runtime/session.py:62](../vortex_StorageStacked/third_party/gem5/runtime/session.py#L62) | 构建 / 报告 / 视图 | 37 | `build/system.path` |
| `check_summary.json` | coralnpu,vortex | vortex | [vortex_StorageStacked/third_party/gem5/runtime/bridge/check.py:203](../vortex_StorageStacked/third_party/gem5/runtime/bridge/check.py#L203) | check, check_negative | 373 | `checks/check_summary.json` |
| `citations.bib` | vortex | vortex | [vortex_StorageStacked/third_party/gem5/runtime/result_layout.py:9](../vortex_StorageStacked/third_party/gem5/runtime/result_layout.py#L9) | 构建 / 报告 / 视图 | 3951 | `traces/vortex/citations.bib` |
| `completion.json` | coralnpu,vortex | vortex | [coralnpu_StorageStacked/integration/main.cc:113](../coralnpu_StorageStacked/integration/main.cc#L113) | check_negative, verify | 121 | `checks/completion.json` |
| `config.dot` | vortex | vortex | [vortex_StorageStacked/third_party/gem5/runtime/result_layout.py:9](../vortex_StorageStacked/third_party/gem5/runtime/result_layout.py#L9) | 构建 / 报告 / 视图 | 84488 | `traces/config.dot` |
| `config.dot.pdf` | vortex | vortex | [vortex_StorageStacked/third_party/gem5/runtime/result_layout.py:9](../vortex_StorageStacked/third_party/gem5/runtime/result_layout.py#L9) | 构建 / 报告 / 视图 | 37265 | `traces/config.dot.pdf` |
| `config.dot.svg` | vortex | vortex | [vortex_StorageStacked/third_party/gem5/runtime/result_layout.py:10](../vortex_StorageStacked/third_party/gem5/runtime/result_layout.py#L10) | 构建 / 报告 / 视图 | 84599 | `views/config.dot.svg` |
| `config.ini` | vortex | vortex | [vortex_StorageStacked/third_party/gem5/runtime/result_layout.py:7](../vortex_StorageStacked/third_party/gem5/runtime/result_layout.py#L7) | 构建 / 报告 / 视图 | 42780 | `config/config.ini` |
| `environment.json` | coralnpu,vortex | vortex | [axi_StorageStacked/scripts/run.py:144](../axi_StorageStacked/scripts/run.py#L144) | run | 924 | `config/environment.json` |
| `hettrace/host.hettrace` | vortex | vortex | [vortex_StorageStacked/third_party/gem5/src/hettrace/het_axi_monitor.cc:86](../vortex_StorageStacked/third_party/gem5/src/hettrace/het_axi_monitor.cc#L86) | 构建 / 报告 / 视图 | 1937104 | `traces/vortex/hettrace/host.hettrace` |
| `hettrace/host.hettrace.meta.json` | vortex | vortex | [vortex_StorageStacked/third_party/gem5/src/hettrace/het_axi_monitor.cc:86](../vortex_StorageStacked/third_party/gem5/src/hettrace/het_axi_monitor.cc#L86) | 构建 / 报告 / 视图 | 429 | `traces/vortex/hettrace/host.hettrace.meta.json` |
| `hettrace/validation.txt` | vortex | vortex | [vortex_StorageStacked/third_party/gem5/src/hettrace/het_axi_monitor.cc:86](../vortex_StorageStacked/third_party/gem5/src/hettrace/het_axi_monitor.cc#L86) | verify | 1524 | `traces/vortex/hettrace/validation.txt` |
| `hettrace/vortex.hettrace` | vortex | vortex | [vortex_StorageStacked/third_party/gem5/src/hettrace/het_axi_monitor.cc:86](../vortex_StorageStacked/third_party/gem5/src/hettrace/het_axi_monitor.cc#L86) | 构建 / 报告 / 视图 | 644904 | `traces/vortex/hettrace/vortex.hettrace` |
| `hettrace/vortex.hettrace.meta.json` | vortex | vortex | [vortex_StorageStacked/third_party/gem5/src/hettrace/het_axi_monitor.cc:86](../vortex_StorageStacked/third_party/gem5/src/hettrace/het_axi_monitor.cc#L86) | 构建 / 报告 / 视图 | 431 | `traces/vortex/hettrace/vortex.hettrace.meta.json` |
| `host_stderr.log` | vortex | vortex | [vortex_StorageStacked/third_party/gem5/runtime/soc.py:56](../vortex_StorageStacked/third_party/gem5/runtime/soc.py#L56) | 构建 / 报告 / 视图 | 0 | `logs/host_stderr.log` |
| `host_stdout.log` | vortex | vortex | [vortex_StorageStacked/third_party/gem5/runtime/soc.py:56](../vortex_StorageStacked/third_party/gem5/runtime/soc.py#L56) | 构建 / 报告 / 视图 | 18984 | `logs/host_stdout.log` |
| `host_summary.json` | vortex | vortex | [vortex_StorageStacked/third_party/gem5/runtime/verify_host.py:66](../vortex_StorageStacked/third_party/gem5/runtime/verify_host.py#L66) | check_negative, report, verify_host | 2191 | `checks/host_summary.json` |
| `llm_summary.json` | coralnpu,vortex | vortex | [coralnpu_StorageStacked/coralnpu/runtime/verify_llm.py:104](../coralnpu_StorageStacked/coralnpu/runtime/verify_llm.py#L104) | report, verify_llm | 2789 | `checks/llm_summary.json` |
| `memsim_data/*.js` | coralnpu,vortex | vortex | [axi_StorageStacked/checks/view_assets.py:7](../axi_StorageStacked/checks/view_assets.py#L7) | 构建 / 报告 / 视图 | 219154 | `views/memsim_data/*.js` |
| `mmu_translations.csv` | vortex | vortex | [vortex_StorageStacked/third_party/gem5/runtime/verify.py:36](../vortex_StorageStacked/third_party/gem5/runtime/verify.py#L36) | report, verify, verify_host | 20069 | `traces/vortex/mmu_translations.csv` |
| `report.md` | coralnpu,vortex | vortex | [vortex_StorageStacked/third_party/gem5/runtime/report.py:5](../vortex_StorageStacked/third_party/gem5/runtime/report.py#L5) | 构建 / 报告 / 视图 | 2339 | `report.md` |
| `resolved.json` | coralnpu,vortex | vortex | [vortex_StorageStacked/third_party/gem5/runtime/session.py:55](../vortex_StorageStacked/third_party/gem5/runtime/session.py#L55) | check_aou, report, trace_policy | 36023 | `config/resolved.json` |
| `soc_summary.json` | vortex | vortex | [vortex_StorageStacked/third_party/gem5/runtime/verify.py:52](../vortex_StorageStacked/third_party/gem5/runtime/verify.py#L52) | report, verify | 112365 | `checks/soc_summary.json` |
| `stats.txt` | vortex | vortex | [vortex_StorageStacked/third_party/gem5/runtime/result_layout.py:7](../vortex_StorageStacked/third_party/gem5/runtime/result_layout.py#L7) | verify, verify_host | 378334 | `traces/vortex/stats.txt` |
| `trace_flits_data/*.js` | coralnpu,vortex | vortex | [axi_StorageStacked/checks/view_assets.py:7](../axi_StorageStacked/checks/view_assets.py#L7) | 构建 / 报告 / 视图 | 98483 | `views/trace_flits_data/*.js` |
| `trace_paths_data/*.js` | coralnpu,vortex | vortex | [axi_StorageStacked/checks/view_assets.py:7](../axi_StorageStacked/checks/view_assets.py#L7) | 构建 / 报告 / 视图 | 39577 | `views/trace_paths_data/*.js` |
| `trace_view.html` | coralnpu,vortex | vortex | [axi_StorageStacked/checks/trace_view.py:85](../axi_StorageStacked/checks/trace_view.py#L85) | memsim_view, trace_view | 792379 | `views/trace_view.html` |
| `transactions.csv` | vortex | vortex | [axi_StorageStacked/checks/memsim_view.py:16](../axi_StorageStacked/checks/memsim_view.py#L16) | check, check_negative, memsim_view, verify | 1299529 | `traces/vortex/transactions.csv` |
| `validation.log` | coralnpu,vortex | vortex | [coralnpu_StorageStacked/coralnpu/runtime/test_rtl.py:57](../coralnpu_StorageStacked/coralnpu/runtime/test_rtl.py#L57) | 构建 / 报告 / 视图 | 75 | `logs/validation.log` |
| `verification.log` | coralnpu,vortex | vortex | [vortex_StorageStacked/third_party/gem5/runtime/validate.py:22](../vortex_StorageStacked/third_party/gem5/runtime/validate.py#L22) | 构建 / 报告 / 视图 | 2105 | `logs/verification.log` |
| `view_store.js` | coralnpu,vortex | vortex | [axi_StorageStacked/checks/view_assets.py:31](../axi_StorageStacked/checks/view_assets.py#L31) | memsim_view, trace_view, view_assets | 3003 | `views/view_store.js` |
| `wave_audit/llm/first_48ns_edges.csv` | coralnpu,vortex | vortex | [axi_StorageStacked/checks/audit_wave.py:168](../axi_StorageStacked/checks/audit_wave.py#L168) | audit_wave | 792 | `checks/wave_audit/llm/first_48ns_edges.csv` |
| `wave_audit/llm/handshakes_from_vcd.csv` | coralnpu,vortex | vortex | [axi_StorageStacked/checks/audit_wave.py:168](../axi_StorageStacked/checks/audit_wave.py#L168) | audit_wave | 2505576 | `checks/wave_audit/llm/handshakes_from_vcd.csv` |
| `wave_audit/llm/summary.json` | coralnpu,vortex | vortex | [axi_StorageStacked/scripts/run.py:134](../axi_StorageStacked/scripts/run.py#L134) | run, test | 904 | `checks/wave_audit/llm/summary.json` |

## trace 级别与验收

full 默认保留完整记录并执行真实检查。light 关闭占比较大的 axi_wave.vcd、ucie_flits.csv、ucie_soc.csv、ucie_mem.csv，保留实际仿真、返回数据、内存与协议概要以及错误信息；不会生成派生的完整链路视图。

light 的 summary 明确为 passed=false、link_verified=false；它只表示诊断仿真完成。检查器在读取不完整 trace 前拒绝完整验收，smoke/test 同样要求 full。默认 full 的产物与基线保持相同语义。
