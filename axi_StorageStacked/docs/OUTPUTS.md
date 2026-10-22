# 项目输出：AXI 响应、结果文件与 SMOKE 验收

[返回文档目录](README.md)。

输入信号、JSON 格式和配置见 [INPUTS.md](INPUTS.md)。本文说明实际返回什么、如何定位一笔
访存、SMOKE 的通过条件、2026-09-25 的历史实测结果，以及当时新增 SMOKE 对源码和运行状态的影响。
逐步创建输入、脚本、入口和失败用例的完整教程见 [从零添加 SMOKE 测试](03-从零添加SMOKE测试.md)。

## 1. 信号级输出

以下方向均为 `AouBackend` → 主设备。只有对应的 `VALID && READY` 在 AXI 上升沿同时为 1，
才计作接收一次。有效响应遇到反压时，存储侧保持 VALID 和全部响应负载，直到完成握手。

| 输出信号 | 位宽 | 表示什么 |
|---|---:|---|
| `awready` | 1 | 可接收一个写地址；不是写完成 |
| `wready` | 1 | 可接收当前一拍写数据；不是写入内存完成 |
| `arready` | 1 | 可接收一个读地址；不是数据已返回 |
| `bid` | 16 | 写响应对应的 AWID |
| `bresp` | 2 | 整个写 burst 的响应状态 |
| `bvalid` | 1 | 写响应有效，与 BREADY 握手后该写事务完成 |
| `rid` | 16 | 当前读响应拍对应的 ARID |
| `rdata` | 256 | 32 个总线字节通道；窄拍按地址低 5 位选取有效 lane |
| `rresp` | 2 | 当前读响应拍的状态 |
| `rlast` | 1 | 当前读 burst 的最后一拍标志 |
| `rvalid` | 1 | 当前读响应拍有效；最后一拍与 RREADY 握手后该读事务完成 |

写 burst 返回一个 B，读 burst 返回 AxLEN+1 个 R。正常访问的响应来自在线内存完成事件，
再经过 UCIe 返回，最终由主设备 READY 策略决定握手时刻。

| 数值 | AXI 名称 | 本项目中的含义 |
|---:|---|---|
| 0 | OKAY | 正常完成；成功读的数据必须逐字节正确 |
| 1 | EXOKAY | 当前公开非独占接口没有此成功场景 |
| 2 | SLVERR | 内存原生失败等错误映射；窄拍非法字节使能也可能产生此状态 |
| 3 | DECERR | 整笔访问未落在 `[base, base+size)` 内；不提交原生内存子请求 |

错误响应的数据不应当作成功访存结果使用。结构不合法的 AXI 输入可能直接触发契约失败，
不能假定所有错误都返回 B/R。

`ready()` 是 C++ 链路就绪查询，链路处于 Active 或 Degraded 时为 true，训练失败时报错；
它不代表某笔事务已经完成。`finish(directory)` 写出最终记录并检查在途内存请求已排空，
调用方应先等待全部 B/R 完成。

## 2. 独立程序直接返回的文件

### 2.1 responses.json：逐笔返回结果

此文件由 C++ `FileMaster` 在实际 B/R 握手后写入。数组顺序对应 JSON 输入顺序，因为文件驱动
串行发起事务；这不是对任意并发主设备的全局返回顺序承诺。

SMOKE 最后一笔读的实际记录为：

```json
{
  "command": "read",
  "id": 6,
  "address": 2415919132,
  "response": 0,
  "begin_tick_fs": 620000000,
  "end_tick_fs": 676000000,
  "data": ["aa1dcc1f"]
}
```

| 字段 | 类型／单位 | 解释 |
|---|---|---|
| `command` | 字符串 | `read` 或 `write` |
| `id` | 整数 | 原始事务 ID；C++ 驱动还检查实际 BID/RID 与它相符 |
| `address` | 整数，字节地址 | 请求的 AXI 绝对地址，不是内存窗口内偏移 |
| `response` | 整数，0～3 | 写为 BRESP；多拍读为各拍 RRESP 的按位或汇总，逐拍状态查看 AXI 日志 |
| `begin_tick_fs` | 整数，fs | 文件主设备开始驱动该请求的仿真时刻，早于或等于首地址握手 |
| `end_tick_fs` | 整数，fs | B 握手或最后一拍 R 握手的仿真时刻 |
| `data` | 字符串数组 | 写为 `[]`；读每拍一个字符串，仅提取该拍有效字节，按地址递增排列 |

