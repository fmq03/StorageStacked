# 核心实现

core 层当前包含真实存储区热路径：`data.cpp` 实现 `MemoryImage`、row buffer、物理坐标、ECC/power/thermal；`stack_model.cpp` 实现外部 MC 可驱动的被动多 stack 器件数组；`memory_backend.cpp` 实现三类 payload 后端；`system.cpp` 实现主动多 stack、多 channel controller、入口反压/QoS、streaming injection 和异步 host response 重组。

本目录实现跨模块核心逻辑，主要是地址映射、多 controller memory system 和真实堆叠存储模型。

主要文件：

- `addr_map.cpp`：系统地址的 interleaved/blocked stack 映射，以及 stack-local Ramulator 风格 channel/pseudo-channel/SID 映射。
- `data.cpp`：真实稀疏存储区、payload 读写、SECDED shadow、数据校验辅助、row buffer、bank/row/column/subarray/mat/cell/microbump 索引、floorplan、tile 内 thermal grid、DRAMsim3-style IDD/VDD 功耗参数、TSV-aware 行为级稀疏三维热耦合和热事件。
- `stack_model.cpp`：被动多 stack 器件数组实现。默认实例化 6 个 stack；显式 `stack_count` 构造保留给对比实验。它只根据外部给出的 `stack_id` 分发 transaction/command，并维护每个 stack 独立的 `MemoryImage`、物理坐标、功耗、热和统计；Bridge-MC Frontend 的地址选择、QoS、reorder、credit 和跨 stack 调度不在这里实现。
- `system.cpp`：每 stack 一套多 channel controller 的创建、stack ingress 反压/QoS、请求分发、并行 tick、transaction completion 收集、host response 重组及 per-stack/全局统计合并。

修改建议：

- 新增地址映射模板时，保持模板名语义清楚，例如 `RoBaRaCoCh`。
- 多 controller 行为应保持一个 channel 一个 controller 的结构，便于对齐 Ramulator2.1。
- system 层只合并统计和 trace，不应实现 DRAM 命令状态机。
- `data.cpp` 是模型层入口，不应反向依赖 CLI；txt load/dump 只作为 checkpoint/report，不参与 RD/WR 热路径的文件 I/O。

## `addr_map.cpp`

地址映射把线性地址拆成 channel、rank、SID、pseudo-channel、bank group、bank、row、column。
它直接影响：

- stream 流量是否均匀分布到多个 channel。
- row hit、row miss、row conflict 的比例。
- HBM pseudo-channel/SID interleave 是否被激活。
- LPDDR efficiency mode 下 secondary subchannel 是否被折叠。

新增映射模板时，要明确模板名的位序含义。项目中类似 `RoBaRaCoCh` 的名字按高位到低位解释。

## `data.cpp`

`data.cpp` 把“地址发生了读写”提升为“真实数据落在堆叠存储区域里”：

- `MemoryImage` 使用稀疏 map 保存访问过的 line，避免分配完整 HBM 容量。
- `DataBlock` 保存 bytes、mask、initialized mask、version、last writer、checksum 和物理坐标。
- `StorageKey` 提供 stack / channel / pseudo-channel / SID / rank / bank group / bank / row / column 视图，保证多个 stack 中相同局部 bank/row/column 不会被当成同一物理位置。
- `RowBufferEntry` 模拟 ACT 打开 row、RD/WR 访问 row buffer、PRE/flush 写回 dirty row。
- `PhysicalAddress` 把存储键映射到 stack / die / layer / tile_x / tile_y / tile_z，并继续给出 tile 内 `thermal_x/thermal_y` 网格坐标、subarray、mat、cell 和 microbump 坐标。
- `StorageModelOptions` 控制 floorplan、power、thermal、命令能量、DRAMsim3-style IDD/VDD 参数、行为级稀疏三维热耦合、TSV 垂直耦合、细粒度几何坐标和 SECDED shadow。
- `DataBlock` 除 payload/mask/version 外，还保存 line 级 SECDED shadow。写入后刷新 shadow，读出前可检查并修正单 bit 错误；`ecc_inject_period` 用于回归测试错误注入。
- `dump_text()`、`dump_csv()`、`dump_binary()`、`load_binary()`、`read_initialized_mask()`、`dump_thermal_text()` 和 `DataValidator::dump_text()` 提供 checkpoint/report/golden verification 支撑。txt 适合代码审计和 diff，CSV 适合 Excel/WPS 直接打开检查 address、data、init mask、version、last writer、bank/row/column、tile 和 ECC shadow，binary 适合大规模稀疏 checkpoint。
- 运行时不直接读写文本或 CSV；输入文件只在启动时加载，输出文件只在结束后
  生成。HBM/LPDDR 容量较大，不适合默认创建完整稠密后端文件。

