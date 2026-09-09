# UCIe 全链路仿真使用说明

本测试将 AXI 请求经桥、双向链路及响应端送至字节存储，并检查返回的状态和数据。实现位于 `systemc/tb/tb_full_link.cpp`，外部接口详见[设计文档](../systemc/doc/design.md)。

## 拓扑

```text
AXI BFM ⇄ Axi2Flit ⇄ UcieAouEndpoint ⇄ UcieLink ⇄ AouTarget ⇄ SimpleBurstMemory
         AXI信号      Flit握手          FdiFlit FIFO          请求/响应FIFO
```

Target 直接连接 UCIe 的 mem_rx/mem_tx FIFO，无需额外的信号转 FIFO 适配器。
请求 FIFO 写入成功仅表示内存接受排队；写入实际执行后才产生 B 对应的响应。
AXI 读写间无隐含全局顺序；需要写后读依赖的测试必须先等待 B。

## 容量设计

每 RP 的 WREQ 数限制可同时组装的写 burst 数；每个 WREQ 为最大 256 beat
预留一个有限组装槽。WDATA 消息搬入已预留的组装槽后归还消息 FIFO 的 credit，
避免长 burst 大于 WDATA credit 窗口时死锁。WREQ credit 在整笔请求提交后归还。
读请求在提交后归还 RREQ credit。提交数还受全局 outstanding 限制，响应缓存
最多保存相同数量的整 burst；响应序列化完成才释放 outstanding 槽。
同 ID 同方向按 RP 内接收顺序返回；同 ID 未完成期间跨 RP 仍遵守桥的顺序约束。

## 精度与边界

这是时钟驱动桥/Target、事件驱动 UCIe、事务级串行存储的联合行为仿真。
内存模型一次处理一个 burst，延迟由固定访问时间与每 beat 时间组成；不是
流水线 HBM 的性能预测。FH 保存序号与重放标志，CRC 使用分半帧的 CRC16-CCITT 校验，详见[字节格式](../systemc/doc/wire_contract.md)。
全链路协调启动，不支持在 UcieLink 内有事务时单端热复位。独占/原子语义不实现。

## 接口与实现

| 模块 | 实现 | 端口/作用 |
|---|---|---|
| AXI 事务 BFM、自检与顶层 | `systemc/tb/tb_full_link.cpp` | AW/W/AR 请求、B/R 响应；500MHz，低有效复位 |
| 请求发起侧桥 | `systemc/include/axi2flit.h` | AXI 请求与消息转换、响应恢复 |
| SoC 侧适配器 | `systemc/integration/ucie_aou_endpoint.h` | Flit ready/valid ↔ 250B FDI FIFO |
| 双向 UCIe | `reference/ucie-model/src/ucie_link.h` | PHY、CRC、序号、ACK/NAK、重放 |
| 存储侧响应端 | `systemc/integration/aou_target.h` | `link_rx/link_tx`、`mem_req/mem_rsp` |
| 简单内存 | `systemc/integration/simple_burst_memory.h` | `request/response` FIFO，单请求串行服务 |

顶层创建四个 `sc_fifo<FdiFlit>`，连接：

```text
Endpoint.fifo_tx  → soc_tx → UcieLink.soc_tx_in
Endpoint.fifo_rx  ← soc_rx ← UcieLink.soc_rx_out
Target.link_rx    ← mem_rx ← UcieLink.mem_rx_out
Target.link_tx    → mem_tx → UcieLink.mem_tx_in
Target.mem_req    → requests → Memory.request
Target.mem_rsp    ← responses ← Memory.response
```

`SimpleMemRequest`：write、rp、address（id/addr/len/size/prot/cache/qos/lock/user）和
write_beats（data/strobe/user）。`SimpleMemResponse`：write、rp、id、写 resp/user，或
read_beats（逐 beat data/resp/user）。读请求无写数据，写响应无读数据。
每次成功 FIFO 写入只交付一次；消费者没空间时保留原事务。Target 自己不主动
访问 SoC 内存；其发送方向仍会主动发布初始 credit 和缓冲释放后的 credit。

内存地址窗口为 **0x1234567800000000 起的 128KiB**，启动全零；默认访问时间
`20ns + 2ns × beat数`，stress 模式为 `80ns + 2ns × beat数`。数据和 strobe
保留 AXI lane 顺序，窄访问的 lane 随地址移动；窄读无效 lane 返回零（本模型约定）。
越界整笔返回 DECERR=3，不产生部分写副作用；不支持的 LOCK 返回 SLVERR=2。
USER 采用地址 USER 回送 BUSER/RUSER 的测试约定，不能视为所有内存控制器的通用要求。

