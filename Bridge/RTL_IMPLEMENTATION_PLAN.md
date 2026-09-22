# Bridge RTL 连续实施方案

## 当前实施状态（2026-09-22）

已经完成 P0-P7 的 256-bit 功能路径：FDI 解包与跨帧重组、AoU 消息解码、
每 RP credit、4-entry Request FIFO、独立 WData FIFO、8-entry parent/child 表、tag/reorder、32B MC
事务合并与边界切分、lane/mask 转换、响应聚合回包、在线 mem_sim transactor 和
AXI/UCIe 联合仿真均有可执行回归。RP_COUNT=1 是默认配置，RP_COUNT=2 已有定向
回归。入口为：

```bash
make -C Bridge rtl-lint
make -C Bridge rtl-test
make -C Bridge rtl-memsim-test
make -C Bridge rtl-full-link
make -C Bridge rtl-diff
```

`rtl-full-link` 已通过原 `tb_full_link` 的 12 项用例，包括 256-beat burst、越界
DECERR、8 路混合 outstanding、同 ID 顺序、长 B/R 背压和随机 strobe。P7 的
CommMonitor socket 入口已可切换 RTL，并通过 mock gem5 字节级回归；主仓库的
gem5 原生 SystemC `gem5_axi::AouBackend` 也已支持 `bridge_impl=cpp|rtl`，tester
模式完成 RTL -> 原 `MemSimBackend` -> mem_sim 闭环。C++ 与 RTL 确定性对拍的
归一化结果为 `1976` 笔 MC 请求、`4` 笔父事务错误，AXI 计数均为
`AW/W/B/AR/R=55/1219/55/66/1241`。P7 的 NPU、GPU、三源及四倍 mem_sim
时间尺度场景均已通过双 RP RTL Bridge 验收，汇总见
`results/acceptance-xpu-rtl-final-20260922/summary.json`。512/1024-bit、RP_COUNT=4
系统压力回归、热复位及综合收敛仍属于后续工作。

## 1. 目标与边界

本方案用于指导 Bridge 从当前 C++ 行为模型逐步演进为可综合 RTL，并在每个阶段保持
可构建、可测试和可与现有 C++ 参考模型对拍。

第一版 Bridge 的边界固定为：

```text
UcieLink / SystemC
    │ 250B FDI PLP，valid/ready
    ▼
StorageBridge RTL
    │ Memory Controller request/response，valid/ready
    ▼
MemSimTransactor C++
    │ ss_mem_submit / ss_mem_step / ss_mem_pop
    ▼
mem_sim
```

Bridge RTL 负责：

- FDI Flit 收发、AoU 消息编解码及跨 Flit 续传；
- Resource Plane、credit、请求槽和背压管理；
- `WriteReq` 与 `WriteData` 的顺序配对；
- burst 按 beat 和内存事务粒度展开；
- AoU 中的 WDATA/WSTRB lane 转换为紧凑 data/byte mask；
- tag、outstanding、响应重排和父请求完成管理；
- 将读数据或写完成重新编码成 `ReadData`/`WriteResp` Flit。

第一版不包含：

- 250B PLP 与 256B 物理帧之间的 scatter/gather、CRC 和链路重放；
- DRAM 地址映射以及 ACT/PRE/RD/WR 调度；
- DFI、PHY 和 DRAM 引脚级时序；
- 多时钟域 CDC、运行期热复位；
- 面积、频率、功耗和综合时序收敛。

上述功能分别属于 UCIe Link、内存控制器或后续硬件实现阶段。Bridge 接收的是已经由
链路层交付的 250B FDI PLP，下游输出的是内存控制器前端事务，不输出 AXI 信号。

## 2. 顶层端口草案

当前冻结的顶层为 `rtl/storage_bridge_top.sv`。以下列出功能接口；实际模块还包含
`protocol_error`、`busy` 和 `debug_*` 诊断输出：

