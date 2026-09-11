# 结果边界与限制

当前流程是“功能执行与存储时序分离”的 open-loop 方法。最重要的限制不是精度参数，而是外部
存储 completion 不会回到 XPU 改变后继请求。

## 各层能说明什么

| 证据 | 可以说明 | 不能说明 |
|---|---|---|
| workload 自检 + gem5 | host/NPU/Vortex 真实数据交接和功能结果 | 外部 DRAM 参数下的应用性能 |
| 统一三源 HETTrace | packet 访存量、地址、时间与确定性五通道投影 | 原生 AXI 协议实现；三源记录均带 `SYNTH` |
| CoralNPU 原生 seam | 当前 16 B 单拍子集的地址、ID、WSTRB | 统一点的五通道事件、LEN/SIZE/BURST/USER 与 pin-level 时序 |
| 外部 `hbm_sim` | 固定请求流在给定 controller/DRAM 配置下的注入、排队和完成时序 | 反馈后会产生的不同流量、IPC、tokens/s |
| `storage_chain/` | 透明边界、ready/valid、反压和字段保真 | 完整 AXI VIP、DRAM 性能或产品实现 |

## Open-loop 的因果边界

流程先由 gem5 生成完整 trace，再由外部模型消费：

```text
gem5 request timeline ──固定──> hbm_sim
                              completion
                                  ╳ 不反馈给 gem5
```

因此可以比较同一请求流在不同 channel、bank、page policy 或 DRAM 配置下的服务时间；不能把
`hbm_sim` 的最终 cycle 直接换算成应用运行时间。真实反馈可能改变 cache miss 并发度、队列
占用、同步时刻、控制流乃至请求集合，这些变化在固定 trace 中不存在。

`hbm_sim` 的 `arrival_cycle` 也可能晚于 trace 的 `requested_cycle`：前端会按自身接受能力串行
注入。比较工具要求 `arrival >= requested`，并单独报告 injection wait，而不是错误地要求二者
相等。

## AXI 保真边界

- 单一 gem5 monitor 位于共享 memory-side interconnect，能避免不同 tap 层级混算；
- 统一 monitor 看到的三源都是 gem5 packet，全部五通道记录均为 `SYNTH`；CoralNPU 原生 seam
  只为当前 16 B 单拍子集保留地址、ID 与 WSTRB，不能把整份统一 trace 称为原生 pin trace；
- monitor 会拆分超过 256 拍或跨 4 KiB 的 packet，生成合法 INCR burst；
- HETTrace 记录地址、WSTRB 和响应，不记录 WDATA/RDATA；
- `mem_sim` 输入中的零值 `data=`/`expect=` 只精确携带请求大小，不能补回 WDATA/RDATA；
- gem5 functional memory 的 `--mem-latency`/`--mem-bandwidth` 只负责让程序可执行，不是最终
  DRAM 性能参数。

## 系统边界

- 当前是 gem5 SE 模式，不包含完整 OS、IOMMU 或 cache coherence；共享映射必须 uncacheable；
- CoralNPU 地址宽度为 32 位，不能直接访问 4 GiB 以上的 Vortex BAR；两设备跨域数据由 host
  中转；
- Vortex SimX 的私有 Ramulator 仍存在，但只服务 Vortex 内部实现，不能与外部 `hbm_sim`
  结果混称为同一存储模型；
- `storage_chain/` 不含 UCIe、memory controller、DFI 或 PHY 实现；
- 合成 LLM workload 复现访存形状，不执行 Transformer 数值计算。

## 做结论前的最低检查

1. workload 功能自检通过；
2. `hettrace validate` 通过，且 meta 的 `unmapped`/`non_monotonic` 为零；
3. 三源正式 trace 均为 `level=interconnect`，并正确区分 `SYNTH`；
4. `mem_sim.trace`、mapping CSV、response CSV 的 request ID 一一对应；
5. 对比实验只改变目标参数，保留 trace、`ticks-per-cycle` 和 warm-up 口径；
6. 报告中明确写成“固定请求流的 open-loop 存储时序”，不写成应用闭环性能。
