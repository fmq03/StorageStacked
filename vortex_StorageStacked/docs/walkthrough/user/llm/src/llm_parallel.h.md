# llm_parallel.h：组内线程怎样共享中间结果

对应源码：[llm_parallel.h](https://github.com/hy2581/vortex_StorageStacked/blob/977cfe8acb737d2f99a6304135a861ba8112ca9c/user/llm/src/llm_parallel.h)。

`LlmScratch` 定义一个工作组共享的中间数组与计数，默认占 660 字节。
主机通过 `launch.lmem_size = sizeof(LlmScratch)` 申请，GPU 通过 `__local_mem()` 取得地址。
它是 GPU 的共享局部内存，权重、KV、trace、token 和 report 仍走外部存储链路。

| 内容 | 用途 |
|---|---|
| `x/n1/q/k/v/.../logits` | 一个 token 位置计算中的中间向量 |
| `probs` | 两个注意力头的概率 |
| `errors/calls/best/linear_calls` | 线程 0 更新的错误数、前向次数、选出的 token 和线性层次数 |
| `column_counts[tid]` | 每个线程独立累计实际计算的输出列数 |

线性层中，线程 t 计算 `t, t+blockDim.x, ...` 列，并写入共享输出数组。
所有线程执行工作组屏障后，线程 0 或下一线性层才能消费这些结果。
默认 8 个线程，线性层执行 42 次，实际列数为 `[60,54,54,54,54,54,54,54]`。
这些计数写入 report，经 CPU 实际回读后由独立参考核对。
