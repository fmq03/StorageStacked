# 控制器接口

controller 头文件定义请求队列、调度、timing gate 和命令执行入口。当前 controller 不保存完整存储体；真实 payload commit、read-forward 和 read-check 通过 `ControllerOptions::memory_image` 与 `DataValidator` 接入。

本目录声明 controller 层的公开接口。controller 层负责 request buffer、调度、
row policy、refresh/RFM、timing gate 和命令执行，是 Ramulator2.1 风格控制器骨架的核心。

主要头文件：

- `controller.hpp`：单 channel controller 顶层接口和 `ControllerOptions`。
- `request_queues.hpp`：四类请求队列、容量查询、active 提升与移除及 bank 所有权。
- `command.hpp`：已发射命令事件格式，用于 trace、统计和 validator。
- `executor.hpp`：`CommandExecutor`，集中声明命令副作用执行接口。
- `timing.hpp`：`TimingEngine`，声明 scope timing、tFAW、WCK window 等检查接口。
- `refresh.hpp`、`rfm.hpp`：维护请求 manager 接口。
- `scheduler.hpp`、`row_policy.hpp`：调度和行策略接口。

修改建议：

- Controller 只做协调，不应把 JEDEC 表、命令语义或统计格式写死在这里。
- 新增调度策略时优先改 `scheduler.hpp/cpp`。
- 新增命令副作用时优先改 `executor.hpp/cpp`，并同步更新 validator。
- 与真实数据、floorplan、功耗、热模型相关的状态由 `core/data.hpp` 声明；controller 只持有共享 `MemoryImage`/`DataValidator` 并在命令或请求完成点调用它们。

## controller 层数据流

典型请求会经过下面路径：

```text
Request
  -> Controller::enqueue()
  -> read/write/priority/active buffer
  -> Scheduler 选择候选请求
  -> dram/state 判断命令是否合法
  -> TimingEngine 判断 timing 是否 ready
  -> CommandExecutor 执行命令副作用
  -> Stats 与 IssuedCommand trace 更新
```

这里的接口只描述 controller 需要对外暴露的形状，具体行为在 `src/controller/`
中实现。头文件保持相对轻量，有助于把 controller 当作可替换模块。

## 主要类型分工

- `Controller`：单 stack、单 channel 控制器。多 stack/multi-channel 并行由 `MemorySystem` 按 `[stack][channel]` 持有多个 controller 实现；Controller 本身不承担跨 stack 路由。
- `ControllerOptions`：队列大小、scheduler、row policy、watermark 等控制器策略配置。
- `IssuedCommand`：已经发出的命令事件，包含 cycle、命令、`stack_id`、stack-local/global 地址、bus、decoded 坐标，以及可选真实 payload、byte mask 和 initialized mask 快照；DFI trace 使用这些快照导出真实 `dfi_wrdata/dfi_rddata`。
- `CommandExecutor`：执行命令后的状态变化，例如打开 row、关闭 bank、更新 RFM 计数。
- `TimingEngine`：维护跨 scope timing bucket，例如 channel、rank、bank-group、bank。
- `RefreshManager` / `RfmManager`：生成维护请求，不直接决定普通读写调度。

## 扩展注意事项

- 新增命令后，不要只改 executor；还要补 `dram/semantics`、`dram/state`、`stats` 和 `validation`。
- 新增 scheduler 时，不要改变 `Request` 结构，优先通过候选视图和比较规则实现。
- 新增 row policy 时，要明确它是改变 RD/WR 形态，还是主动插入 PRE 类维护请求。
- 新增 HBM/LPDDR 差异时，优先让 `DramSpec` 提供开关，controller 根据配置走不同路径。
