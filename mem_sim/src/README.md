# src 分层说明

当前实现按“平台 + 模型”分层：frontend 流式产生请求，core 管理地址映射、多 controller 和真实存储区，controller 负责调度和命令完成语义，dram 提供标准/timing/接口规则，validation/stats 负责审计输出。不要把文件后端逻辑写进 controller。

`src/` 按模拟器职责分层，目标是接近 Ramulator2.1 的模块边界，同时保持 HBM/LPDDR 专用小型项目的清晰度。

- `cli/`：命令行、运行选项、结果输出装配；调用配置库生成模型，不写协议状态机。
- `config/`：配置文件分层、模型 key 映射、schema 3 参数联动与矛盾检查；可由库调用。
- `core/`：跨模块核心对象，例如地址映射、多 controller `MemorySystem` 和真实堆叠存储 `MemoryImage`。多 channel 并行、channel mapper、全局命令 trace 合并、真实 payload、bank/row/column/subarray/mat/cell/microbump 存储视图、floorplan、SECDED shadow、功耗和 TSV-aware 热事件都从这里进入。
- `dram/`：DRAM 标准描述，包括标准 traits、JEDEC 换算、命令语义、命令状态合法性、接口开销、完整 organization/timing profile 和派生 timing table。新增 HBM/LPDDR 标准参数时优先改这里。
- `controller/`：控制器内部实现，包括 request buffer 调度、row policy、refresh/RFM manager、timing engine 和命令执行器。它对应 Ramulator2.1 controller 侧的核心骨架。
- `frontend/`：合成流量和 trace 读取。进入 controller 前要完成地址解码，避免调度热路径重复做映射。
- `frontend/` 中还维护初始化/训练控制序列生成器。`init_sequence` 会把 `MRW/MRR/DVFS/WCK_TRAIN/PDE/PDX/SREFEN/SREFEX/ECC_SCRUB/RAS_ERR` 这类维护请求预置到普通 workload 前，使 mode register、WCK training、DVFS、链路保护和低功耗状态可以走完整 controller/validator 路径；生成器会拒绝 HBM/LPDDR 跨族序列，CLI 的 `--check-config` 也会提前报告该错误。
- `stats/`：统计数据、完整文本、人工摘要及版本化 JSON 结果；payload/interface bandwidth、system cycles、controller aggregate cycles 等保持明确口径。
- `validation/`：命令 trace 导出、DFI beat/signal trace 生成、离线 command validator 和 DFI validator。Ramulator2.1/golden trace/DFI 视图扩展应优先复用这个层的结构化事件。

依赖方向应尽量保持为：

```text
cli -> config/frontend/core/controller/stats/validation
config -> dram
core -> controller/stats
controller -> dram/stats
frontend -> core/dram
validation -> dram/core
dram -> common
```

新增功能时优先放入对应层，避免把所有逻辑继续塞回 `Controller` 或 `main`。

命名约定：

- 目录表达大类，文件名表达本层职责。比如 `dram/state.cpp` 是 DRAM 命令状态合法性，`controller/timing.cpp` 是 controller 侧 timing gate，`validation/validator.cpp` 是离线 trace validator。
- 文件名可以短，但类型名保持完整。`controller/executor.cpp` 里仍然实现 `CommandExecutor`，这样读代码和搜索类名时不会丢语义。
- CLI 的用户说明独立放在 `cli/help.cpp`；模型字段覆盖与推导放在 `config/model.cpp`，`cli/main.cpp` 负责运行选项与流程装配。

常见修改入口：

