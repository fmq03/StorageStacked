# UCIe 高速互联模型：需求、建模方法与验证说明

> 文档日期：2026-08-25
> 适用目录：`ucie_systemc/`
> 实现语言：C++17 + SystemC 2.3.4
> 对应交付范围：研究成果 3——UCIe 高速互联 SystemC 行为级仿真模型

## 1. 文档目的与结论

本文说明当前 UCIe 高速互联仿真模型的：

- 项目需求与当前模型之间的对应关系；
- 端到端系统框架和 `ucie_systemc` 内部模块框架；
- FDI、D2D Adapter、Flit、PHY、PAM4/NRZ、信道、CDR、重传等功能的建模方法；
- 时间、带宽、延迟、误码率和利用率的计算方法；
- 现有功能测试与参数扫描如何验证模型；
- 已验证内容、尚未覆盖内容以及模型适用边界。

当前模型的定位是**系统级/行为级性能模型**。它能够研究速率、lane 数、Flit 格式、
信道噪声、ISI、抖动、lane skew、反馈延迟和重传缓存深度对吞吐、延迟及可靠性的影响，
但不能替代 SPICE、IBIS-AMI、S 参数、眼图、浴盆曲线或 UCIe 合规性测试。

截至本文日期，`scripts/run_tests.sh` 已重新执行并得到：

```text
UNIT_PASS=22 UNIT_FAIL=0
PASS=82 FAIL=0
```

这个结果证明主要软件路径能够按当前模型定义工作，但不等于已经证明模型与真实芯片的
物理误码特性完全一致。

## 2. 项目总体需求与范围

### 2.1 项目级端到端目标

项目目标是建立从计算侧访存请求到堆叠存储阵列的全链路模型：

```text
CPU/GPU/NPU
    │ 访存请求
    ▼
GEM5 SoC/MMU/Cache
    │ AXI
    ▼
AXI2Flit 协议转换
    │ FDI / Flit 流
    ▼
UCIe D2D Adapter + PHY              ← 本文重点
    │ Flit 流
    ▼
Flit2DFI / Protocol Bridge
    │ 原子化存储命令
    ▼
Memory Controller + MEM PHY
    │ DRAM 时序命令
    ▼
DRAMsim3 / Ramulator / 堆叠存储阵列
```

`ucie_systemc` 只直接实现和验证上图中的 UCIe 高速互联部分，并为前后的协议转换、
桥接和存储模型提供可对接的 Flit/FDI 抽象。全链路指标必须在各子系统联合后再次测量。

### 2.2 三项核心技术指标追踪

| 技术指标                             | 当前模型中的对应方法                                    | 当前证据                                                   | 结论                                                       |
| ------------------------------------ | ------------------------------------------------------- | ---------------------------------------------------------- | ---------------------------------------------------------- |
| 单通道速率支持 24 GT/s 或更高        | `--rate-gtps` 配置每 lane 速率，UI 按 `1/rate` 换算 | T2 验证 24 GT/s；T9 验证 32 GT/s；sweep 覆盖 24/32/48 GT/s | 链路模型层面已覆盖                                         |
| 全链路对存储阵列带宽利用率不低于 90% | 统计有效 payload 吞吐并除以物理层原始容量               | 干净链路约 92.18%                                          | 仅证明 UCIe 链路段；不能单独代表“到存储阵列”的全链路结果 |
| 协议桥接单元引入延迟不超过 20 ns     | 应从 AXI 输入时间戳测到 DFI/存储命令输出时间戳          | 当前模型统计 FDI 请求、echo response 与链路 round-trip，不含真实 Bridge/MC | 需与 AXI2Flit、Flit2DFI/Bridge RTL 联合验证                |

因此，正式报告中应使用“UCIe 链路侧利用率达到约 92.18%”，不能把它直接写成
“整个存储访问全链路已经达到 92.18%”。

## 3. `ucie_systemc` 内部框架

### 3.1 模块拓扑

模型包含彼此独立的请求和响应业务方向：

```text
请求方向：
FdiSource -> FWD TxAdapter -> FWD PhyChannel -> FWD RxAdapter -> MemoryResponder
                  ^                                  |
                  +--------- FWD ACK/NAK -------------+

响应方向：
MemoryResponder -> REV TxAdapter -> REV PhyChannel -> REV RxAdapter -> FdiSink
                       ^                                  |
                       +--------- REV ACK/NAK -------------+
```

顶层模块实例化和连接位于 [`src/ucie_systemc_main.cpp`](src/ucie_systemc_main.cpp)。主要模块职责如下：

| 模块 | 建模职责 |
| --- | --- |
| `FdiSource` | 生成内部 Payload 或注入外部文件 Payload，控制注入速率并响应 FDI FIFO 背压 |
| `TxAdapter` | 接收真实 FDI Payload、分配序号、构造 Flit、Retry Buffer、ACK/NAK 和 timeout replay |
| `PhyChannel` | lane striping、PAM4/NRZ、ISI/AWGN/jitter、skew/deskew、CDR 和流水线延迟 |
| `RxAdapter` | CRC/序号检查、重复包丢弃、NAK 抑制和按序交付 |
| `FeedbackPath` | 无误码、固定延迟、可流水化的 ACK/NAK 返回路径 |
| `MemoryResponder` | 检查请求，加入可配置存储延迟并生成 echo response |
| `FdiSink` | 检查响应 transaction ID、VC、Payload 和顺序，记录反向及 round-trip 延迟 |
| `Watchdog` | 超过 `max_time_ui` 时停止仿真 |

`TxAdapter + PhyChannel + RxAdapter + FeedbackPath` 被实例化两组。两组分别维护序号、Retry Buffer、ACK/NAK、CDR 和错误统计，因此业务 response 不会与链路 ACK/NAK 混用。

### 3.2 SystemC 时间模型

SystemC 时间分辨率为 1 fs，模型以 UI 为主要时间单位：

$$
T_{UI}=\frac{1}{R_{lane}}
$$

24/32/48 GT/s 分别对应约 41.667/31.250/20.833 ps。所有 TX、信道、RX、反馈、训练、存储响应和 CDR 重锁时间均先以 UI 配置，再映射到 SystemC 时间轴。

### 3.3 Request/Response 数据流

1. `FdiSource` 产生包含 Payload、transaction ID、VC 和时间戳的 `FdiFlit` 请求。
2. 正向 `TxAdapter` 添加链路 Header/CRC，保存 Retry Buffer 副本并串行化发送。
3. 正向 PHY 施加信道行为；正向 RX 检查 CRC/序号，并生成本方向 ACK/NAK。
4. 正确请求到达 `MemoryResponder`，经过 `memory_delay_ui` 后形成业务 response。
5. response 经独立的反向 TX/PHY/RX 和反向 ACK/NAK 机制返回。
6. `FdiSink` 检查响应 ID、VC、顺序和 Payload，记录反向 hop 与完整 round-trip 延迟。