`end_tick_fs - begin_tick_fs` 是文件主设备观察到的请求往返时间，包含 AXI 等待、链路、
内存调度及 B/R 反压。它不是纯 DRAM 延迟，也不是程序的真实墙钟运行时间。
`1 ns = 1000000 fs`；上述最后一笔读的往返时间为 56 ns。
`end_tick_fs` 也不包含结束后的排空、统计导出和波形关闭所需时间。

### 2.2 这次运行保存的配置、总摘要和运行日志

| 文件 | 产生者 | 用途 |
|---|---|---|
| `input.json` | Python 入口 | 已补齐默认值、检查格式并标准化的完整输入；可作为重跑输入 |
| `config.json` | Python 入口 | `runtime="standalone-axi"` 和 `axi` 配置对象，供链路／内存审计使用 |
| `summary.json` | 通用运行器；SMOKE 最后补充字段 | `passed`、版本、事务数、末笔完成时间、检查项、波形审计摘要；SMOKE 还包含 `smoke_passed` 与报告名 |
| `smoke_summary.json` | 新增 SMOKE 入口 | 固定场景覆盖结果、通道计数、回读字节、掩码映射、原生访存数量 |
| `run.log` | 仿真程序和检查器的 stdout/stderr | 仿真异常、断言失败和各层审计输出 |

通用运行器先写 `passed:false`，所有检查通过才改为 true。SMOKE 发生仿真、数据或覆盖检查失败时，
会把两个摘要都写为 false 并非零退出。若在创建目录前就失败（例如目录已存在、禁用了 Python 断言），
不会写入该目录。进程被外部终止时也可能缺少最终文件，不能把残留文件或部分 true 当作 SMOKE 通过。

## 3. 证据文件：从 AXI 到实际内存

以下是通过 `./run.sh run` 或 `./run.sh smoke` 运行后产生的文件。直接 C++ 集成的调用方需要
自行注册 monitor/trace 并调用相应检查器，不能假定所有文件自动存在。

### 3.1 AXI 波形与握手

| 文件 | 内容与读法 |
|---|---|
| `axi_wave.vcd` | 外部 ACLK/ARESETn、五通道信号及 AXI2Flit/UCIe 内部信号，时间单位 1 fs |
| `axi_events.csv` | 外部监视器记录每次握手：`tick,cycle,channel,id,address,len,size,data,strb,last,resp` |
| `protocol_summary.json` | 总线宽度、每秒 tick 数、AXI 周期、各通道握手次数和反压采样沿数 |
| `aou_events.csv` | 公共桥接内部的 AW/W/B/AR/R 和 TX/RX 事件，可与外部采样对照 |
| `axi256_handshakes.csv` | 检查器从波形恢复的 256 位握手及十六进制负载 |
| `axi_first_200ns_cycles.csv` | 检查器导出的前 200 ns 有效周期视图，用于初始时序定位 |
| `wave_audit/handshakes_from_vcd.csv` | 独立波形审计重新恢复的握手记录 |
| `wave_audit/first_48ns_edges.csv` | 前 48 ns 采样沿，用于复位、训练后首请求定位 |
| `wave_audit/summary.json` | VCD/CSV 一致性、VALID 与负载保持、AW/W→B、AR→R、LAST 和排空检查 |

`axi_events.csv` 的 data 和 strb 是**十进制总线整数**，不是按地址排列的字节串；
`aou_events.csv` 的 `data_hex` 则是从最高 lane 到最低 lane 输出的总线数值。
例如输入的 `00010203...1f` 在总线十六进制视图显示为 `1f1e...03020100`，含义相同。

外部监视器的 W 行没有 ID/地址字段来源，相关列为 0；必须按 AW 顺序恢复所属 burst。
R 行用 RID 关联 AR 获取地址。`aou_events.csv` 的 W 行会填入恢复后的地址，不能机械地逐列
比较这两个 CSV。`cycle` 是监视器的上升沿计数，包含复位期间边沿；时间关联应优先使用 tick。

