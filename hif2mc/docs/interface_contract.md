# 第一版 HIF 接口契约

这是 `hif2mc` 的可执行接口约定。资料未明确或存在冲突的部分，必须区分为项目选择。
它不是特定厂商 MC 的周期精确替代物，不构成器件、DFI 或 HIF 全功能合规认证。

## 1. 资料规定与本版选择

| 项目 | 资料含义 | 第一版实际行为 |
|---|---|---|
| 命令握手 | 上升沿 `valid && !stall` 接收 | 同一命令停顿期间不得改变或撤销 valid |
| 数据握手 | 写数据使用独立 valid/stall | 停顿时 data/mask/end 必须稳定 |
| 命令编码 | WPS：00 写、01 读、10 RMW、11 保留 | 按 WPS 编码；截图中重复的 `01 read-mod-write` 注释不采用 |
| length | 截图参数 1bit，WPS 表另有 2bit 部分长度编码 | 只接受 0；整体长度由显式 `command_bytes` 决定，不猜测其他编码 |
| 优先级 | VPRW 关闭时 0=LPR，1=HPR；启用时编码不同 | 只支持关闭 VPRW 的 0/1；写/RMW pri=0 |
| 地址 | `MEMC_DRAM_DATA_WIDTH/8` 字节为一地址单位 | 从 `address_unit_bytes` 换算字节地址，必须整命令对齐 |
| pointer | 写命令的写缓存标识，不是地址 | 按命令顺序回送；数据严格按通知顺序、不交织 |
| pointer 延迟 | 波形最少 2 拍，最大未指定 | `pointer_delay_cycles >= 2`；默认 2，队列冲突可以延后 |
| 首拍数据 | pointer 通知后最少 1 拍 | 通知被采样的下一上升沿才允许接收数据 |
| 读返回 | token 配对，可乱序，valid 可有空隙 | 父请求按后端完成顺序返回；单父请求各拍连续，不交织 |
| 读返回反压 | 无 ready | 完整返回空间由在途上下文预留 |
| credit | 每笔命令消耗相应请求额度，资源释放时归还 | 独立管理，选定明确的返还策略，见第 4 节 |
| 写完成 | 无独立逐笔写完成信号 | 仅内部完成统计；WR credit 不是写完成应答 |

未规定的写数据交织、动态协商、优先级超时、热复位行为不自动补成“原厂规定”。

## 2. 信号与位序

方向相对模型/MC；`tick()` 表示 `core_ddrc_core_clk` 的一次上升沿。

| 方向 | 信号 | 参考位宽 |
|---|---|---|
| 输入 | `core_ddrc_rstn` | 1，低有效 |
| 输入 | `hif_cmd_valid/type/addr/pri` | 1 / 2 / 40 / 2 |
| 输入 | `hif_cmd_token/length/wdata_ptr` | 53 / 只支持 0 / 19 |
| 输入 | `hif_cmd_autopre/latency` | 1 / 11；本版只支持 0 |
| 输出 | `hif_cmd_stall` | 1 |
| 输入 | `hif_wdata_valid/data/mask/end` | 1 / 256 / 32 / 1 |
| 输出 | `hif_wdata_stall` | 1 |
| 输出 | `hif_wdata_ptr_valid/ptr/ptr_addr_err` | 1 / 19 / 1 |
| 输出 | `hif_rdata_valid/end/token/data` | 1 / 1 / 53 / 256 |
| 输出 | `hif_rdata_addr_err/uncorr_ecc_err/crc_err` | 各 1 |
| 输出 | `hif_lpr_credit/hpr_credit/wr_credit` | 参考模式每类 1bit 脉冲 |
| 输出 | `hif_wrecc_credit` | 2，本版恒 0 |
| 输入 | `hif_go2critical_lpr/hpr/wr` | 各 1，本版要求 0 |

数据采用 `vector[8*i +: 8]` 对应第 i 个字节，mask[i]=1 写对应字节。
一笔命令各拍按递增字节地址传输；只支持命令对齐、critical word=0 的顺序访问。
`hif_wdata_ptr_addr_err` 是统一名称，时序图中的 `hif_wdata_ptr_err` 指同一类错误。
C++ 的 uint64_t/uint32_t 只是容器，命令有效时检查配置位宽，不截断 token 或 pointer。
第一版数据总线固定为 256bit，并非任意位宽通用实现。

## 3. 显式配置与参考组合

`Config{}` 缺少地址单位、命令字节数、时钟、资源和策略，构造时会失败。
`Config::reference_64b()` 明确选择：

| 配置 | 取值与性质 |
|---|---|
| HIF 参考组合 | DRAM_DATA_WIDTH=32bit、FREQ_RATIO=4、BURST_LENGTH=16；WPS 支持的组合之一 |
| HIF 一拍 | 256bit=32B |
| 地址单位 | 4B，因此 byte_address = cmd_addr × 4 |
| 完整命令 | 64B，两拍，length=0；cmd_addr 低 4bit 必须为 0 |
| LPR/HPR/WR | 24/8/32；选用示意图数值作模型配置，不是固定硬件规格 |
| 在途上下文 | 最多 128，独立于 credit；每笔预留完整数据空间 |
| HIF 时钟 | 2000ps；项目其他链路使用 2ns 的参考选择 |
| pointer 最小延迟 | 2 周期 |
| 写数据组装 FIFO | 2 拍；每周期最多组装 1 拍 |
| RMW | 具名参考配置启用；普通 Config 默认关闭 |
| credit 策略 | McCommandIssued，含下面明确的退化路径 |