```systemverilog
module storage_bridge_top #(
    parameter int FDI_PLP_W       = 2000, // 250 B
    parameter int ADDR_W          = 64,
    parameter int MC_DATA_W       = 256,  // 当前 mem_sim transaction 为 32 B
    parameter int MC_MASK_W       = 32,
    parameter int MC_TAG_W        = 32,
    parameter int RP_COUNT        = 1,
    parameter int MAX_OUTSTANDING = 8,
    parameter int MC_INFLIGHT     = 8
) (
    input  logic                 clk,
    input  logic                 rst_n,
    input  logic                 link_active,

    input  logic                 fdi_rx_valid,
    output logic                 fdi_rx_ready,
    input  logic [FDI_PLP_W-1:0] fdi_rx_plp,

    output logic                 fdi_tx_valid,
    input  logic                 fdi_tx_ready,
    output logic [FDI_PLP_W-1:0] fdi_tx_plp,

    output logic                 mc_req_valid,
    input  logic                 mc_req_ready,
    output logic                 mc_req_write,
    output logic [ADDR_W-1:0]    mc_req_addr,
    output logic [5:0]           mc_req_bytes,
    output logic [MC_DATA_W-1:0] mc_req_data,
    output logic [MC_MASK_W-1:0] mc_req_byte_mask,
    output logic [MC_TAG_W-1:0]  mc_req_tag,
    output logic [3:0]           mc_req_qos,

    input  logic                 mc_rsp_valid,
    output logic                 mc_rsp_ready,
    input  logic [MC_TAG_W-1:0]  mc_rsp_tag,
    input  logic [2:0]           mc_rsp_status,
    input  logic [MC_DATA_W-1:0] mc_rsp_data
);
```

首版默认参数：

| 参数 | 默认值 | 说明 |
|---|---:|---|
| FDI PLP | 250B | 10B protocol header + 240B payload |
| granule | 5B | 每个 Flit 共 48 个 granule |
| AoU data width | 256bit | 与当前正式 AXI256 链路一致 |
| MC transaction | 最大 32B | 与当前 HBM4 在线接口一致 |
| RP_COUNT | 1 | 支持 1..4；当前回归覆盖 1 和 2 |
| REQUEST_SLOTS | 4/RP | 与 `AouTarget` 当前容量一致 |
| WDATA_GRANULES | 64/RP | 与 `AouTarget` 当前容量一致 |
| MAX_OUTSTANDING | 8 | 父请求容量 |
| MC_INFLIGHT | 8 | 子事务在途容量，后续可独立调整 |

字节序必须在接口规范中固定：

```text
fdi_plp[8*i +: 8]     == C++ AouWireFlit[i]
mc_req_data[8*i +: 8] == 地址递增方向的第 i 个有效字节
```

`mc_req_*` 表示紧凑事务，不再携带 AXI lane：

```text
mc_req_addr       系统绝对字节地址；下游 transactor 做窗口检查并减 base
mc_req_bytes      本次有效字节数，范围 1..32
mc_req_data       从 bit[7:0] 起按地址递增紧凑排列
mc_req_byte_mask  每个有效字节对应一个掩码位
mc_req_tag        Bridge 分配的唯一子事务标识
```

例如地址对应 AoU 数据拍 lane 14 的单字节写：

```text
AoU WDATA[14] = 8'h11, WSTRB[14] = 1
    -> mc_req_data[7:0] = 8'h11
    -> mc_req_byte_mask[0] = 1
    -> mc_req_bytes = 1
```

## 3. RTL 模块拆分

接口基线中的目录：

```text
Bridge/rtl/
├── bridge_pkg.sv
├── storage_bridge_top.sv
├── fdi_rx_unpack.sv
├── aou_msg_decode.sv
├── bridge_credit_mgr.sv
├── bridge_req_buffer.sv
├── bridge_scheduler.sv
├── bridge_write_fsm.sv
├── bridge_read_fsm.sv
├── bridge_outstanding.sv
├── bridge_reorder_buffer.sv
├── bridge_response_gen.sv
├── fdi_tx_packer.sv
├── bridge_fifo.sv
└── bridge_assertions.sv
```