### 3.2 UCIe 与消息关联

| 文件 | 内容与用途 |
|---|---|
| `ucie_flits.csv` | 两个方向、两端观测的实际 Flit：时间、端点、方向、事件、序号、尝试次数、重放、状态、长度和原始字节 |
| `ucie_soc.csv`、`ucie_mem.csv` | 上述日志按 SOC/MEM 端点拆分 |
| `aou_summary.json` | 目标读写次数、读写拍数、内存完成数、Flit 数、重放与错误统计 |
| `aou_check_summary.json` | AXI/桥接/波形/传输计数与数据的一致性检查 |
| `aou_messages.csv` | 从原始 Flit 解码出的 ReadReq、WriteReq、WriteData、ReadData、WriteResp、Credit 等消息 |
| `flit_pairs.csv` | 以方向和序号关联发送与接收，记录传输延迟、尝试次数与最终状态 |
| `axi_flit_path.csv` | 每个 AXI 握手关联的消息、资源平面、Flit 序号区间、发送和接收时间 |
| `link_check_summary.json` | 独立解码检查结果、观测数量、已交付和退出时仍在链路中的 Flit 数 |

FWD 是 SOC→MEM，REV 是 MEM→SOC。`TX_FDI/RX_FDI` 是适配器边界事件，
`TX_FRAME/RX_FRAME` 是链路帧事件；相同请求会有多条日志记录，不能将行数当作请求数。
当前 FDI 记录 250 字节、物理帧记录 256 字节，由现有链路编码决定。

结束时允许存在**只有信用流控消息或空内容**的在途 Flit；检查器明确拒绝业务消息尚未交付。
本次 SMOKE 的 `fdi_in_flight_at_exit=1` 属于此情形，不表示有 AXI 读写未完成。
总 Flit 数也包含信用消息，不能与 AW/W/B/AR/R 次数简单相等。

### 3.3 在线内存、DRAM 和 DFI

| 文件 | 内容与用途 |
|---|---|
| `memsim_config.json` | 实际采用的内存标准、通道、step 周期、原生事务粒度、队列、窗口和 PHY 类型 |
| `memsim_bridge.csv` | `accept/submit/submit_stall/complete/return` 事件；包含 burst 序号、AXI ID、地址、字节数、偏移、原生子请求 ID、时间、数据、掩码 |
| `memsim_bridge_summary.json` | AXI burst 数、原生子请求数、错误响应数、提交与响应停顿次数 |
| `memsim_core.json` | 原生提交／返回数、周期数、命令与 DFI 事件数、命令与 DFI 错误数 |
| `memsim_commands.csv` | ACT、RD、WR、PRE、REF 等实际调度命令，包含 request_id、周期和地址映射 |
| `memsim_dfi.csv` | DFI 命令／数据事件与来源、拍数、地址、初始化状态 |
| `memsim_dfi_signals.csv` | 行为级 DFI 信号、写数据、写掩码、读数据和使能／有效信息 |
| `memsim_image.csv` | 最终内存内容、初始化标记及地址映射等元数据 |
| `memsim_stats.txt` | 原生模型的附加运行统计 |
| `memsim_check.json` | 将 AXI/Flit、内存子请求、DRAM/DFI 和最终镜像逐项关联后的结果 |
| `memsim_journeys.json` | 每个 burst 的完整访存轨迹，含原生子请求、完成与实际 DRAM 命令 |

桥接日志使用 AXI **绝对地址**，原生命令和镜像的 address 使用**窗口内偏移**：
`native_address = axi_address - base`。默认 AXI 地址 `0x9000001c` 对应镜像地址 `0x1c`。
默认窗口较大，镜像按已触及的原生粒度稀疏导出；本次 SMOKE 只有两个 32 字节行，
不是缺少了 768 MiB 数据。未写入位置的模型初值为零。

AXI burst 可以拆成多个原生子请求。默认 HBM4 的原生粒度为 32 字节，
本次 64 字节 burst 拆成 2 个，4 字节窄拍对应 1 个子请求。
`mem_id`/`request_id` 是原生子请求编号，与 AXI ID 含义不同。