当前热模型的实现边界很清楚：`thermal_grid_cols_per_tile` 和 `thermal_grid_rows_per_tile` 让 bank tile 内部可以按 row/column 切成网格，`power_source = dramsim3_idd` 会按 DRAMsim3 的 `VDD * IDD * time` 思路推导 ACT/RD/WR/REF 能量；温度更新包含横向邻接、上下层邻接和 TSV-aware 垂直耦合，但只是行为级温差转移，不是完整 RC/材料热求解器。

热节点只在插入时初始化温度，首次直接事件保留耦合预热。耦合邻居独立计算 tile/grid，
无法唯一反解的 DRAM 地址为 -1；热图末列 `address_kind` 标明代表地址是否来自直接事件。
读完成路径用 O(1) 的 `ecc_status_counters()` 快照判定 ECC 响应状态；
完整的 `storage_stats()` 仍用于统计采集，不应在每次读前后触发拓扑/热节点扫描。

## `stack_model.cpp`

`stack_model.cpp` 是面向未来 RTL MC 后端的被动器件接口：

- `StackModel` 表示一个 stack device。它有自己的 `MemoryImage`，因此同一个局部地址在不同 stack 中可以保存不同 payload。
- `MultiStackMemoryModel` 表示多个 stack device 的数组，默认 6 个 stack，只按 `stack_id` 做边界检查、分发和统计汇总。
- transaction-level `read/write` 用于当前软件 workload 或快速验证。
- command-level `StackCommand` 用于未来 MC slice 驱动：外部 RTL MC 产生 `ACT/PRE/RD/WR/REF`，模型记录命令事件、维护数据/row buffer、输出功耗和热统计。
- 这里不做 UCIe flit、Bridge-MC Frontend、全局 QoS、reorder、credit 或 stack 选择策略；这些属于上游 RTL 设计。

## `system.cpp`

`MemorySystem` 管理 `stack_count * channels_per_stack` 个 controller。它负责：

- 把全局地址映射成 stack_id 和 stack-local 地址。
- 通过 per-stack ingress queue 表达反压，并按 FCFS/strict-priority QoS 分发。
- 根据 stack-local decoded channel 或 channel mapper 选择 controller。
- 让所有 stack/channel controller 在同一个 system cycle 并行 tick。
- 合并 per-stack/全局统计和带 `stack_id` 的 command trace。
- 收集 Controller `TransactionResponse`，按 host id/index 重组读数据、初始化掩码和状态。

同一 stack 的多个 Controller 共享一个 `MemoryImage`。
Controller 的单 Channel spec 只是局部调度视图；`density_reference_channels` 保存父模型
Channel 数，以便校验时保留完整器件密度/刷新时序。该字段不是用户配置项。
单个 `controllers()[i].stats()` 中命令、队列和 PHY 字段是 channel-local，storage、ECC、
power、thermal 字段则是该共享 Stack 的快照，不能把后者跨 Channel 再求和。需要物理
统计时应使用 `MemorySystem::stats()` 或 `per_stack_stats()`；系统收尾会按 MemoryImage
重新聚合并去除 Controller 中间快照的重复。

异步模式使用 `try_submit/step/has_response/pop_response`，Maintenance 走独立的
`try_submit_maintenance`；`HostOnly/TransactionOnly/Both` 控制保留哪种响应视图，
`idle/quiescent` 分别表示执行完成和响应也已取空。`run(RequestSource&, ...)`
各重载使用同一流式驱动；CLI 通过 `RunOptions` 提供 Host/transaction 消费回调，
不另写注入循环。默认调用按批处理选项运行。这里提供协议中立的
运行时语义，不实现 UCIe flit、NoC credit 或 RTL CDC，具体映射见
`堆叠存储模型交付手册.md` 的 2.7 节。

`RunOptions::drain_responses` 表示驱动每拍消费所选响应视图，没有回调的
已启用视图会取出并丢弃。回调要求对应 delivery mode 已启用；Disabled 不能
请求 drain。统一驱动负责注入时间检查、拒收重试、step、进度和 finish，
在 cycle limit 处仍消费已经完成的响应，不额外执行尚未完成的事务。
回调是 always-ready 消费者；需要模拟外部 host 停顿时使用逐拍接口。

它不负责决定单个 controller 内本周期发什么 DRAM 命令，这个职责仍在
`src/controller/`；也不宣称模拟完整 UCIe flit/credit/link 仲裁。

## 调试建议

- 如果 `active_controllers` 很小，优先检查 address mapping 和 channel mapper。
- 如果总带宽很低但单 controller 很忙，可能是流量没有分散到足够 channel。
- 如果多 controller 合并统计异常，检查 system-level 和 aggregate-controller-level 分母是否混用。
