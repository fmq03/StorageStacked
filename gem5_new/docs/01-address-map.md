# 统一物理地址空间

`addrmap.json` 是地址区域、访问者和源时钟的唯一手写真值源。修改后必须同步生成 C++/Python
表并检查：

```bash
python3 scripts/gen_addrmap.py
python3 scripts/gen_addrmap.py --check
```

生成物为 `libhettrace/include/hettrace/addrmap.h` 和 `tools/hettrace/addrmap.py`。gem5 配置受其
Python 运行环境限制仍保留常量，若两边漂移，trace 会出现 `unmapped`，正式回归会失败。

## 区域

| 区域 | 基址 | 大小 | 用途/访问者 | gem5 responder |
|---|---:|---:|---|---|
| `boot_rom` | `0x00000000` | 256 MiB | host 预留 | — |
| `npu_slave` | `0x10000000` | 256 MiB | CoralNPU 内部视图 | device |
| `vortex_cp` | `0x20000000` | 512 B | host→Vortex command processor | `VortexGPGPU.pio` |
| `npu_pio` | `0x30000000` | 4 KiB | host→CoralNPU registers | `CoralNPU.pio` |
| `host_heap` | `0x80000000` | 256 MiB | host 代码、堆和栈 | `host_mem` |
| `shared_buffer` | `0x90000000` | 256 MiB | host、CoralNPU 显式交接 | `unified_mem` |
| `vortex_vram` | `0xa0000000` | 256 MiB | Vortex 设备局部诊断视图 | 不单独声明 |
| `npu_work` | `0xb0000000` | 256 MiB | host、NPU 工作区 | `unified_mem` |
| `npu_mailbox` | `0xc0000000` | 16 B | CoralNPU 内部 mailbox | device |
| `vortex_bar` | `0x100000000` | 4 GiB | host、Vortex 共享 BAR | `unified_mem` |

正式 HETTrace 统一使用 host 物理视图。Vortex 设备地址通过
`host_pa = pin_addr + vortex_dev_addr` 对齐到 `vortex_bar`，因此 host 与 Vortex 对同一字节
记录相同地址。`vortex_vram` 只用于 standalone 设备视角诊断，不应与正式三源 trace 混用。

## 硬约束

CoralNPU AXI 地址为 32 位，只把 `[0x80000000, 0xc0000000)` 判为 DDR，不能表达 4 GiB 以上
的 Vortex BAR。Vortex runtime 又固定使用 `PIN_BASE_ADDR=0x100000000` 和 4 GiB pin region。
所以 NPU 与 Vortex 不能直接共享同一物理字节，三源 workload 由 host 分别完成交接。
当前地址图不存在三方以同一物理地址直连共享的区域。

每个地址只能有一个 responder。异构配置由 `host_mem` 响应 `host_heap`，由
`UnifiedTimingMemory` 响应 `shared_buffer`、`npu_work` 和启用时的 `vortex_bar`；Vortex BAR
不能同时声明为另一段 PIO memory。下游零延迟 xbar 只做地址解码，不是存储控制器。

## SE 页池与一致性

只有 `host_mem` 设置 `conf_table_reported=True`，所以 gem5 SE 页池只会从 `host_heap` 分配
进程页，不会悄悄侵入共享区。设备窗口由 host 以 VA=PA、`cacheable=False` 显式映射。当前
系统没有 CPU cache 与设备 DMA 的一致性协议，去缓存属性是功能正确性的必要条件。

`UnifiedTimingMemory` 保存共享字节并返回 gem5 timing response，使程序能够执行；其
`--mem-latency`、`--mem-bandwidth` 不是最终 DRAM 性能真值。统一 memory-side monitor 在地址
解码前观察所有功能请求，再把固定 trace 交给外部 `hbm_sim`。

## Trace 窗口与时钟

`HETTRACE_FILTER=dram` 对应 `[0x80000000, 0xc0000000) ∪ vortex_bar`。若修改 BAR/DDR 窗口却
未重新生成地址表，记录可能被过滤或标为 `unmapped`。分析前检查 meta 中的 `filtered`、
`unmapped` 和区域分布。

| 源 | 频率 | gem5 tick/周期 |
|---|---:|---:|
| host | 2 GHz | 500 |
| Vortex | 1 GHz | 1000 |
| CoralNPU | 500 MHz | 2000 |

gem5 全局 tick 为 1 ps。修改源时钟时必须同步 `addrmap.json`、gem5 clock domain 和回归预期。