桥接 `tick` 的单位为 fs；原生 `cycle/issued_cycle/completion_cycle` 需要乘
`memsim_config.json.period_fs` 才能关联仿真时间。默认内存 step 是 250000 fs，即 0.25 ns，
与 AXI 的 2 ns 周期不同；配置还会记录 tCK 等参数，不能将这些周期混用。
桥接写掩码字符串的字符 1 表示写入；DFI 的 `dfi_wrdata_mask` 按字节用 00 表示允许、ff 表示屏蔽。

### 3.4 如何追踪某一笔请求

1. 在 `responses.json` 找到地址、ID、开始／完成时间，确认响应和读回数据。
2. 在外部 `axi_events.csv` 找 AW/AR 和对应 B/R；必要时打开 VCD 观察 READY/VALID。
3. 用 `axi_flit_path.csv` 的 `channel + axi_id + axi_tick_fs` 找传输消息和往返序号，
   再用 `aou_messages.csv` / `flit_pairs.csv` 查看原始链路解码。
4. 用 `memsim_journeys.json` 的 burst、AXI ID、地址和时刻，定位 `memsim_bridge.csv` 中的
   accept→submit→complete→return；由 mem_id 关联 DRAM 命令和 DFI 数据。
5. 用 `memsim_image.csv` 的窗口内偏移检查最终字节。最终镜像只说明最后状态，中间读值仍以逐笔响应为准。

ID 可以在前一事务完成后复用，不能只按 ID 关联整份日志。命令时序也可能受刷新和队列影响，
不要仅凭某个配置名推断整段运行的延迟一定单调变化。

## 4. SMOKE 的通过条件与实际执行结果

统一命令为 `./run.sh smoke`。本次 2026-09-25 实际执行：

```bash
./run.sh build
./run.sh smoke --output results/smoke-20260925-docs
./run.sh test --output results/acceptance-20260925-smoke
```

上述目录是已经产生的证据，重跑应省略 `--output` 或选择另一个新目录。
完整原始波形和日志位于本机 `results/smoke-20260925-docs/`。
便于随源码审阅的关键数值和判据统一保存在本文，历史结果目录已移出项目树。

SMOKE 通过必须同时满足：

- 进程正常返回，`summary.json.passed`、`summary.json.smoke_passed` 和
  `smoke_summary.json.passed` 均为 true。
- 6 笔操作身份、响应和完成顺序正确，所有响应为 OKAY；四次读满足显式 expected 和独立字节模型。
- AW=2、W=3、B=2、AR=4、R=7；VCD 与 CSV 相符，VALID/负载保持与 LAST 正确，无未完成 AXI 请求。
- 实际出现 B/R 反压；窄拍 WDATA 位于 lane 28～31，WSTRB 为 `0x50000000`，最终窄拍回读 `aa1dcc1f`。
- 原始 Flit 解码与 AXI 数据一致，六笔 burst 均经过在线内存；原生 10 个子请求全部返回。
- DRAM 中有 7 个 RD、3 个 WR，DFI 数据／掩码和最终镜像正确，命令与 DFI 错误为零。

本次实际结果：

| 检查 | 实测值 |
|---|---|
| SMOKE | PASS，6 笔事务、18 次五通道握手 |
| B/R 反压采样沿 | B=8、R=28；另观测 W=4 |
| 在线内存 | 6 个 burst、10 个子请求，提交=返回=10 |
| DRAM 数据命令 | RD=7、WR=3；另有 ACT=2、PREpb=1、REFpb=20，共 33 条命令 |
| DFI 数据事件 | 18；包含命令等在内的全部 DFI 事件为 51，错误数 0 |
| 最终镜像 | 64 字节；只把模式数据的偏移 28、30 改为 aa、cc |
| 末笔响应完成时间 | 676000000 fs = 676 ns |
| 完整回归 | 19/19 原生测试、在线 C ABI、8/8 场景通过；6 类非法输入及损坏读返回被拒绝 |

各笔时间从当次实际 responses.json 提取并保存在下表：

| ID | 操作 | 开始 ns | 完成 ns | 往返 ns |
|---:|---|---:|---:|---:|
| 1 | 初始 64 字节读 | 24 | 122 | 98 |
| 2 | 64 字节写 | 124 | 416 | 292 |
| 3 | 完整回读 | 418 | 486 | 68 |
| 4 | 4 字节掩码写 | 488 | 548 | 60 |
| 5 | 掩码后 64 字节读 | 550 | 618 | 68 |
| 6 | 掩码后 4 字节读 | 620 | 676 | 56 |