- 新增或校准 JEDEC/vendor timing：标准身份/能力放在 `dram/standard_traits.cpp`，基准 organization/timing 和公式放在 `dram/profiles.cpp`，并更新 `TimingTable` 来源与测试。主模板不重复完整基准表。
- 单个自定义 speed/density/mode：复制家族主配置后修改主输入或内联 Timing，不要求新增集中注册的 preset；通用公式仍放在 `dram/profiles.cpp`，联动规则放在 `config/model.cpp`。
- 每个 preset 必须明确 `standard + speed-bin + density + stack-height + mode + vendor + source`，避免 JEDEC、vendor、外部仿真器和研究默认数值混用。
- 新增命令类别：先改 `include/hbm_sim/core/common.hpp` 的 `Command`，再改 `dram/semantics.cpp`、`dram/state.cpp`、`controller/executor.cpp` 和 `validation/validator.cpp`。MRW/MRR/WCK_SYNC/WCK_TRAIN/DVFS/PDE/PDX/SREFEN/SREFEX/ECC_SCRUB/RAS_ERR 这类控制命令也要同步补 `Stats` 和 sequence test。
- 新增调度策略：扩展 `include/hbm_sim/core/common.hpp` 的 `SchedulerKind` 和 `controller/scheduler.cpp`，保持 Controller 只提供候选视图。
- 新增地址映射：扩展 `AddressMappingKind`、`core/addr_map.cpp` 和相关 sequence test。
- 新增 refresh/RFM 策略：优先扩展 `controller/refresh.cpp` 或 `controller/rfm.cpp`，Controller 只负责把维护请求放入 priority path。
- 新增 LPDDR mode register、WCK、DVFS、CA/link protection 或低功耗行为：先扩展 `DramSpec` 中的配置字段和 `dram/profiles.cpp` 的 profile 展开，再改 `controller/timing.cpp`、`dram/state.cpp` 或 `controller/controller.cpp` 中真正需要的状态机；如果会改变命令合法性，也要同步更新 `validation/validator.cpp` 的离线重放规则。如果该行为消耗命令/地址总线或 metadata bit，还要补 `Stats::interface_command_bits` 或 request-level overhead。
- 新增 LPDDR6 REFdb timing 或其他 dual-bank refresh 规则：先把字段放入 `Timing` 和 timing table，再在 `dram/spec.cpp` 的 LPDDR constraint 中选择保守/精确作用域；如果需要区分 short/long bank-pair，应同步扩展 validator。
- 新增 PHY/DFI 行为：公共生命周期、FIFO 和协议编码放在 `dram/mem_phy.cpp`，事件/CSV 校验放在 `validation/dfi.cpp`。Behavioral 数据完成会改变 Controller completion 路径；仍不宣称 pin-level 模拟训练或厂商 lane 合规。
- 新增接口开销或 metadata lane 口径：优先改 `dram/interface.cpp`，保持 `DramSpec` 只保存配置字段，Controller/MemorySystem 只消费已经计算好的 request/command 开销。
- 新增初始化、训练或链路控制序列：优先改 `frontend/traffic.cpp` 的 `generate_control_sequence()`，再补 `tests/sequence_tests.cpp` 和 `tests/smoke.sh`，保证 CLI、online controller 和离线 validator 都能看到同一条命令路径。
- 新增统计字段：先放到 `Stats`，再在 `stats/stats.cpp` 追加输出，避免改变旧字段顺序。

## 一次请求的代码路径

下面是一个普通读请求从输入到完成的大致路径：

```text
cli/main.cpp
  -> frontend/traffic.cpp 生成 Request
  -> core/addr_map.cpp 解码地址
  -> core/system.cpp 分发到目标 Controller
  -> controller/controller.cpp 放入 read/write buffer
  -> controller/scheduler.cpp 选择候选请求
  -> dram/state.cpp 判断下一条命令是否合法
  -> controller/timing.cpp 判断 timing 是否 ready
  -> controller/executor.cpp 执行命令副作用
  -> stats/stats.cpp 输出统计
  -> validation/validator.cpp 可选离线重放检查
```

读代码时可以按这条路径顺藤摸瓜。不要一开始就从 `controller.cpp` 细节钻进去，
否则容易把地址映射、器件语义、timing gate 和统计口径混在一起。

## 修改类型到目录的映射

| 修改目标 | 优先目录 | 需要同步检查 |
| --- | --- | --- |
| 新增 CLI 参数 | `cli/` | README、smoke、配置字段 |
| 新增 workload 或 trace 格式 | `frontend/` | examples、tests |
| 新增地址映射 | `core/` | sequence test、channel 分布统计 |
| 新增 JEDEC/vendor timing | `dram/`、`configs/hbm.cfg`/`lpddr.cfg` | config tests、strict timing、dump timing table |
| 新增 DFI trace/signal 字段 | `validation/`、`cli/` | smoke、README、CSV 兼容 |
| 新增命令语义 | `dram/` | executor、validator、stats |
| 新增调度策略 | `controller/` | sequence test、smoke |
| 新增 refresh/RFM 行为 | `controller/`、`dram/` | validator、stats |
| 新增统计字段 | `stats/` | smoke、compare tool |
| 新增验证规则 | `validation/` | 正例/反例 sequence test |

## 保持代码简洁的约定

- 文件名依赖目录语义保持简短，类型名保持完整。
- 大逻辑先问“属于哪一层”，不要直接塞进 `main.cpp` 或 `controller.cpp`。
- 真实标准数值和研究默认值必须能从 timing source 区分。
- 新行为至少要有一个测试或 smoke 覆盖入口。
- 如果线上仿真和离线 validator 对同一规则理解不同，以补齐 validator 为优先事项。