### 3.4 当前输入、输出与反馈接口

当前接口是 SystemC 事务级 `sc_fifo`，不是 RTL 逐信号 FDI。独立 `UcieLink` 顶层的
`soc_tx_in/soc_rx_out/mem_tx_in/mem_rx_out` 四个数据端口统一使用：

```cpp
struct FdiFlit {
    std::vector<std::uint8_t> payload;
    std::size_t valid_bytes;
    std::uint64_t transaction_id;
    std::uint8_t vc;
    BusinessKind kind;
    sc_time transaction_start;
    sc_time fdi_time;
};
```

Payload 由 producer 提供。默认 producer 生成确定性数据；指定 `--payload-file` 时读取外部实际字节。TX Adapter 不会根据 ID 替换 Payload，而是添加链路 Header 和 CRC。

真正经过 PAM4/NRZ 和信道损伤的是 `Frame.bytes`。transaction ID、VC、方向和时间戳仍是行为级事务元数据；信号级对接时，上层协议应将必要字段编码进 Payload，或由 transactor 显式映射。

每个业务方向各有一组内部 `FbMsg {is_nak, seq}`，只用于该方向链路可靠性。业务 response 使用反向 `FdiFlit` 数据路径，内部 `Frame` 不跨越 `UcieLink` 边界。模块另输出 `link_state`，编码 Reset/Training/Active/Degraded/Failed。详细契约见 [`INTERFACE.md`](INTERFACE.md)。

## 4. 各项功能的建模方法

### 4.1 FDI 流式注入与背压

`FdiSource` 不是一次性把全部请求塞入链路，而是按照固定间隔生成请求。默认
`fdi_interval_ui=0` 表示自动取一个 Flit 的串行化时间作为注入间隔：

$$
T_{inject}=T_{serialize}
$$

FDI FIFO 默认深度为 64。队列满时，Source 等待 `data_read_event()`，从而把下游拥塞
反馈为上游停止注入。这是流量控制和背压的 SystemC 行为级表达。

初始时 Source 与 TX 并行等待 `train_ui`，因此训练延迟只发生一次，不会叠加两遍。

### 4.2 Flit 格式与有效载荷效率

模型支持两种格式。

#### 4.2.1 Standard 256B

| 字节范围       |  长度 | 内容                      |
| -------------- | ----: | ------------------------- |
| `[0]`        |   1 B | 序号低 8 bit              |
| `[1]`        |   1 B | 标志位，bit 0 表示 replay |
| `[2..237]`   | 236 B | TLP payload               |
| `[238..241]` |   4 B | DLLP，占位为 0            |
| `[242..251]` |  10 B | Reserved，占位为 0        |
| `[252..253]` |   2 B | 前半组 CRC-16             |
| `[254..255]` |   2 B | 后半组 CRC-16             |

有效载荷效率为：

$$
\eta_{flit}=\frac{236}{256}=92.1875\%
$$

这也是正常满载条件下链路利用率接近 92.18% 的直接原因。

#### 4.2.2 Compact 68B

```text
2B header + 64B payload + 2B CRC = 68B
```

其 payload 效率为：

$$
\eta_{flit}=\frac{64}{68}=94.1176\%
$$

该格式用于检查不同 Flit 尺寸对串行化、反馈占比和吞吐的影响，不应自动视为标准
UCIe 256B Flit 的替代格式。

### 4.3 Payload 与端到端数据黄金参考

内部模式根据 transaction ID 和固定的 `splitmix64` 序列生成确定性 Payload；外部模式从
`--payload-file` 读取调用者提供的实际字节。黄金参考在仿真开始前保存，MemoryResponder
和 SoC Sink 分别检查正向请求与反向响应的 transaction ID、VC、有效长度、顺序和 Payload。

这样可以发现：

- CRC 没有拦截的残余数据错误；
- 错误重传导致的乱序；
- 重复交付或漏交付；
- Flit 构造、解构过程中的数据损坏。

任何异常都会累加到 `integrity_errors`，同时区分 forward/reverse。仿真最终只有在全部业务响应交付且
`integrity_errors==0` 时返回成功。

### 4.4 CRC 建模

模型使用 CRC-16/CCITT：

```text
多项式：0x1021
初始值：0xFFFF
```

Standard 256B 分两组计算：

- Group A：bytes `[0..127]`；
- Group B：bytes `[128..251]`。

Compact 68B 对 bytes `[0..65]` 计算一个 CRC。RX 重新计算并与 Flit 尾部字段比较。

单元测试已加入独立 CRC-16/CCITT-FALSE 黄金值 `123456789 -> 0x29B1`，并对两种 Flit
执行固定 bit 翻转检测；系统回归再验证随机错误能够触发 NAK/replay。该证据验证当前行为
模型的算法和覆盖路径，但仍不等同于正式 UCIe 合规签核。

### 4.5 TX D2D Adapter

#### 4.5.1 序号与 Retry Buffer

TX 为每个新 Flit 分配内部 64-bit 单调序号，发送前保存：

```text
seq -> {payload, valid_bytes, transaction_id, vc, kind, timestamps}
```

内部序号可以持续增长：

```text
0, 1, 2, ..., 254, 255, 256, 257, ...
```

当前模型把Flit的第0字节定义为序号字段，因此真正放入 `Frame.bytes` 并经过PHY传输的
只有内部序号低8 bit：

```cpp
b[0] = static_cast<std::uint8_t>(seq & 0xFFU);
```

即：

$$
seq_{wire}=seq_{internal}\bmod256
$$

| 内部序号 | 线上8-bit序号 |
| -------: | ------------: |
|      254 |           254 |
|      255 |           255 |
|      256 |             0 |
|      257 |             1 |
|      512 |             0 |

这里的8 bit是当前简化Flit Header的设计选择，不是由PAM4、lane数或256B Flit自动推导
出来的，也不应直接表述成所有UCIe模式都必须使用该字段宽度。

Retry Buffer 默认深度64，并被限制为不超过127。8-bit序号空间共有256个位置，如果允许
未确认窗口达到128，接收端看到相差128的序号时，无法判断它是“向前128个的新帧”还是
“向后128个的旧帧”。将窗口限制为小于半个序号空间后，可以采用：

```text
diff = 0       ：当前期望帧
diff = 1～127  ：未来/超前帧
diff = 128～255：按当前实现视为旧帧
```