这些是当前固定配置的记录值，SMOKE 不把这些精确时间写死为跨版本通过条件。
完整回归的延迟反馈用例另外检查同一笔首写的往返时间，从 scale=1 的 84 ns 增至 scale=4 的 144 ns。

同日的公共实现拆分记录通过 7 个原有场景、19 项原生测试及在线 C ABI；
加入固定 SMOKE 后，发布复验与 Vortex 目录适配记录均通过 8 个场景。
SMOKE 教程还验证了通用运行、Python 检查入口和完整链路路径，错误掩码与错误期望值被拒绝。
上述历史目录中的关键结果统一收录于本文；新的原始输入、响应和检查输出继续写入 results。

## 5. 失败如何判定，覆盖边界在哪里

| 现象 | 判定与排查 |
|---|---|
| 没有 `build/axi_storage` 或动态库无法加载 | 构建／加载失败，先执行 `./run.sh build`；不算运行通过 |
| 提示结果目录已存在 | 换新目录，旧结果被保留 |
| 禁用 Python 断言 | SMOKE 拒绝执行，清除 `PYTHONOPTIMIZE` 并使用正常 Python |
| JSON 非法、地址不对齐、跨 4 KiB | 输入被拒绝，不生成假的成功响应 |
| 链路训练或 AXI watchdog 超时 | 仿真失败，查看 run.log，不能用部分日志认定通过 |
| 仿真退出 0，但字节、握手或 Flit 不一致 | 验收失败，总摘要为 false；返回码 0 只表明仿真程序自身没有抛错 |
| 通用运行检查通过，但 SMOKE 覆盖计数或窄拍断言失败 | SMOKE 专属失败，两个摘要均为 false |
| 命令／DFI 错误或原生请求未返回 | 在线内存验收失败，沿 memsim_journeys 追踪 |

本次还执行了以下失败路径验证，结论从当次 negative_checks.json 整理：

1. 对已有 SMOKE 结果目录再次运行，命令被拒绝；逐文件比较内容，原结果全部保持不变。
2. 设置 `PYTHONOPTIMIZE=1` 后运行，命令被拒绝且不创建结果目录。
3. 验证脚本在内存中复制输入，把部分写 strobe 从 5 改为 15，同时把后续期望数据改为完整写入值。
   真实仿真及通用审计通过，但 SMOKE 的 WSTRB 覆盖检查失败，两个摘要均为 false。
   固定的 `examples/smoke.json` 没有被修改。这份预期失败的原始证据保留在
   `results/smoke-20260925-negative-coverage/`，不计入成功的 SMOKE 场景。

这个短用例验证固定 HBM4 配置下的基本链路、数据正确性、多拍、掩码和响应反压。
它不覆盖并发压力、ID 重用、所有 AxSIZE、256 拍极限、所有内存标准、错误注入重放、窗口外错误、
运行中复位或处理器计算。更广覆盖使用 `./run.sh test` 和 [examples/edge_cases.json](../examples/edge_cases.json)。
PHY/DFI 为行为模型，HBM4 当前配置记录了 23 个临时时序项；SMOKE 不是物理接口或工艺签核。

## 6. SMOKE 对项目做了哪些更改

### 6.1 源码与入口改动

