# 工具

工具目录用于放置不参与仿真热路径的辅助脚本。当前主要用途是比较文本统计输出；memory image 的 txt/CSV/bin 读写由模拟器本身完成，不放在 tools 中重新实现，避免出现第二套不一致的数据格式。

本目录保存辅助工具脚本，不属于模拟器核心库。工具应尽量保持独立、可从命令行直接运行。

主要文件：

- `compare_stats.py`：比较两次仿真输出中的关键统计字段，用于轻量 golden/baseline 对比。
- `view_stats.py`：把一份或多份完整 JSON / `key : value` 统计文件按指定字段输出为对齐表格。
- `result_io.py`：共同结果读取器；实验工具要求完成状态、整数计数与必要字段，不再把缺失值补零。
- `audit_case_backends.sh`：用相同配置、trace 和初始 image 审计 sparse/mmap/chunk 三种后端。
- `config_selection.py`：验证工具共享的配置/preset 选择表，共同面入口位于 `configs/validation/*.cfg`，避免各工具重复维护路径。
- `model_validation.py`：运行项目原生 HBM4 配置，检查分析公式、性能阈值、真实存储、full-stack、DFI、来源和 project identity。
- `timing_boundary_validation.py`：审计 C++ probe 生成的四标准 `t-1/t` Timing 边界矩阵。
- `ramulator2_differential.py`：将本地 Ramulator2.1 作为非规范性外部参考，检查四标准共同命令面。
- `performance_curve.py`：生成注入率—延迟—吞吐量 CSV/JSON 曲线。
- `sensitivity_uncertainty.py`：生成 Timing 单因素敏感性和联合输入扰动区间。
- `dramsim3_aux_validation.py`：执行较老 HBM 公共命令、IDD 功耗公式和热趋势辅助验证。
- `visualize.py`：将已有 command/DFI CSV、性能曲线 JSON、thermal map 和文本统计输出为一个无需服务端的交互式 HTML 仪表盘。

## 离线可视化

项目不把浏览器框架嵌入仿真核心。先按原有方式导出 trace，再用标准库脚本生成一个
自包含 HTML；复制该 HTML 到另一台机器也能离线查看。
热图读取 `address_kind`，悬停时将仅耦合节点标为地址未知，不将邻居的代表地址误当成
该节点的 DRAM 地址；没有此列的输入仍可显示温度和网格。
这对应 Ramulator2 的 offline trace visualizer。当前不实现 HTTP/WebSocket 实时推流，
因为那会把服务端生命周期和网络错误处理引入 controller 的仿真热路径；以后若确有实时
观测需求，可在此 HTML 数据格式之外增加独立 streamer，而不改变现有 trace 入口。

```bash
mkdir -p outputs/visualization
./build-clang-debug/hbm_sim --config configs/hbm.cfg --standard hbm4 --requests 128 \
  --stats-json outputs/visualization/result.json \
  --cmd-trace outputs/visualization/commands.csv \
  --dfi-trace outputs/visualization/dfi.csv \
  --dump-thermal-map outputs/visualization/thermal_map.txt \
  | tee outputs/visualization/stats.txt

python3 tools/visualize.py \
  --command-trace outputs/visualization/commands.csv \
  --dfi-trace outputs/visualization/dfi.csv \
  --stats outputs/visualization/result.json \
  --thermal-map outputs/visualization/thermal_map.txt \
  --out outputs/visualization/dashboard.html
```

仪表盘借鉴 Ramulator 的 offline trace explorer，包含 command/bank 与 request swimlane
两种时间线、密度概览与点击定位、stack/channel/command/request 过滤、命令比例、DFI
command/data 计数、可选的注入率—延迟—吞吐曲线和热图。生成后还可以在浏览器中通过
`Open command CSV` 替换 command trace，方便观察另一轮实验而不需要重新运行 Python。
`make visualize-example` 会产生完整 HBM4 示例；`make visualization-smoke` 验证生成器
可消费当前 CLI 的实际输出。

修改建议：

- 工具输出应适合 CI 和人工阅读，失败时说明字段、baseline、candidate 和阈值。
- 不要把核心仿真逻辑写进 tools；核心行为应在 `src/`。
- Python 工具产生的 `__pycache__/` 已被 `.gitignore` 忽略。
- 后续适合放在这里的工具包括：`final_memory.txt` 转 CSV/Excel、checkpoint diff、`mismatch_report.txt` 汇总、`thermal_map.txt` 热点排序/绘图，以及更广的 DRAMsim3/Ramulator2.1 差分矩阵。