其中 `diff=128` 在数学上仍是半圈歧义点；由于合法未确认窗口最大只有127，正常新帧不应
出现在这个距离，当前实现把它归入旧帧分支。限制窗口的目的正是让正常业务避开该歧义点。

当 Retry Buffer 满时，TX 不再读取 FDI FIFO，并累计：

- `retry_buffer_full_events`；
- `max_retry_buffer_occupancy`。

#### 4.5.2 TX 状态抽象

TX 定义四个状态：

```text
Idle
  ├── 收到 ACK ──> AckReceived ──> Idle
  ├── 收到 NAK ──> NakReceived ──> NakInProgress ──> Idle
  └── Replay Timer 超时 ─────────> NakInProgress ──> Idle
```

状态主要用于表达事件阶段；真正决定发送行为的是 `replay_active_`、
`replay_cursor_` 和 `replay_end_`。

#### 4.5.3 累计 ACK

收到 `ACK(n)` 后，TX 删除 Retry Buffer 中所有满足：

$$
seq \le n
$$

的条目，因此反馈采用累计确认语义。

#### 4.5.4 NAK 与区间重传

收到 `NAK(n)` 后，TX 使用 `lower_bound(n)` 在 Retry Buffer 中寻找第一个序号大于或
等于 `n` 的未确认 Flit。若找到，则从该序号开始重传：

```text
[第一个不小于n的未确认序号, 当前Retry Buffer最大序号]
```

这是一种 Go-Back-N 风格的区间重传。只有当 Retry Buffer 中不存在任何序号大于或等于
`n` 的条目，即 `lower_bound(n)==end` 时，才增加 `stale_nak_count`。因此，即使 `n`
本身已经被累计 ACK 释放，只要缓存中仍有更大的未确认序号，当前实现仍会从第一个更大
序号开始重传。

例如Retry Buffer保存 `[10,11,12,13,14,15]`。RX正确收到10和11，但12发生CRC错误；
随后到达的13～15虽然可能CRC正确，因为RX没有乱序缓存，也不能越过12交付。RX发送
`NAK(12)` 后，TX重传 `[12,13,14,15]`，而不是只重传12。

如果Retry Buffer为 `[12,13,14,15]`，不同NAK的实际结果为：

| 反馈        | `lower_bound`结果 | 动作                          |
| ----------- | ------------------: | ----------------------------- |
| `NAK(12)` |                  12 | 重传12～15                    |
| `NAK(13)` |                  13 | 重传13～15                    |
| `NAK(11)` |                  12 | 重传12～15                    |
| `NAK(16)` |             `end` | 不重传，`stale_nak_count++` |

#### 4.5.5 Replay Timer

若仍有未确认数据且长时间没有反馈或发送进展，TX 自动重放整个未确认窗口：

$$
T_{replay}=4\times
(T_{serialize}+T_{tx}+T_{channel}+T_{rx}+T_{feedback}+64\,UI)
$$

该机制保证重传 Flit 再次损坏、且后续没有新流量时仍能继续推进，属于活性保护模型。

`last_progress_`会在发送新Flit、发送Replay Flit、收到ACK或收到NAK时更新，所以这些
事件都会刷新超时计时基准。只有在Retry Buffer非空、没有正在重传，并且距离最后一次
发送或反馈超过Replay Timeout时，才重放整个未确认窗口。

Replay Timer触发的是**重传**，不是链路复位。它不会：

- 清空Retry Buffer；
- 把序号归零；
- 重新执行CDR训练；
- 重置整个TX/RX状态机。

默认256B、16-lane PAM4时，`serialize=64 UI`，因此：

$$
T_{replay}=4\times(64+4+8+4+32+64)=704\,UI
$$

24 GT/s下约为29.33 ns。若此时缓存中为 `[100,101,102,103]`，超时动作就是依次重新发送
100～103，等待后续累计ACK释放这些条目。

### 4.6 串并转换与 lane striping

Flit 串行化时间按下式计算：

$$
N_{serialize,UI}=
\left\lceil
\frac{N_{flit,bits}}
{N_{lanes}\times bits\_per\_UI}
\right\rceil
$$

UI是Unit Interval，即发送一个符号占用的时间：

$$
T_{UI}=\frac{1}{R_{symbol}}
$$

PAM4一个符号有4种电平，可以表达 `00/01/11/10`，所以一个UI发送一个PAM4符号并携带
2 bit；NRZ一个符号只有2种电平，所以一个UI携带1 bit。

当前模型把 `--rate-gtps 24` 当作每秒24G个UI/符号。因此：

```text
24G UI/s × 2 bit/UI = 48 Gbit/s/lane（PAM4）
24G UI/s × 1 bit/UI = 24 Gbit/s/lane（NRZ）
```

严格工程表述中，PAM4的符号速率写成24 GBaud更不容易与bit rate混淆；这里沿用程序参数
名称 `rate-gtps`，容量计算以模型定义为准。

以 256B、16 lane、PAM4 为例：

$$
N_{serialize,UI}=\frac{256\times8}{16\times2}=64\,UI
$$

字节按 round-robin 映射：

$$
lane(i)=i\bmod N_{lanes}
$$

每个 byte 在对应 lane 上按 MSB-first 发送：

- PAM4：每 byte 变成 4 个符号；
- NRZ：每 byte 变成 8 个符号。

接收端按相同 lane 位置解码并 de-stripe 回原始字节序列。

### 4.7 PAM4 和 NRZ

#### 4.7.1 PAM4

PAM4 采用 Gray 映射：

| bit    | 电平 |
| ------ | ---: |
| `00` |   -3 |
| `01` |   -1 |
| `11` |   +1 |
| `10` |   +3 |

接收门限为 `-2、0、+2`：

```text
y < -2       -> -3
-2 <= y < 0  -> -1
0 <= y < 2   -> +1
y >= 2       -> +3
```

#### 4.7.2 NRZ

NRZ 映射为：

```text
0 -> -1
1 -> +1
```

接收端以 0 为门限判决。项目需求要求覆盖 PAM4，因此默认模式是 PAM4；模型同时提供
`--mod nrz` 用于 NRZ 工作点参考和两电平/四电平对比。

### 4.8 信道与信号完整性

每条 lane 独立处理符号。接收采样值为：

$$
y[k]=h_0x[k]+h_1x[k-1]+h_2x[k-2]
+\sigma n[k]
+G_j\Delta t[k](x[k]-x[k-1])
$$

其中：

| 项                  | 含义                     |     默认值 |
| ------------------- | ------------------------ | ---------: |
| `h0`              | 主游标                   |        1.0 |
| `h1`              | 第一后游标 ISI           |       0.08 |
| `h2`              | 第二后游标 ISI           |       0.03 |
| `sigma`           | AWGN 标准差              |       0.05 |
| `n[k]`            | 标准高斯随机变量         | `N(0,1)` |
| `jitter_sigma_ui` | 采样抖动标准差           |    0.01 UI |
| `jitter_isi_gain` | 抖动到幅度误差的等效增益 |       0.40 |