AXI 请求仍须 INCR、按 SIZE 对齐、SIZE 不超过总线宽度且不跨 4KB，违例由桥
fail-fast。全宽单 burst 最大 beat 数因 4KB 约束为 128/64/32；256 beat 用例采用
4B 窄访问，因此合法。RP0～RP3 均为同一存储窗口的资源隔离通路，不是四份内存。

## 如何运行

在项目根目录执行，依赖安装及 `SYSTEMC_HOME` 设置见[接入说明](../systemc/integration/README.md)：

```bash
# 默认256bit、RP1、无误码。终端直接显示逐组结果。
make full-link

# 指定数据宽度、资源平面和压力/错误注入。
make WIDTH=1024 FULL_LINK_ARGS="--rp 2 --stress --replay" full-link

# 256/512/1024 × RP1/2/4 无误码，以及每个位宽的 RP2 压力+重放，共12种配置。
# 每种配置重新运行全部12组用例；生成独立日志，任一失败立即返回非零。
make full-link-all

# 有意破坏一处参考读数据；只有观察到数据不匹配且仿真失败，控制测试才通过。
make full-link-negative

# 从已有 VCD 生成无需联网、无需第三方 Python 包的浏览器波形页。
make full-link-wave
make WIDTH=1024 FULL_LINK_VCD=sim/full_link_1024_rp2_stress_replay.vcd full-link-wave
```

产物在 `systemc/sim/`，名称包含位宽、RP 和场景，与桥功能测试的 `waveform.vcd` 分开保存：

```text
full_link_256_rp1.vcd              原始逐信号波形
full_link_256_rp1.csv              仅记录真实 AXI 握手的事件日志
full_link_256_rp1.log              full-link-all 保存的文本日志
full_link_256_rp1.html             full-link-wave 生成的交互查看页
full_link_1024_rp2_stress_replay.*  压力+重放配置的独立产物
```

CSV 数据按总线十六进制表示，最右侧为 lane0；`strb_bits` 为从高 lane 到低 lane
的 0/1 字符串。R 行的地址与 beat 索引从已接收的 AR 推导，不是 R 通道额外引脚。
B 行地址为0，因为 B 通道只携带 ID/状态；通过 ID 关联 AW。

`preflight` 汇总桥和链路组件回归；`full-link-all` 运行完整链路功能回归，两者分别执行。

独立交付后可用 `make UCIE_DIR=/新路径/ucie-model full-link-all` 更换链路位置，
无需修改 C++ 源码。相对路径以 `systemc/` 为基准；补丁检查位置和 UCIe 所需的
`aou_format6.h` 路径由构建入口自动定位，细节见[接入说明](../systemc/integration/README.md)。

## 性能结果的适用范围

`make -C systemc report` 生成 `systemc/sim/perf_report.txt`，使用简化链路
`LinkPacer + FlitDelayLine + RemoteAouModel`。1024bit 桥基线读/写吞吐为
42.66/42.40 GB/s，口径B为94.79%/94.22%，通过现有门限；这些不是全链路
测试的持续吞吐结果。`full-link-all` 报告功能计数、错误/重放和背压覆盖，
不进行稳态带宽门限考核。

可从 AXI CSV 测量实际事务延迟。例如默认256bit/RP1的TC2，AW→B为52ns、
最后W→B为48ns、AR→R为48ns，均包含链路、适配器、Target和内存服务时间。
不能把端到端延迟与桥内 TX/RX 延迟直接比较，也不能把带主动等待/背压的
功能用例总字节数除以总时长，当成链路最大吞吐。

联合性能测量需要独立的持续读/写/混合流量、预热和统计窗口、可配置outstanding，
并分别报告AXI有效字节吞吐、协议载荷效率、桥内延迟和端到端延迟。简单内存
一次串行处理一个burst，会限制结果；例如1024bit/16beat的纯内存服务上限为
2048B÷(20ns+16×2ns)≈39.38GB/s（尚未计入其他开销）。若用此配置要求全链路
达到桥基线42GB/s，会被内存服务能力限制，需要另设足够快的性能后端或明确系统目标。

## 用例与验收点

| TC | 激励 | 自检内容 |
|---|---|---|
| 1 | 未写地址单拍读 | 零初始化 |
| 2 | 单拍写后读 | 完整数据、ID、USER、响应、LAST |
| 3 | 2/4/16 beat 与全宽4KB最大 burst | beat数、跨 Flit 处理、逐字节读回 |
| 4 | 初始写、部分strobe覆盖、全零strobe覆盖 | 被屏蔽字节保持原值 |
| 5 | 地址偏移4B的窄 burst | lane旋转、部分掩码、全宽回读 |
| 6 | 256 beat × 4B | 写组装与credit窗口循环，无长burst死锁 |
| 7 | 最后一字节恰好在4KB末尾 | 合法边界读写 |
| 8 | 窗口外读写、高32bit不同但低地址相同 | DECERR、无有效地址别名破坏 |
| 9 | 多AW排队、独立读写重叠、跨RP | AW/W配对，多笔outstanding及ID关联 |
| 10 | 同ID连续读写 | 同方向同ID响应顺序 |
| 11 | 长时间BREADY/RREADY=0，随后随机ready | 背压稳定性、credit耗尽与恢复、无重复 |
| 12 | 固定随机种子的合法burst/strobe/发送间隔 | 可复现的组合覆盖 |