## `compare_stats.py`

这个工具用于比较两份 `hbm_sim` 文本输出中的统计字段。典型用途：

- 对比本次修改前后的 smoke 输出。
- 对比某个配置在不同 scheduler/channel mapper 下的关键指标。
- 作为通用 golden/baseline 文本统计检查；结构化 Ramulator2.1 命令差分使用 `ramulator2_differential.py`。

使用方式示例：

```bash
python3 tools/compare_stats.py baseline.txt candidate.txt \
  --keys completed_reads,commands.ACT --abs-tol 0 --rel-tol 0

python3 tools/compare_stats.py baseline.txt candidate.txt \
  --keys achieved_bw_GBps --rel-tol 0.05
```

只查看或并排阅读统计值时使用 `view_stats.py`；它不做 PASS/FAIL 判断：

```bash
python3 tools/view_stats.py outputs/a.txt outputs/b.txt \
  --labels baseline,candidate \
  --keys standard,completed_reads,system_cycles,achieved_bw_GBps,avg_read_latency
```

三后端审计是可直接重复调用的仓库脚本，不依赖当前 Bash 会话中是否定义过函数：

```bash
bash tools/audit_case_backends.sh configs/hbm.cfg \
  hbm4 none \
  outputs/backend_audit_hbm4_run1
```

文件后端具有持久状态。为防止旧 image 污染新结果，若输出目录已经包含 `data.bin` 或
`.meta`，脚本会明确失败；请换一个新目录，或由操作者确认后自行归档/删除旧目录。

## `model_validation.py`

```bash
python3 tools/model_validation.py \
  --binary ./build-clang-debug/hbm_sim \
  --json-out outputs/hbm_sim_model_validation.json
```

它也由 `ctest --test-dir build-clang-debug --output-on-failure` 自动运行。Makefile
兼容入口为 `make CXX=/usr/bin/clang++-18 model-validation`。

当前项目原生持续负载基线为 64.5%，默认允许 4.5 个百分点的回归余量，因此门槛为
60%。它是固定 workload/controller/profile 的经验回归基线，不是从 `nCCDR` 单独
推导的器件效率。脚本还检查同地址顺序、mismatch 退出码、DFI expected payload、
file-backed 重开、四标准入口、burst、物理/功耗/热路径和 identity 证据绑定。

## `ramulator2_differential.py`

```bash
python3 tools/ramulator2_differential.py \
  --binary ./build-clang-debug/hbm_sim \
  --ramulator-root /path/to/ramulator2 \
  --json-out outputs/hbm_sim_ramulator2_diff.json
```

Makefile 兼容入口为
`make CXX=/usr/bin/clang++-18 reference-validation RAMULATOR2_ROOT=/path/to/ramulator2`。

当前参考检查覆盖 HBM3/HBM4/LPDDR5/LPDDR6 的 timing、命令顺序、decoded 坐标、
逐命令周期、same/different BG、双向总线换向、tFAW、refresh/RFM 和自动预充电。
JSON 同时保存 Ramulator 根目录、Git commit/describe、dirty 状态和 Python 版本；局部容差
与规范化规则逐场景记录，不能表述为实现或器件等价。`git_dirty=true` 时应保存外部仓库
diff，或在干净 checkout 上复跑后再把结果作为归档基线。

字段比较建议：

- 请求完成数、命令计数：通常使用绝对误差 `0`。
- 带宽、延迟、利用率：可使用相对误差，例如 `0.01` 或 `0.05`。
- 队列平均值：通常允许一定误差，因为调度微调可能影响排队过程。

## 工具维护原则

- 工具只读取文件和打印比较结果，不调用内部 C++ API。
- 失败输出要说明字段名、baseline、candidate、差值和阈值。
- 用户希望保留的验证产物写到项目内 `outputs/`；测试内部的短生命周期夹具可使用
  系统临时目录，并在退出前删除。
- 不要提交 `__pycache__`、临时输出、实验日志。

## 适合新增的工具

- `memory_image_to_csv.py`：把 `final_memory.txt` 的 `key=value` 文本转成标准 CSV，便于 Excel 打开。
- `memory_image_diff.py`：比较两个 memory image dump 的 `address/data/init/version/last_writer`。
- `mismatch_summary.py`：统计 mismatch report 中的地址、请求号、bank/row/column 和 expected/actual 差异。
- `thermal_hotspots.py`：按温度或能量排序 `thermal_map.txt`，输出热点坐标。