公式中的各项在源码中相加；高斯随机数和抖动随机数本身可正可负，所以采样值仍可能被
向上或向下扰动。

#### 4.8.1 ISI项

`h0*x[k]`是当前符号主游标，`h1*x[k-1]`和`h2*x[k-2]`表示前两个符号在当前采样点的
残留。默认值表示前一个符号贡献8%、前两个符号贡献3%。例如：

```text
x[k]   = +3
x[k-1] = -3
x[k-2] = -3
```

不考虑噪声和抖动时：

$$
y[k]=3+0.08\times(-3)+0.03\times(-3)=2.67
$$

理想值为+3，现在被前序负电平拉低到2.67；PAM4的+3判决门限是2，因此仍能正确判决，
但到门限的裕量从1缩小到0.67。`h1`、`h2`也可以配置为负值，当前正值只是默认行为参数。

#### 4.8.2 AWGN项

噪声项满足：

$$
\sigma n[k]\sim\mathcal{N}(0,\sigma^2)
$$

当前PAM4电平是 `-3/-1/+1/+3`，理想电平到最近门限的距离为1。`sigma=0.05`表示相对于
归一化电平的标准差，不代表0.05 V；只有结合实际电压摆幅校准后才能换算成物理电压。
模型对不同lane和不同符号顺序抽取高斯样本，用来表示独立、平稳的白噪声。

#### 4.8.3 抖动等效项

真实抖动改变采样时刻。由于当前模型没有连续时间波形，它用：

$$
G_j\Delta t[k](x[k]-x[k-1])
$$

近似采样时刻偏移造成的幅度误差。当前后符号相同，差值为0，抖动项不产生影响；跳变幅度
越大，该项越大。例如PAM4从-3跳到+3的差值为6，比+1跳到+3的差值2更敏感。这保留了
“边沿附近采样更容易受抖动影响”的一阶趋势，但不是真实波形重采样。

#### 4.8.4 判决与协议层影响

得到 `y[k]` 后，PAM4按 `-2/0/+2`三个门限判决，NRZ按0门限判决。判决电平与原始电平
不同就增加 `symbol_errors`，随后解码为bit和字节。最终影响链为：

```text
ISI/AWGN/jitter
 -> 采样值跨越门限
 -> symbol/bit错误
 -> CRC或序号失败
 -> NAK/Replay
 -> 延迟上升、有效吞吐下降
```

#### 4.8.5 可信范围

该模型对“损伤增加导致误码和重传增加”的机制及相对趋势具有较好解释力；对真实链路
绝对SER/BER的可信度较低，除非使用独立物理数据校准。主要限制包括：

- `h1/h2`不是从真实封装或走线S参数提取的；
- `sigma`没有绑定具体电压、温度和接收机噪声；
- 抖动被等效为幅度误差，没有随机/确定性抖动频谱；
- 没有FFE、CTLE、DFE、串扰、电源噪声和lane间相关噪声；
- ISI记忆在每个Flit开始时置零，没有跨Flit连续性；
- 不同速率默认使用相同归一化损伤参数，因此24/32/48 GT/s可能得到相同SER。

所以它适合机制验证、参数敏感性分析和系统性能传播，不适合直接用于PHY签核或预测某条
真实链路的绝对BER。

### 4.9 Lane skew 与 deskew

每个 Flit、每条 lane 随机产生：

$$
skew_l\sim U\{0,1,\ldots,skew_{max}\}
$$

源码使用整数均匀分布，单位为UI。对于当前Flit，定义：

$$
N_{max\_skew}=\max(skew_0,skew_1,\ldots,skew_{L-1})
$$

例如4条lane分别得到 `[0,1,2,1] UI`，最慢lane比模型的0 UI基准晚2 UI。接收端必须暂存
先到lane的数据并等待最慢lane，因此当前Flit增加2 UI正向延迟。

`deskew_depth_ui`表示接收端能够容忍的最大lane偏差。如果任意lane的随机skew超过该深度，
则记录 `deskew_failure`。当前模型不真正移动各lane符号流或模拟deskew FIFO，而是在
de-stripe完成后翻转接收Flit中的一个随机bit，使错误进入：

```text
deskew超限 -> bit错误 -> CRC失败 -> NAK -> Replay
```

默认 `skew_max=2 UI`、`deskew_depth=8 UI`，正常工况不会发生 deskew failure。

24 GT/s时，2 UI约为83.33 ps，8 UI约为333.33 ps。若配置 `--skew 10 --deskew 8`，只要
某条lane抽到9或10 UI，该Flit就被判为deskew failure。

严格的lane-to-lane skew通常写为 `max(skew)-min(skew)`；当前实现把0 UI当作统一基准，
直接使用 `max(skew)`。此外，真实静态skew通常在多个Flit之间相对稳定，而当前模型每个
Flit重新随机生成。因此该功能适合观察deskew裕量不足对重传和性能的影响，不适合确定
真实硬件所需的deskew buffer深度。

### 4.10 CDR 锁定/失锁抽象

CDR 没有建模鉴相器、环路滤波器和 VCO，而是建模为 Flit 粒度状态机：

```text
初始 TRAIN
    │ 等待 train_ui
    ▼
 LOCKED
    │ 连续 N 个高SER Flit
    ▼
UNLOCKED
    │ sigma放大，等待relock_ui
    ▼
 LOCKED
```

#### 4.10.1 初始训练

默认：

```text
train_ui = 512 UI
```

训练阶段不发送业务 Flit。它是固定启动延迟，没有显式模拟训练序列、均衡器收敛或相位
逐步锁定。

#### 4.10.2 高误码 Flit 判定

对每个经过 PHY 的 Flit 计算：

$$
SER_{flit}=\frac{symbol\_errors}{total\_symbols}
$$

若：

$$
SER_{flit}>10\%
$$

则 `bad_streak` 加 1，否则立即清零。连续 4 个高误码 Flit 触发失锁。

Standard 256B Flit共有2048 bit。PAM4每个符号携带2 bit，因此每个Flit总计1024个符号；
严格大于10%意味着至少103个符号判错才算高误码Flit。NRZ则有2048个符号，至少205个
符号判错才超过10%。该门限表示严重、持续的信号劣化，不是偶发的一两个错误。

连续计数示例：

| Flit | Flit SER |    `bad_streak` |
| ---: | -------: | ----------------: |
|  100 |      12% |                 1 |
|  101 |      14% |                 2 |
|  102 |      11% |                 3 |
|  103 |      13% | 4，触发失锁后清零 |