无误码配置关闭噪声、抖动、ISI尾项和skew。`--replay` 启用固定seed=7的2%额外
Flit误码注入；`--stress` 将四个公开FDI FIFO和两个内存FIFO深度缩为1，retry
buffer设为8，反馈延迟设为256 UI，并增加内存延迟/随机响应背压。
错误注入的验收条件为**确实发生CRC错误与重放，同时AXI数据仍全部正确**；
不能要求该场景CRC计数为0。默认无误码场景则必须CRC错误为0。

scoreboard只根据SoC侧AW/W握手更新独立字节地址参考表，按AR握手建立期望读数据。
它不读取目标内存内容、不复用消息编解码器计算期望。所有五通道均检查
valid被背压时的数据稳定性；额外检查W beat数、WLAST、R beat数、RLAST、
B/R ID/USER/RESP及逐字节数据。结束检查AXI、Target、Memory事务数一致，
并等待链路排空后观察一段静默期，捕获迟到的重复响应。

## 如何判读结果和波形

完整运行成功应出现：

```text
[PASS] TC1 ...
...
[PASS] TC12 ...
FULL_LINK: 12 cases / 0 errors
ALL FULL-LINK TESTS PASSED
```

任意数据/协议检查失败或等待超时返回非零。仅看到某个TC的PASS不够，还须看
最终汇总；最终覆盖/计数检查可能在全部用例结束后发现错误。日志中的
`Simulation stopped by user` 是 SystemC 对测试程序调用 `sc_stop()` 的常规提示，
表示仿真由测试程序正常结束。

浏览器直接打开HTML，默认显示TC2；可选择用例、输入起始时间/跨度、缩放平移，
悬停总线区间查看完整值。HTML保留64bit地址和1024bit数据的十六进制字符串，
不经过JavaScript浮点数转换。也可使用GTKWave查看原始VCD：

```bash
gtkwave systemc/sim/full_link_256_rp1.vcd
```

在GTKWave的`SystemC/axi`中加入awvalid/awready/awaddr/awlen、wvalid/wready/wdata/
wstrb/wlast、bvalid/bready/bid/bresp、arvalid/arready/araddr/arlen以及rvalid/rready/
rdata/rid/rlast/rresp；再加入clk、rst_n、testcase。总线选择十六进制。
结构体的C++字段容器可能宽于逻辑AXI字段（例如ID用16bit容器，但有效ID为10bit）。

建议先看TC2：AW握手一次，W握手一次且WLAST=1；随后BVALID/BREADY握手，BRESP=0；
之后AR握手一次，R握手一次且RLAST=1，RDATA与先前WDATA相同。再看TC3：
握手次数等于LEN+1，只有最后一拍LAST=1。最后看TC11：VALID=1且READY=0的
多个周期里数据/ID/LAST不变，READY恢复后的上升沿只接收一次。
计数必须按**上升沿上的VALID&&READY**，不能按VALID高电平持续时间或跳变次数。

## 默认回归结果

12种配置 × 12组用例全部通过，合计144组配置内用例；每配置55笔写、66笔读。
默认256bit/RP1：AW=55、W=1219、B=55、AR=66、R=1241；CRC错误为0。
三组RP2压力+重放分别观察到：

| AXI位宽 | CRC失败帧数 | 重发Flit数 | 最终AXI错误 |
|---|---:|---:|---:|
| 256 | 73 | 243 | 0 |
| 512 | 76 | 237 | 0 |
| 1024 | 81 | 236 | 0 |

256bit ReadData占8 granule，整除48 granule数据区，因此该配置不强制产生跨Flit。
512/1024bit压力配置检查确实出现响应跨Flit，分别观察到148/372次。
负向scoreboard控制测试已确认能够报告数据错误并返回失败。

这些结果覆盖行为链路与简单存储的事务闭环，不覆盖多随机种子长时间运行、热复位、DRAM 引脚时序、独占/原子执行和 RTL 时序。接入其他主机或后端后的接口要求见[设计文档](../systemc/doc/design.md)，性能判据见[验证文档](../systemc/doc/verification.md)。
