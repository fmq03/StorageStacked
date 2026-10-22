# 流量前端接口

frontend 公共接口当前支持两种形态：一次性 `generate_traffic()` 和默认 CLI 使用的流式 `TrafficStream`。大 trace、synthetic 满负载和 `BW/BR` burst 都应优先走 streaming source，避免把完整 workload 存成 vector。

本目录声明 workload/frontend 层接口。frontend 负责产生请求，不负责 DRAM 命令级行为。

主要头文件：

- `traffic.hpp`：synthetic traffic、trace reader、初始化/训练控制序列生成接口。

修改建议：

- 新增 workload pattern 或 trace 格式时优先改 frontend。
- 进入 controller 前应完成地址解码，减少调度热路径重复工作。
- mode register、training、DVFS、低功耗这类控制序列应以维护请求形式进入统一命令路径。
- 数据正确性 trace 扩展也属于 frontend：`data=` 生成写 payload，`expect=` 生成读期望，`mask=` 生成 masked write 语义。

## frontend 层输入输出

输入：

- CLI/config 中的 traffic 参数。
- trace 文件路径。
- `DramSpec` 中的地址组织和初始化/训练需求。

输出：

- 已经带有 `DecodedAddress` 的 `Request` 列表。
- 可选的初始化、mode register、WCK training、DVFS、低功耗、RAS/ECC 控制请求。

frontend 不应该直接发 `ACT/RD/WR` 等 DRAM 命令。它只描述上层请求或控制序列，
具体命令展开由 controller 和 dram state 决定。

## trace 与 synthetic workload

- synthetic workload 适合做带宽趋势、channel mapper、row policy 对比。
- trace workload 适合做固定访存序列、golden 对比和回归测试。
- 初始化/训练序列适合验证 mode register、WCK、DVFS、低功耗状态是否走完整路径。
- 数据正确性 trace 适合验证真实存储区，写请求用 `data=HEX`，读请求用 `expect=HEX`，masked write 用 `mask=HEX`；如果要人工检查每条写的提交顺序，建议在 trace 第一列写显式 cycle。