如果中间任意一个Flit的SER不超过10%，`bad_streak`立即清零。这样可以避免单个偶然坏
Flit造成CDR频繁失锁。

这里判断的是 PHY 产生的符号错误比例，而不是 CRC failure。`--extra-flit-error` 和
deskew failure 在符号判决完成后翻转 bit，因此不会增加 `symbol_errors`，也不会直接触发
CDR 失锁。

CRC只能表示一个Flit“是否存在错误”，无法区分只有1个符号错误还是数百个符号错误；CDR
状态机使用Flit内SER，是为了只响应严重的模拟信道劣化。一个CRC failure不等于一次CDR
失锁。

#### 4.10.3 失锁影响与重锁

失锁时将噪声放大：

$$
\sigma_{unlocked}=4\sigma_{locked}
$$

默认基础 `sigma=0.05` 时，失锁后的有效sigma变成0.20。这个处理并不是认为真实热噪声
必然增加四倍，而是用更大幅度扰动等效“采样相位不再位于眼图最佳位置”造成的判决恶化。

每个Flit开始处理时先查询当前锁定状态，处理结束后才用本Flit SER更新CDR。因此触发失锁
的第4个高误码Flit本身仍按锁定状态处理，噪声放大从后续Flit开始生效。

默认重锁窗口为512 UI：

|    速率 | 512 UI对应时间 |
| ------: | -------------: |
| 24 GT/s |       21.33 ns |
| 32 GT/s |       16.00 ns |
| 48 GT/s |       10.67 ns |

到期后自动回到锁定状态，不要求连续若干个正确Flit，也没有显式训练序列或相位逐步收敛。
如果重锁窗口内没有业务Flit，下一个在截止时间之后到达的Flit会直接按锁定状态处理。

失锁期间仍会统计Flit SER。若再次累计4个高误码Flit，当前实现会增加
`lock_loss_count`，并把 `relock_until`重新设置成“再次触发时间+512 UI”，相当于延长
失锁窗口。普通正确Flit只能清零 `bad_streak`，不会提前结束当前重锁计时。

该状态机能表达“持续严重误码 -> 失锁 -> 错误进一步恶化 -> 定时恢复”的系统级因果链，
但10%、连续4个Flit、4倍sigma和512 UI都是行为参数，不是由鉴相器、环路带宽或真实
抖动容限计算得到的。现有单元测试直接验证连续坏 Flit 门限和 512 UI 重锁，系统回归 T11
通过可控 CDR 参数稳定触发失锁、Degraded 状态和重传恢复。

### 4.11 正向管线延迟

一个 Frame 经过 PHY 后的到期时间为：

$$
T_{due}=T_{now}+
(N_{tx}+N_{channel}+N_{max\_skew}+N_{rx})\times UI
$$

默认固定部分是：

```text
tx_pipe=4 UI
channel=8 UI
rx_pipe=4 UI
```

这里的 `N_max_skew`是当前Flit所有lane随机skew中的最大值，表示接收端为等待最慢lane而
增加的时间。例如 `[0,1,2,1] UI`对应 `N_max_skew=2 UI`。默认固定延迟16 UI、最大skew
2 UI时，正向管线延迟最多18 UI；24 GT/s下约为0.75 ns。

`PhyChannel` 把待输出 Frame 与到期时间放入队列，egress 线程到时释放。该延迟是纯流水线
延迟，不会再次限制吞吐；吞吐主要由 TX 串行化时间控制。

### 4.12 RX D2D Adapter

RX 使用 CRC 和模 256 序号差判断帧类型。设期望低 8 bit 为 `exp8`：

$$
diff=(rx\_seq8-exp8)\bmod256
$$

判定规则：

| 条件                         | 动作                                     |
| ---------------------------- | ---------------------------------------- |
| CRC 正确且`diff=0`         | 按序交付，发送 ACK，期望序号加 1         |
| CRC 正确且`1<=diff<=127`   | 认为是未来/超前帧，记录序号失败并发送NAK |
| CRC 正确且`128<=diff<=255` | 认为是旧重复帧，丢弃并重发最近ACK        |
| CRC 错误                     | 不信任内容和序号，记录CRC失败并发送NAK   |

`1<=diff<=127`并不一定表示当前Flit的数据本身损坏，而是表示它到达得太早。例如RX当前
期望10却收到CRC正确的12：

$$
diff=(12-10)\bmod256=2
$$

这说明10和11尚未按序接收。当前RX没有Selective Repeat式乱序缓存，不能先保存12并等待
缺口，只能丢弃12、发送 `NAK(10)`，让TX按Go-Back-N方式重新发送10、11、12及后续未确认
Flit。这里的“序号失败”更准确地说是“当前不能按序接收”。

回绕时规则仍成立：若当前期望255却收到0，则 `diff=(0-255) mod 256=1`，说明0是超前1个
的未来帧，RX仍需先请求255。相反，当前期望2却收到255时，`diff=253`，被视为旧重复帧。

`diff=128`正好位于8-bit序号圆环的对面，数学上无法判断方向。由于Retry Buffer被限制为
最多127个合法未确认Flit，正常未来帧不会距离期望值128；当前代码因条件 `diff>127`将
128归入旧帧分支。

RX 状态为：

```text
NoRetry <──正确期望帧── RetryInProgress
   │                         ▲
   └────错误/超前帧──────────┘
```

为避免流水线内多个错误/超前帧造成 NAK 风暴，首次错误立即发送 NAK，随后同一恢复阶段每
8 个 Frame 最多重复一次 NAK。

### 4.13 反馈通道

反馈通道默认固定延迟 32 UI。它采用待发送队列和到期时间，因此是纯延迟模型，不限制
ACK/NAK 消息吞吐。

当前假设反馈通道无误码。这便于把实验结果集中归因于正向主链路，但不能覆盖反馈丢失、
反馈 CRC 错误等故障场景；Replay Timer 只提供无反馈时的活性兜底。

### 4.14 Watchdog

当仿真达到 `max_time_ui` 仍未交付全部 Flit 时：

- 设置 `watchdog_fired=1`；
- 停止 SystemC 仿真；
- 程序因交付数量不足返回状态码 2。

该功能用于限制极高误码或重传风暴工况的运行时间，不代表链路已经恢复。

## 5. 参数配置

### 5.1 命令行参数