| 文件 | 新增／修改内容 | 作用 |
|---|---|---|
| `docs/INPUTS.md` | 新增本文配套输入文档 | 说明信号、JSON、配置、字节通道、启动顺序及固定 SMOKE 输入 |
| `docs/OUTPUTS.md` | 新增输出文档 | 说明返回值、结果文件、实测数据、失败判定与本改动清单 |
| `examples/smoke.json` | 新增六笔固定事务和四组显式读期望 | 覆盖初始值、完整写回读、掩码写、窄拍读 |
| `scripts/smoke.py` | 新增 `run_smoke()` 和命令行入口 | 调用现有 `run_case()`，追加固定覆盖、lane、WSTRB、原生 RD/WR 计数检查，并写 SMOKE 报告 |
| `run.sh` | 增加 `smoke` 分支与帮助信息 | 提供可复制的统一入口 `./run.sh smoke [--output 新目录]` |
| `scripts/test.py` | 在原有七场景之外调用 `run_smoke(output/'smoke')` | 完整验收增为八场景；摘要中包含 smoke；原生 19 项与 C ABI 检查保留 |
| `README.md` | 增加两篇文档和 SMOKE 命令链接 | 从项目首页可直接找到输入、输出和快速验收 |
| `ACCEPTANCE.md` | 追加本次执行命令与结果 | 保留旧验收记录，补充新 SMOKE／八场景回归证据 |
| `docs/OUTPUTS.md` | 集中保存固定 SMOKE 的关键数值、验收判据与失败路径结论 | 历史结果目录已移出项目树，新的原始证据由 results 保存 |

通用 `scripts/run.py`、C++ 文件主设备、AXI 端口定义、存储桥接、UCIe、mem_sim、CMake 配置与
项目版本保持原实现。本次没有添加测试 RAM、预写内存捷径、功能性旁路或处理器依赖。
新增 SMOKE 通过固定输入与断言组合使用已有的真实链路。

`run_smoke()` 的执行顺序是：检查断言模式与新目录 → 读取固定 JSON → 调用通用仿真和全部链路审计
→ 检查 SMOKE 覆盖 → 写专属报告并在总摘要标记 smoke_passed。
覆盖检查失败也会改写总摘要为 false，避免底层 run 已通过而顶层 SMOKE 误报成功。

### 6.2 运行一次 SMOKE 会修改什么

运行会新建结果目录，并在该目录写入这次运行保存的配置、响应、日志、波形、内存镜像及审计报告。
它复用预先构建的 `build/axi_storage`，不会在运行期间编辑源码或示例输入。
`./run.sh build` 是单独的准备步骤，会更新 build 中的编译出的文件；完整 test 还会创建原生测试与
其他场景的结果文件。

在这次仿真进程的 mem_sim 内部，SMOKE 先读取零值，再写入地址
`[0x90000000, 0x90000040)` 的 64 字节，最后把 `0x9000001c` 改为 aa、`0x9000001e` 改为 cc。
其余字节保持模式值。末次窄拍读的结果因此是 `aa1dcc1f`。
进程退出后保存的是导出的内存镜像；下次独立运行新建内存状态，不自动导入上次镜像。
它不会改变两个处理器驱动进程中的内存，也不会对主机物理地址 `0x90000000` 进行访问。

源码变更与仿真中的写入是两件事：前者是新增文档、向量和验收入口，后者是为了验证本项目
真实读写能力而在模型地址空间执行的两笔 AXI 写事务。

## 7. 第一次读结果时，只追一笔请求

先在 `responses.json` 找到目标读操作，记下 ID、地址、开始和结束时刻。
例如读取一个 4 字节数，返回 `"29000000"`，表示四个字节 `29 00 00 00`。
用小端整数解释为 41；VCD 的整条 256 位信号按比特位显示，外观顺序可能不同。

然后在 `axi_events.csv` 对照该次 AR 与 R，检查实际握手、ID 和响应码。
文件驱动按顺序执行，可以复用已完成的 ID；关联请求时同时看顺序、地址和时间，
不要把所有同 ID 的行都拼成一笔访问。

| 问题 | 正确读法 |
|---|---|
| 为什么 CSV 有很多行？ | AW、W、B、AR、R 分别记录；多拍也会增加数据行 |
| 为什么写响应没有读数据？ | B 表示写入的完成状态，读数据由 R 返回 |
| 某个检查文件 passed=true 就结束了吗？ | 继续看最终 summary；SMOKE 还需专属检查通过 |
| 看到时间 2000000 是多久？ | 若单位为 fs，就是 2 ns |
| 可视化网页空白怎么办？ | 保留配套 JS 和数据目录，先看原始 JSON/CSV 和日志 |

第 4～6 节记载的是 2026-09-25 增加 SMOKE 时的运行与改动，
其中历史时间和计数不能当成本轮文档更新的实测结果。
新输入、版本或参数是否通过，应读新结果目录，不应修改历史结果去适配当前说明。