拆分进度（2026-09-22）：上述 15 个建议文件均已落地。`bridge_req_buffer`
通过每 RP 的 `bridge_fifo` 实例拥有入口队列；`bridge_scheduler` 独立完成
四组轮询选择；读写 FSM、outstanding、reorder、response generator 和 assertions
均为独立实例。另有 `storage_bridge_rx` 作为 RX 组合层，
`bridge_transaction_engine` 作为编排层，保留 parent 表、RP 内顺序队列、
credit 释放时机和单项 MC issue 寄存器。

模块职责：

| 模块 | 职责 | 当前 C++ 参考 |
|---|---|---|
| `fdi_rx_unpack` | 解析 250B PLP、MsgStart 和跨 Flit carry | `AouStreamDecoder` |
| `aou_msg_decode` | 解码请求、数据和 credit 消息 | `MsgDecoder` |
| `bridge_credit_mgr` | credit 消耗、归还、header/Misc 编码 | `CreditManager` |
| `bridge_req_buffer` | 每 RP 请求描述符和 WDATA FIFO | `writes_[]/reads_[]/data_[]` |
| `bridge_scheduler` | RP × 读写轮询仲裁 | `AouTarget::submit()` |
| `bridge_write_fsm` | 写 chunk 的地址/长度计算和 lane/mask 压紧 | `assemble()` + `MemSimBackend` |
| `bridge_read_fsm` | 组合计算读 burst 的下一笔边界事务 | `MemSimBackend` |
| `bridge_outstanding` | 分配 child tag，保存 parent/offset/bytes 映射并匹配完成 | `tickets_`/`children` |
| `bridge_reorder_buffer` | 将紧凑读数据散回原 beat/lane，保存数据和完成位 | `tickets_`/response data |
| `bridge_response_gen` | 把已选 parent 编码为 `ReadData`/`WriteResp` | `next_message()` |
| `fdi_tx_packer` | granule 装包、spill 和 credit 捎带 | `transmit()` |
| `bridge_assertions` | MC/response 稳定性、容量、事务形状和 tag 唯一性断言 | 现有 checker/scoreboard |
| `bridge_fifo` | Request/WData 共用的参数化 ready/valid FIFO | 固定深度队列 |
| `storage_bridge_rx` | 串接 FDI 解包和 AoU 消息译码 | RX 组合层 |
| `bridge_transaction_engine` | parent/order/issue 状态及所有事务子模块编排 | `AouTarget` 主控制流 |

## 4. 缓冲与流式实现

RTL 不为最长 256-beat burst 一次性分配动态缓冲。采用固定资源、流式推进：

```text
请求描述符保存 RP/ID/user/address/len/size/当前 beat
    -> WriteData FIFO 缓存有限个 beat
    -> W/R FSM 逐 beat 生成 mc_req
    -> 最多 MC_INFLIGHT 个子事务同时在途
    -> response 按 mc_tag 写回 reorder slot
    -> 父请求满足返回条件后生成 AoU 响应
```

当前实现按 32B MC 边界聚合或切分 AXI beat：多个相邻窄 beat 可合成一笔紧凑
transaction，首尾不足 32B 时生成残片；每笔请求均不跨 32B 边界。该逻辑与
`MemSimBackend::build_chunks()` 的事务数量已经在 `rtl-diff` 中对齐。未来
512/1024-bit 数据宽度仍需扩展并验证。

关键状态至少包括：

- 父请求表：`valid/write/rp/axi_id/user/address/beats/next_beat/status`；
- 子事务表：`valid/tag/parent_slot/offset/bytes/done`；
- 有界写数据 FIFO；
- 有界读响应数据槽；
- 每 RP 的请求与响应队列；
- RX carry 与 TX spill 寄存器；
- 五类 credit 的 RX capacity、pending return 和 TX available 计数。

## 5. 必须保持的协议规则