| 参数                   |          默认值 | 作用                                     |
| ---------------------- | --------------: | ---------------------------------------- |
| `--flits`            |            2000 | 请求交付的 Flit 数                       |
| `--format`           | `standard256` | `standard256` 或 `compact68`         |
| `--lanes`            |              16 | mainband lane 数                         |
| `--rate-gtps`        |              24 | 每 lane 速率                             |
| `--mod`              |        `pam4` | `pam4` 或 `nrz`                      |
| `--fdi-queue`        |              64 | FDI TX FIFO 深度                         |
| `--retry-buffer`     |              64 | 未确认 Flit 缓存深度，范围 1～127        |
| `--tx-delay`         |            4 UI | TX 管线延迟                              |
| `--rx-delay`         |            4 UI | RX 管线延迟                              |
| `--channel-delay`    |            8 UI | 正向信道传播延迟                         |
| `--feedback-delay`   |           32 UI | ACK/NAK 反馈延迟                         |
| `--train`            |          512 UI | 初始训练时间                             |
| `--fdi-interval`     |               0 | 注入间隔；0 表示自动等于 Flit 串行化时间 |
| `--sigma`            |            0.05 | AWGN 标准差                              |
| `--jitter`           |         0.01 UI | 采样抖动标准差                           |
| `--isi1`             |            0.08 | 第一后游标                               |
| `--isi2`             |            0.03 | 第二后游标                               |
| `--skew`             |            2 UI | 最大随机 lane skew                       |
| `--deskew`           |            8 UI | RX deskew 容限                           |
| `--extra-flit-error` |               0 | 每次 PHY 传输后额外翻转一个 bit 的概率   |
| `--seed`             |               7 | 随机种子                                 |
| `--max-time`         |   50,000,000 UI | Watchdog 时间                            |
| `--csv`              |              空 | 指定汇总 CSV 路径                        |
| `--verbose`          |            关闭 | 打印部分 TX/RX/重传事件                  |

### 5.2 CDR 与可靠性参数

以下关键 CDR/可靠性参数已经暴露为命令行选项：

| 参数                              | 默认值 |
| --------------------------------- | -----: |
| `jitter_isi_gain`               |   0.40 |
| `isi_h0`                        |    1.0 |
| `--cdr-relock`                  | 512 UI |
| `--cdr-ser`                     |   0.10 |
| `--cdr-bad-flits`               |      4 |
| `--cdr-noise-mult`              |    4.0 |
| `--nak-repeat`                  |      8 |

`jitter_isi_gain` 和 `isi_h0` 仍为源码级模型常量；常规扫描使用已公开的 `--jitter`、
`--isi1` 和 `--isi2`。

## 6. 指标定义与计算方法

### 6.1 原始容量

$$
C_{raw}=N_{lanes}\times bits\_per\_UI\times R_{lane}
$$

例如 16 lane、24 GT/s、PAM4：

$$
C_{raw}=16\times2\times24=768\text{ Gbit/s}
$$

注意：这是当前模型把 `rate-gtps` 作为每 lane 符号/UI 速率后的容量定义。

### 6.2 有效吞吐

$$
Throughput=
\frac{completed\_payload\_bytes\times8}
{active\_time}
$$

其中：

$$
active\_time=t_{last\_delivery}-T_{train}
$$

训练时间从吞吐统计区间中扣除，避免固定启动开销随仿真 Flit 数变化而扭曲稳态带宽。

### 6.3 带宽利用率

$$
Utilization=\frac{Throughput}{C_{raw}}
$$

在无错误、持续满载条件下，该值接近 Flit payload 效率，而不是 100%。重传、反馈等待和
背压会进一步降低利用率。

### 6.4 延迟

round-trip 延迟从 `FdiSource` 将请求注入正向 FDI 开始，到 `FdiSink` 收到正确业务响应为止：

$$
Latency_{rt,i}=t_{response\_sink,i}-t_{request\_source,i}
$$

因此：

- 初始 `train_ui` 不计入单个事务延迟；
- 正向/反向 FDI 等待、Retry Buffer、串行化、PHY 管线、重传和 `memory_delay_ui` 均计入；
- 同时输出正向 hop、反向 hop 以及 round-trip 的平均/P95/P99。

在 T1 的 16 lane、24 GT/s、PAM4、无 skew 条件下：

```text
正向 hop = 64 UI serialization + 4 UI TX + 8 UI channel + 4 UI RX = 80 UI
反向 hop = 80 UI
MemoryResponder = 32 UI
round-trip = 192 UI × 41.667 ps ≈ 8.000 ns
```

该值与 T1 的正向/反向各约 3.333 ns、round-trip 约 8.000 ns 一致。

### 6.5 错误指标

$$
SER=\frac{symbol\_errors}{total\_symbols}
$$

$$
BER=\frac{bit\_errors}{total\_bits}
$$

$$
FlitFailRate=
\frac{crc\_fail\_count+seq\_fail\_count}
{total\_phy\_frames}
$$

`FlitFailRate` 会包含原始错误以及前序错误引起的超前序号帧，因此不一定等于
`extra_flit_error_rate`。

## 7. 现有验证体系

### 7.1 验证层次

当前验证分为两层：

1. `scripts/run_tests.sh`：固定场景和断言，用于功能回归；
2. `scripts/run_sweep.sh`：速率、噪声和 lane 数扫描，用于趋势和性能分析。

`make test` 会先根据源码依赖检查是否需要重编译，再运行测试。直接运行
`scripts/run_tests.sh` 时只会在二进制不存在时编译，因此日常回归推荐使用：

```bash
make test
```

### 7.2 22 项单元断言与 82 项系统断言

`src/ucie_unit_tests.cpp` 提供 CRC 标准黄金值、两种 Flit、PAM4/NRZ、lane 映射、序号回绕、
CDR 门限/重锁、deskew 和配置边界等 22 项确定性断言。SystemC 系统回归包含：

