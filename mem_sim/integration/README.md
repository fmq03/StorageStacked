# 在线宿主接口

`online.h/.cpp` 将已有 MemorySystem 暴露为 C ABI，共享库目标 `storagestacked_memsim`。
它不依赖 SystemC，不创建线程或独立时间循环。当前宿主桥在
`../../gem5_axi/memsim_backend.hh/.cc`，由 gem5 原生 SystemC 调度；总入口见 `../../env/README.md`。

宿主顺序：create → 在每个原生时刻尝试 submit → step 一次 → pop 可容纳的响应 → finish → destroy。
`period_fs` 返回一个原生 step 在宿主时间轴上的时长；`clock` 是已完成的 step 数。
通道数通过 `config::build_model` 解析，容量、密度和总线宽度保持一致。`scale` 只乘在
宿主时间映射上，不修改原生模型的速率/tCK配对。配置记录同时保留 tCK_ps、
effective_tCK_ps、period_fs 及解析出的容量参数。`memsim_stats.txt` 的内存模型内部指标
仍使用原生时间域；缩放后的端到端延迟以宿主 transactions.csv 和 period_fs 映射为准。
scale 是整段内存时间轴的敏感度实验，不是重新选择器件速度档。
宿主不能提前交付 completion_cycle 对应物理时刻之后才产生的响应。

`submit` 返回 1 表示已复制请求、接管 ID；0 表示入口满，应保留相同请求稍后重试；
-1 表示接口错误，读取 `ss_mem_error()`。ID 在响应 pop 后可复用。
单请求不超过 transaction_bytes（当前 HBM4 为 32B），不能跨该粒度边界，ID 必须在在途请求中唯一。
写入须提供 bytes 长的数据和 0/1 byte mask；读取无需数据指针。地址为本地窗口偏移，
宿主负责窗口范围检查与拆分。`pop` 返回 0/1/-1；所有调用在同一宿主线程执行。
错误字符串有效到下一次错误为止。调用者提供有效句柄、输出目录和缓冲区。

`ss_mem_response.status` 遵循原生 ResponseStatus：0 正常、1 ECC 修正，2–7 为原生异常。
SystemC 桥把 0/1 映射为 AXI OKAY，把其余映射为 SLVERR；窗口越界在桥侧返回 DECERR，
不提交任何子请求。写完成采用原生 HostResponse 完成定义，不额外捏造固定延迟。
DFI 轨迹由实发命令重建；多拍 DFI 数据的末拍可能晚于原生写完成点，不能把该模型定义
误读为外部 RTL 的全部引脚传输结束。

当前配置为单 stack、可配置 channel、HBM4 preset、Behavioral MemPhy、HostOnly 响应队列。
入口、控制器和 PHY 队列有界；训练为 8 个原生 tick。8KiB 窗口在模型唯一 MemoryImage 中
显式初始化为零，无影子内存。原生刷新、行状态、调度和数据检查保持启用。
`finish` 输出原生命令、DFI、内存镜像、统计，检查业务响应排空及命令/DFI 合法性；
后台刷新无需永远停止。

桥持有有限个 burst 的数据缓冲区，按 32B 边界拆分，等待所有子请求完成后回包；
响应全局 FIFO，比同 ID 有序更严格，可能增加队头阻塞。响应 FIFO 满则保留响应，
burst 槽耗尽后停止接收新请求，背压沿 AouTarget/UCIe/AXI 返回上游。
默认不插入额外等待，response_hold 只用于压力测试。

独立 C ABI 验证：

```bash
source env/activate.sh
python mem_sim/integration/check_online.py \
  "$MEMSIM_BUILD/libstoragestacked_memsim.so" results/api-check
```

它验证掩码读写、原生完成、满队列无副作用重试，以及重复 ID / 跨粒度请求拒绝。
完整链路的 `gem5_axi/scripts/check_memsim.py` 进一步将 AXI、原始 Flit 解码结果、子请求、
DRAM/DFI 和最终内存逐项关联。当前验收 workload 要求每个子请求均产生 RD/WR，未覆盖
控制器写合并或读转发；检查器会明确拒绝不符合该范围的结果，而不是静默略过物理证据。