模型读取后端实际事务粒度和 tick 时长，要求命令长度是后端事务字节数的整数倍。
HBM4 基准源码构建的示例为 32B 子事务、250ps/tick，因此一条 HIF 命令有两个子事务，
每 HIF 周期推进 8 tick。不同周期通过累积余量换算，不把每周期固定 tick 数写死。
这套 HIF 参考组合与后端 HBM 物理总线/突发组织属于不同层，后端仍使用自己的参数。

## 4. Credit 生命周期

类别 k 的计数按上升沿更新：

```text
available[k] += 本沿采样到的返还脉冲
available[k] -= 本沿被接受命令需要的额度
```

读消耗一个 LPR 或 HPR；写消耗一个 WR；RMW 同时消耗一个 LPR 和一个 WR。
返还可以连续为高，每个采样周期计一次；同类事件积压时排队输出，每拍最多一个。
`reuse_credit_same_edge=true` 明确允许归还当拍立即使用；关闭时下一拍才可使用。
初始额度由双方配置共享，复位时不发送一串“初始 credit”，无动态额度协商。

两种策略不能混称：

1. `McCommandIssued`：最后一个子事务发出 RD/RDA 或 WR/WRA 后释放该命令对应资源。
   这是可观察的 **MC 命令发射**，早于后端完成；不是 WPS 所述引脚级 DFI 消费。
2. `BackendCompleted`：整组后端子事务完成时释放，作为明确选择的保守功能策略。
   它也不等待最后一个 HIF 读数据拍；credit 和读输出空间是不同资源。

当前 `mem_sim` 有公开的每控制器命令记录，但没有直接供本模块调用的 DFI 消费通知。
`MemSimBackend` 因而保留并逐 tick 读取命令记录；不会把 `try_submit()` 成功当作发射。
若某事务由写合并或读转发完成，没有自己的发射记录，在完成时才释放；
`stats().credit_fallbacks` 显式记录这种退化，不能冒充提前返还。
此版本命令记录会保留历史，长仿真会有内存/追踪开销；不宣称已适配为恒定内存事件流。

credit 返回后仍可能保留 token、完整响应或等待写完成，因此 `max_inflight` 满时仍 stall。
重名的在途 token/pointer 也会 stall，直到前一笔上下文退休；编号不会因 credit 提前返还而被覆盖。
本模型的 HPR 数字映射到后端 qos_class，区分额度池；不宣称复现原厂完整调度机制。

## 5. 写和 RMW

示例按上升沿编号：命令沿 0 接收，pointer 最早沿 2 被采样，第一拍数据最早沿 3 被接收。
`pointer_valid` 连续有效时，每沿通知一笔；模型用通知队列确定当前数据属于谁。
最后一拍只有 `valid && !stall && end` 才结束。早到或缺失的 end 会报协议错误。
组装 FIFO 满或还没有到期的已通知请求时输出数据 stall，命令反压独立产生。
增大 `write_assembly_interval` 可降低组装资源的服务频率；这不是随机注入 stall。

RMW 是实际的“后端读旧值 → 等待新数据 → 按 mask 合并 → 后端写回”。内部读不会
发出普通 HIF 读数据。读阶段释放 LPR，写阶段释放 WR；两类事件独立。
旧值读取和外部新数据接收可以重叠。在整个读改写期间，较晚的重叠地址请求被阻挡。
这保证本适配器发起的访问不会插入 RMW，但不包含其他外部主机绕过适配器访问后端的情况。
这是逻辑 RMW，不复刻厂商 ECC SRAM 端口冲突和所有 ECC 开销。
mask=0 的字节必须有已初始化旧值，否则不能把未知旧值写成“已初始化”，模型明确停止。

## 6. 错误、复位和未实现合同

- 越界、地址乘法溢出或不对齐：不提交后端。读按完整拍数返回零数据及 addr_err。
- 错误写：pointer 通知带 addr_err。本版约定主机仍发送完整数据段，模型接收后丢弃并退还资源；
  “看到 pointer error 后取消数据”的另一种行为未定义，不能混接。
- 不可纠正读 ECC：映射 uncorr_ecc_err；已纠正 ECC 正常返回。未初始化读在统计中记录，
  保留后端数据，不伪装成 ECC 或 CRC 错误。
- CRC/Inline ECC credit：本版关闭，输出 0；无虚构 CRC 检查。
- DataMismatch、Retry、Timeout，以及写完成后无法表达的失败：停止仿真并给出异常诊断，
  不静默吞掉，也不添加非标准 HIF 写响应。异常后模型进入停止状态，不能继续 tick。
- 非零 autopre、length、latency、go2critical、VPR 编码及保留命令会明确报错；
  禁用 RMW/HPR 后出现对应有效命令也会报错。
- 复位期间禁止接收。只允许空闲接口复位，保留后端数据、时钟与生命周期统计；
  这是接口复位，不是重建/复位整个存储系统。存在在途请求时的热复位会报错。
- 不支持运行中 DVFS/改变后端 tick 时长、pin-level DFI、动态初始化额度或厂商全部配置组合。

## 7. 后端映射

每个父请求的读/写阶段使用独立 host_request_id，每个子事务有唯一 Request::id。
请求地址、transfer_bytes、payload、byte_mask、transaction_index/count 都明确填写。
单 stack 时显式 AddressMapper 解码；多 stack 由 MemorySystem 路由并重新解码。
只保留 HostResponse 模式，避免开启两种响应视图却漏消费其中一种。
响应队列、在途上下文、数据组装 FIFO 均有独立边界。

调用方应独占传入 Backend；不能让另一个驱动器同时推进同一个 MemorySystem。
后端完成和 HIF 最后一拍都排空后才能 finish。析构不自动替用户排空或完成半笔写。