| 测试            | 激励                              | 关键断言                                                  | 覆盖功能                          |
| --------------- | --------------------------------- | --------------------------------------------------------- | --------------------------------- |
| T1 干净链路     | 3000 Flit，关闭噪声/抖动/ISI/skew | 全部交付；0 replay；0 NAK；0 integrity error；利用率≥90% | 基本数据通路、格式效率、吞吐      |
| T2 24 GT/s      | 16 lane PAM4，24 GT/s             | 容量约 768 Gbit/s；速率记录正确                           | 核心速率指标、容量公式            |
| T3 错误注入     | 2000 Flit，5%额外 bit 翻转        | 出现 CRC fail、NAK、replay；最终全部按序正确交付          | CRC、错误反馈、Retry Buffer、重传 |
| T4 中等噪声     | 2000 Flit，sigma=0.20             | SER>0；发生 replay；全部正确交付                          | 行为级信道到可靠传输的因果链      |
| T5 NRZ          | 32 GT/s NRZ，关闭损伤             | 容量约512 Gbit/s；利用率≥90%；无完整性错误               | NRZ映射和容量                     |
| T6 Compact 68B  | 1000 Flit                         | 全部交付；payload=64B；无完整性错误                       | 第二种Flit格式                    |
| T7 背压         | Retry Buffer=4，反馈延迟512 UI    | 缓存满事件>0；最大占用=4；全部交付                        | 缓存边界和背压                    |
| T8 可复现性     | seed 11两次、seed 12一次          | 同seed CSV完全一致；不同seed结果不同                      | 随机模型确定性                    |
| T9 32 GT/s PAM4 | 1000 Flit，sigma=0.05             | 全部响应交付；利用率≥90%                                 | 高于24 GT/s运行点                 |
| T10 外部 Payload | 3 个不同 ID/VC/长度的外部记录      | 正反向都发送；返回文件与输入逐字节相同；0 integrity error | 外部 FDI、有效长度、ID、VC、反向业务链路 |
| T11 CDR 专项     | 可控 CDR 门限和重锁时间            | 触发失锁和 Degraded；全部恢复；0 integrity error          | CDR 失锁/重锁与状态输出           |
| T12 deskew       | 1 lane、0/1 UI skew、0 UI 容限      | deskew fail 和 replay>0；全部交付                         | deskew 异常与恢复                 |
| T13 48 GT/s      | 16 lane PAM4、双向满载             | 容量1536 Gbit/s；利用率≥90%；双向各1000 Flit              | 48 GT/s 与双向吞吐                |
| T14 Watchdog     | `max_time_ui=10`                   | 返回不完整状态；watchdog=1；link_state=Failed             | 超时与失败状态                    |

### 7.3 当前实测回归结果

2026-08-25 重新执行结果：

```text
UNIT_PASS=22 UNIT_FAIL=0
PASS=82 FAIL=0
```

部分关键结果：

| 场景          | 结果                                                         |
| ------------- | ------------------------------------------------------------ |
| T1 干净双向链路 | 利用率 0.921253，round-trip 约 8.000 ns，正反向各 3000 个新 Flit，0 replay/NAK/error |
| T3 5%错误注入 | 正反向合计 193 次额外错误、182 NAK、427 replay，最终 2000/2000 响应正确交付 |
| T4 sigma=0.20 | 聚合 SER 约 4.79e-5、209 NAK、506 replay，最终 2000/2000 响应正确交付 |
| T10 外部 Payload | 3/3 请求及响应交付，导出的 transaction ID、VC 和 Payload 与输入完全一致 |
| T7 小缓存     | 最大占用严格为4，缓存满事件1059，最终1000/1000正确交付       |

### 7.4 参数扫描

`run_sweep.sh` 包含：

- PAM4：24/32/48 GT/s × sigma 0.00/0.05/0.10/0.15/0.18/0.20；
- lane 数：8/16/32/64；
- NRZ：24/32 GT/s参考点。

当前 `results/sweep/summary.csv` 中的典型结果：

|    速率 | sigma |  利用率 |  平均延迟 | NAK / replay | 完整性错误 |
| ------: | ----: | ------: | --------: | -----------: | ---------: |
| 24 GT/s |  0.00 | 92.178% |   3.42 ns |        0 / 0 |          0 |
| 24 GT/s |  0.15 | 92.117% |   7.47 ns |        1 / 2 |          0 |
| 24 GT/s |  0.18 | 89.901% | 108.10 ns |      38 / 76 |          0 |
| 24 GT/s |  0.20 | 78.831% | 187.67 ns |    182 / 508 |          0 |
| 32 GT/s |  0.05 | 92.179% |   2.56 ns |        0 / 0 |          0 |
| 48 GT/s |  0.05 | 92.180% |   1.71 ns |        0 / 0 |          0 |

结果体现了当前模型预期的因果关系：

```text
sigma增大
  -> 符号判错增多
  -> CRC/序号失败增多
  -> NAK与重传增多
  -> 延迟上升
  -> 有效吞吐和利用率下降
```

lane 扫描中利用率保持约 92.2%，平均延迟随 lane 数增加而下降，符合串行化 UI 数减少的
模型定义。

## 8. 验证结论的可信范围

### 8.1 已有较强证据支持的结论

- SystemC 模块可以完整运行并交付指定数量 Flit；
- 干净链路的串行化、容量和利用率计算内部一致；
- bit 错误可以被 CRC 捕获并转化为 NAK/replay；
- 重传后 Sink 数据内容和顺序保持正确；
- Retry Buffer 深度限制和背压生效；
- 固定 seed 可以复现实验；
- 24/32/48 GT/s 可以在当前 UI 时间模型下运行；
- 噪声增强会导致重传、延迟上升和利用率下降。

### 8.2 尚不能由现有测试证明的结论

#### 统计准确性尚未建立

当前扫描已经覆盖 seed 7/11/29 和多组 jitter，但尚未给出严格统计置信区间，也没有将纯
AWGN 场景的 SER 与理论 4-PAM/NRZ 结果比较。CRC 已加入 `123456789 -> 0x29B1` 独立
黄金值；CDR 和 clean-path 80 UI 单跳/192 UI round-trip、P50/P95/P99 已加入自动断言。

#### 物理通道没有校准

`sigma`、ISI tap和jitter gain是归一化行为参数，没有与某个具体封装、走线、工艺、
接收机带宽或实测眼图绑定。不同速率下这些归一化参数默认保持不变，所以“速率提升但
SER不变”是当前模型参数化方式造成的结果，不应解读为真实高速链路一定没有额外损耗。

#### 全链路指标尚需联合仿真

当前吞吐和延迟只覆盖 FDI Source 到 FDI Sink。AXI转换、Protocol Bridge、MC调度、
DRAM bank冲突和刷新均不在本仿真中，因此不能独立完成项目级全链路验收。

## 9. 建议补充的验证用例

### 9.1 单元级黄金向量

建议把以下函数拆出独立单元测试：

| 对象         | 建议黄金参考                               |
| ------------ | ------------------------------------------ |
| CRC-16/CCITT | 标准字符串和固定256B向量                   |
| PAM4编码     | `00/01/11/10 -> -3/-1/+1/+3`逐项断言     |
| PAM4判决     | 门限内、门限边界、门限外测试               |
| NRZ判决      | 0两侧及边界测试                            |
| lane mapping | 固定递增byte向量的striping/de-striping结果 |
| Flit构造     | 字段偏移、CRC覆盖区间、replay flag         |
| 序号回绕     | 254、255、0、1附近的ACK/重复/超前判断      |

### 9.2 CDR 专项验证

至少增加以下场景：