- 仅在 `valid && ready` 时完成传输；
- `valid && !ready` 时 payload 必须稳定；
- FDI 输入输出固定为 250B PLP，不含物理帧 scatter/gather；
- 写数据按 WriteReq 接收顺序配对，消息本身不携带 WLAST；
- 写数据进入已保证容量的存储位置后才归还 WriteData credit；
- 请求成功进入 MC 请求队列后才归还 WriteReq/ReadReq credit；
- 最后一段响应成功进入 FDI TX 后才释放父请求 outstanding；
- `mc_rsp` 可以乱序返回，必须按 tag 恢复 parent、offset 和 bytes；
- 写响应只能在全部写子事务完成后生成；
- 读响应必须生成恰好 `len + 1` 个 beat，正确设置 LAST；
- RP、ID 和同组完成顺序必须与当前 AoU 契约一致；
- 第一版只支持启动复位，热复位要求重建完整链路。

## 6. 连续实施阶段

| 阶段 | 实现内容 | 验收效果 | 预计新增规模 |
|---|---|---|---:|
| P0 | 接口规范、package、顶层空壳、lint | 端口、参数、字节序冻结 | 300–500 行 |
| P1 | FDI unpack/pack、单 Flit 消息 | C++ 生成 Flit，RTL 解码并可重编码 | 700–1000 行 |
| P2 | 跨 Flit carry、MsgStart、credit | 1024-bit 消息跨帧，credit 无溢出/欠账 | 700–1100 行 |
| P3 | W/R FSM、请求槽、lane/mask | 连接 mock MC 后读写逐字节一致 | 900–1400 行 |
| P4 | tag、outstanding、reorder | MC 乱序完成后仍正确回包 | 700–1100 行 |
| P5 | mem_sim transactor | RTL 请求真实进入 mem_sim 并返回 | C++ 400–700 行 |
| P6 | UCIe 全链路替换 | 现有用例、压力和重放回归通过 | TB 800–1500 行 |
| P7 | gem5/XPU 接入 | CPU/GPU/NPU 请求经过 RTL Bridge | 集成 300–600 行 |

预估总规模：

```text
RTL                    4,000–6,000 行
testbench / SVA        1,500–3,000 行
SystemC/C++ wrapper      600–1,000 行
构建、脚本、文档          300–600 行
合计                    6,400–10,600 行
```

单人完成可运行联合仿真的工程量预估为 3–5 周，不包含综合时序、面积/功耗收敛和
真实内存控制器 IP 对接。

## 7. 验证策略

现有 C++ `AouTarget + MemSimBackend` 保留为黄金参考。同一组 FDI 输入分别送入：

```text
C++ AouTarget + MemSimBackend
RTL StorageBridge + MemSimTransactor
```

逐项比较：

- MC 请求的地址、方向、bytes、data、byte mask、QoS；
- credit 消耗和归还；
- `WriteResp`/`ReadData` 的 RP、ID、状态、数据和 LAST；
- 子事务与父 burst 的 tag、offset 和完成关系；
- 最终 mem_sim MemoryImage；
- 请求、响应、错误、stall 和 outstanding 计数；
- 仿真结束时全部 FIFO、tag 和 outstanding 排空。

必要断言：

```text
valid && !ready -> payload stable
credit never underflows or exceeds configured capacity
tag cannot be reused before response retirement
response must have a live matching request
write response requires every child transaction complete
read response beat count equals len + 1
outstanding <= MAX_OUTSTANDING
MC inflight <= MC_INFLIGHT
reset leaves all valid, queue and credit state deterministic
```

回归分层：

1. RTL 单模块定向测试；
2. AoU C++ 编解码器与 RTL 的向量对拍；
3. mock MC 随机反压和乱序响应；
4. mem_sim 在线 C ABI 联仿；
5. AXI2Flit/UCIe 全链路回归；
6. gem5 CPU 和 XPU 场景回归。

## 8. 联合仿真接缝

上游需要一个 FDI 信号 transactor：

```text
sc_fifo<FdiFlit>
    <-> fdi_rx/fdi_tx valid-ready + 2000-bit PLP
```

下游需要一个 mem_sim transactor：

```text
mc_req valid-ready
    -> ss_mem_submit()
仿真时间推进
    -> ss_mem_step()
ss_mem_pop()
    -> mc_rsp valid-ready
```