1. 连续3个高SER Flit：不得失锁；
2. 连续4个高SER Flit：`lock_loss_count`必须加1；
3. 中间插入一个低SER Flit：`bad_streak`必须清零；
4. 失锁时检查有效sigma从 `sigma` 变成 `4*sigma`；
5. 511 UI时仍失锁，512 UI到期后恢复；
6. 失锁期再次触发时验证窗口是否按设计延长；
7. `train=0/512/1024`下首个业务Flit出现时间是否准确。

为了稳定构造高SER Flit，建议允许命令行设置CDR门限和连续Flit数，或给
`BehavioralPhy::observe()`编写直接单元测试。

### 9.3 统计验证

关闭ISI、jitter和skew，只保留AWGN，对每个sigma运行多个seed，统计：

- SER/BER均值；
- 95%置信区间；
- 理论值是否落入置信区间；
- Flit数量增加时估计值是否收敛。

随后逐项打开ISI、jitter、skew，避免多个损伤同时存在而无法归因。

### 9.4 协议与异常路径

建议增加：

- 明确跨越8-bit序号回绕点的断言；
- burst error和连续CRC失败；
- replay本身再次损坏；
- stale NAK和duplicate ACK；
- Replay Timer明确触发；
- Watchdog预期触发；
- deskew刚好等于容限和超过容限；
- FDI FIFO满、Retry Buffer满和两者同时发生；
- 不同反馈延迟下的最大在途窗口。

### 9.5 性能解析对照

对无损链路自动计算并断言：

- `serialize_ui`；
- 首Flit延迟；
- 稳态Flit间隔；
- 理论raw capacity；
- 理论payload效率；
- 测量吞吐与理论值的允许误差。

### 9.6 外部校准与全链路集成

物理准确性应至少选择一种独立参考进行校准：

- S参数/IBIS-AMI或SPICE产生的BER、眼高、抖动容限；
- 实际SerDes测试芯片或板级测量；
- 经确认的链路预算和误码曲线。

项目级验收则应把本模型接入：

```text
GEM5/AXI Trace
 -> AXI2Flit
 -> UCIe SystemC Link
 -> Flit2DFI/Protocol Bridge
 -> MC/MEM PHY
 -> DRAMsim3/Ramulator
```

统一使用请求ID和时间戳，分别记录：

- AXI2Flit延迟；
- UCIe链路延迟；
- Bridge转换延迟；
- MC排队延迟；
- DRAM服务延迟；
- 端到端平均/P95/P99延迟；
- 各层有效带宽和背压占比。

只有这样才能判断“全链路利用率≥90%”与“协议桥接延迟≤20 ns”是否真正满足。

## 10. 编译、运行与复现

### 10.1 编译

```bash
cd /home/hy258/ucie/ucie_systemc
make
```

依赖：

- C++17编译器；
- SystemC 2.3.4或兼容版本；
- `libsystemc`可被链接器找到。

### 10.2 典型运行

```bash
# 默认：24 GT/s、16 lane、PAM4
./build/ucie_sc_sim

# 干净链路
./build/ucie_sc_sim \
  --sigma 0 --jitter 0 --isi1 0 --isi2 0 --skew 0

# 中等噪声并输出事件
./build/ucie_sc_sim \
  --rate-gtps 32 --sigma 0.18 --verbose

# NRZ模式
./build/ucie_sc_sim \
  --mod nrz --rate-gtps 32

# 输出CSV
./build/ucie_sc_sim \
  --csv results/latest.csv
```

### 10.3 功能回归

```bash
make test
```

结果位于：

```text
results/tests/*.log
results/tests/*.csv
```

### 10.4 参数扫描

```bash
make sweep
```

汇总结果位于：

```text
results/sweep/summary.csv
```

复现实验时应同时记录：源码版本、编译器/SystemC版本、完整命令行、随机seed和输出CSV。

## 11. 源码与文档索引

```text
ucie_systemc/
├── Makefile
├── README.md
├── INTERFACE.md                       # UcieLink/FdiFlit接口契约
├── TEST_PLAN.md                       # 测试矩阵与复现入口
├── MODELING_AND_VERIFICATION.md       # 本文
├── src/
│   ├── ucie_common.h                  # Config、CLI、CRC、Flit、Stats
│   ├── ucie_fdi.h                     # 公开事务和链路状态
│   ├── ucie_phy.h                     # PAM4/NRZ、信道、skew、CDR
│   ├── ucie_link.h                    # 独立双向UcieLink
│   ├── ucie_unit_tests.cpp            # 22项黄金断言
│   └── ucie_systemc_main.cpp          # 测试端点、仿真和指标
├── scripts/
│   ├── run_tests.sh                   # 82项系统断言
│   ├── run_sweep.sh                   # 36点参数扫描
│   └── run_validation.sh              # 一键验证与报告生成
└── results/
    ├── tests/                         # 功能回归日志和CSV
    └── sweep/                         # 扫描日志、CSV和summary.csv
```

相关文件：

- [项目说明](README.md)
- [接口定义](INTERFACE.md)
- [测试计划](TEST_PLAN.md)
- [公共配置与Flit定义](src/ucie_common.h)
- [事务级FDI](src/ucie_fdi.h)
- [行为级PHY](src/ucie_phy.h)
- [独立UcieLink及Adapter](src/ucie_link.h)
- [SystemC测试平台](src/ucie_systemc_main.cpp)
- [功能测试](scripts/run_tests.sh)
- [参数扫描](scripts/run_sweep.sh)
- [当前扫描汇总](results/sweep/summary.csv)

## 12. 总结

当前 `ucie_systemc` 已形成一个可运行、可配置、可复现的 UCIe 高速互联行为级框架：

- 用独立双向 `UcieLink` 和统一 `FdiFlit` 四端口表达链路边界；
- 用 TX/RX D2D Adapter 表达序号、CRC、ACK/NAK和可靠重传；
- 用行为级PHY表达lane映射、PAM4/NRZ、ISI、AWGN、jitter、skew和CDR失锁；
- 用 SystemC 时间轴表达串行化、管线、反馈、训练和重锁延迟；
- 用功能回归和参数扫描观察正确性与性能趋势。

现有22项单元断言和82项系统断言覆盖独立CRC/PAM4/NRZ/lane黄金向量、CDR、deskew、
精确延迟、背压、重传和24/32/48 GT/s双向传输，能证明链路模型在低噪声条件下的
payload利用率约92.18%。36点扫描进一步覆盖速率、lane、噪声、jitter和多个随机seed。
尚需外部补充的主要是统计理论对照和真实物理通道校准。

因此，这个模型目前适合作为独立 UCIe 链路的功能、可靠性、性能和参数敏感性验证组件；
在完成外部物理校准前，不应把行为参数结果直接作为真实芯片 PHY 签核数据。