地址本地化和映射由下游 transactor/内存控制器完成：先检查 `mc_req_addr` 是否位于
`[base, base+size)`，再减 base，并由 mem_sim `AddressMapper` 解码。Bridge 不把
`DecodedAddress` 固化为 RTL 接口，可在 trace 中记录解码坐标用于等价对拍。

当前独立 P6 联仿使用 Verilator `--cc` 生成 C++ 模型，由 `RtlBridgeSystemC` 在同一
SystemC 进程内驱动 `eval()`；每个 2ns Bridge 周期推进 8 个 250ps mem_sim tick。
gem5 原生接入由 `gem5_axi::RtlAouTarget` 完成：它把 RTL 的 1..32B 紧凑 MC
valid-ready 请求转换成 `SimpleMemRequest`；2 次幂长度保持原地址/长度以便日志对拍，
12B/20B/24B 等 AxSIZE 无法表示的 chunk 才装入对齐的 32B 容器，并用 lane/strobe
保留真实有效区间。wrapper 用 1..1023 的宿主 ID 表映射 32bit RTL tag，读响应从
对应 lane 中压紧后送回 RTL。这样能承载非 2 次幂 chunk，又不改变复用的 AXI 形态
`SimpleMemRequest` 契约。原 `AouTarget` 和
`MemSimBackend` 均保留；运行时只替换 Target，并复用 MemSimBackend 的窗口检查、
粒度拆分、C ABI 驱动和日志。Verilator 仅生成普通 C++，由 gem5 SCons 使用同一
工具链编译并链接 gem5 原生 SystemC，不引入第二套 SystemC 内核。

建议构建入口：

```text
make -C Bridge rtl-lint
make -C Bridge rtl-test
make -C Bridge rtl-diff
make -C Bridge rtl-full-link
make -C Bridge rtl-model-src
bash env/build.sh
gem5/build/AXI/gem5.opt --outdir=results/rtl-gem5 \
  --listener-mode=off gem5_axi/configs/run.py --mode tester \
  --backend aou --bridge-impl rtl --memory-backend memsim --planes 1
```

## 9. 已达到的效果

P0-P4 已完成：Bridge RTL 可在 mock MC 环境中独立完成 AoU 请求到内存事务、再到 AoU
响应的闭环，并具备反压、credit 和乱序完成能力。

P5-P6 已完成：请求可通过真实 UCIe 行为链路进入 RTL Bridge，再由 transactor 访问在线
mem_sim；现有 C++ `AouTarget` 已退出 RTL 运行路径，但继续作为黄金参考。

P7 的 gem5 tester/CPU/XPU 接入已经完成：存储请求可在单一 gem5/SystemC 进程内经过 RTL
Bridge 到达 mem_sim，并将真实内存完成时间沿原链路反馈给请求源。gem5 专用 Verilator
模型按 RP_COUNT=4 生成，wrapper 按运行时 `planes` 使用 1..4 个 RP。2026-09-22 已用
`BRIDGE_IMPL=rtl` 完成双 RP NPU、GPU、三源和慢时标四组验收。当前仍属于 RTL Bridge +
行为级 UCIe/mem_sim 的联合仿真，不能宣称
已经包含真实 MC、DFI 或 PHY RTL。

## 10. 待后续确认项

- 下游 MC transaction 宽度是否长期固定为 256bit；
- RP_COUNT=4 的资源规模和系统级回归；
- MC 响应状态编码及其到 AoU RESP 的映射；
- QoS 是否直接使用 AoU AxQOS，还是增加 RP 到 QoS 的策略；
- parent outstanding 和 child inflight 是否分别保持 8；
- 是否需要配置寄存器承载 base/size，而不是编译期参数；
- 是否需要支持运行期 link down/degraded 和热复位恢复；
- 未来真实内存控制器接口是否继续使用该 req/rsp stream。

其中顶层 MC req/rsp 接口已经冻结；其余项目若发生变化，需要先更新本方案和接口回归，
因为它们会影响缓冲结构、tag 格式或下游兼容性。
