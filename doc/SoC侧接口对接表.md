# SoC 侧接口对接表

本文供主机模型、AXI 激励和 trace 回放器开发者使用，描述当前可以连接的接口。实现定义见 [axi2flit.h](../systemc/include/axi2flit.h)、[axi_if.h](../systemc/include/axi_if.h)，已有接线与监视器见 [tb_full_link.cpp](../systemc/tb/tb_full_link.cpp)。

**当前 SoC 边界是 `Axi2Flit` 的 AXI 从机端口。** TLM socket、gem5 Packet、Vortex 原生请求和 trace 文件都不能直接绑定这些端口，需要主机侧适配器。相关适配器尚未实现，开发方案见[全链路联合仿真接入指南](全链路联合仿真接入指南.md)。

## 1. 模块边界与端口方向

```text
主机模型 / AXI BFM / 主机适配器
                 ⇅ 本文的 AXI 五通道
             Axi2Flit
                 ⇅ 帧握手
          UcieAouAdapter ⇄ UcieLink ⇄ AouTarget ⇄ 存储后端
```

BFM（Bus Functional Model，总线功能模型）负责产生总线访问与处理握手，不要求模拟完整 CPU。`UcieAouAdapter` 位于桥与 UCIe 之间，不是主机到 AXI 的适配器。

下表方向相对 `Axi2Flit`。各端口通过顶层创建的 `sc_signal<T>` 连接，时钟也可以绑定 `sc_clock`；每条信号只能有一个驱动方。

| 功能 | 桥输入：主机/顶层驱动 | 桥输出：桥驱动 | 对象类型 |
|---|---|---|---|
| 时钟与复位 | `clk`、`rst_n` | — | `bool`，复位低有效 |
| AW 写地址 | `aw_valid`、`aw_ch` | `aw_ready` | 握手为 `bool`，载荷为 `AxChannel` |
| W 写数据 | `w_valid`、`w_ch` | `w_ready` | 握手为 `bool`，载荷为 `WChannel` |
| B 写响应 | `b_ready` | `b_valid`、`b_ch` | 握手为 `bool`，载荷为 `BChannel` |
| AR 读地址 | `ar_valid`、`ar_ch` | `ar_ready` | 握手为 `bool`，载荷为 `AxChannel` |
| R 读数据 | `r_ready` | `r_valid`、`r_ch` | 握手为 `bool`，载荷为 `RChannel` |

这是五组独立通道，不能用一个 `Axi4Beat` 对象替代整个顶层端口集合。通道结构体中自带的 `valid/ready` 成员不控制顶层握手；主机填写请求时可保留这些成员默认值，接收响应也只按上表的独立端口判断握手。

所有包含本项目公共接口头文件的模块必须使用相同的 `AXI_DATA_WIDTH_CFG` 和兼容的 C++/SystemC 构建；外部模型自身的位宽可以不同，由适配器转换。`D = AXI_DATA_BYTES = AXI_DATA_WIDTH / 8`，取 32、64、128；它不是运行中可更改的参数。

## 2. 请求与响应字段

### AW/AR：`AxChannel`

| 字段 | C++ 容器 / 有效范围 | 功能及接入要求 |
|---|---|---|
| `id` | `uint16_t` / 0～1023 | AXI ID。高位不能用于区分请求，较宽的上游 ID 需映射 |
| `addr` | `uint64_t` / 64 位 | 系统物理字节地址；地址窗口由顶层与后端共同约定 |
| `len` | `uint8_t` / 0～255 | 拍数减一；单拍填 0，256 拍填 255 |
| `size` | `uint8_t` / 0～log2(D) | 每拍字节数的 log2；4B 填 2，32B 填 5 |
| `burst` | `uint8_t` / 当前为 1 | 仅 INCR，地址按拍递增 |
| `lock` | `uint8_t` / 1 位 | 普通访问填 0；当前简单内存不执行独占，非零返回访问错误 |
| `cache` | `uint8_t` / 4 位 | 内存属性；当前桥传递，简单内存不执行缓存策略 |
| `prot` | `uint8_t` / 3 位 | 保护属性；当前简单内存不执行权限检查 |
| `qos` | `uint8_t` / 4 位 | 通过 `qos % rp_count` 选择资源平面；初次接入可填 0 |
| `user` | `uint16_t` / 16 位 | 地址附加信息；当前简单内存将其回送至 BUSER/RUSER |

不使用的属性须显式初始化。AXI 通道可表达的属性不代表后端已经执行相应语义；主机适配器不能把原子、一致性或不支持的特殊访问默认为普通读写。

### W/B/R 载荷

| 对象字段 | C++ 容器 | 功能及接入要求 |
|---|---|---|
| `w_ch.data` | `uint8_t[D]` | 本拍完整总线数据，每项一个字节 |
| `w_ch.strb` | `uint8_t[D]` | 每个数据字节一项，0 禁用、非零启用；不是打包后的位图 |
| `w_ch.last` | `bool` | 仅本突发的最后一拍为 true |
| `w_ch.user` | `uint16_t` | 写拍附加信息；当前简单内存不使用 |
| `b_ch.id` | `uint16_t`，有效 10 位 | 对应 AWID |
| `b_ch.resp` | `uint8_t`，有效 2 位 | 整笔写响应状态 |
| `b_ch.user` | `uint16_t` | 写响应附加信息 |
| `r_ch.id` | `uint16_t`，有效 10 位 | 对应 ARID |
| `r_ch.data` | `uint8_t[D]` | 本拍完整总线读数据 |
| `r_ch.resp` | `uint8_t`，有效 2 位 | 本拍读响应状态 |
| `r_ch.last` | `bool` | 仅本读突发的最后一拍为 true |
| `r_ch.user` | `uint16_t` | 本拍读响应附加信息 |

当前普通存储配置使用 OKAY=0、SLVERR=2、DECERR=3；不提供独占成功的执行语义。R 没有独立字节掩码，主机根据已接受 AR 的地址、SIZE 和拍号提取有效字节。B/R 也不携带地址，主机须保存请求上下文。

## 3. 窄访问与数据字节布局

对本项目支持的对齐 INCR，设 `S = 1 << size`，拍号 n 从 0 开始：

```text
本拍首地址 A[n] = addr + n × S
本拍起始 lane  = A[n] % D
data[lane + j] 对应内存地址 A[n] + j，0 <= j < S
```

当前要求 S 不超过 D 且地址按 S 对齐，因此单个拍不会跨越其总线字节组。合法写选通只落在本拍的访问 lane 内；范围之外的字节建议初始化为零，选通必须关闭。全零选通也是一笔需要正常完成的写拍，不能跳过它或少发送 LAST。

例：256 位总线（D=32），首地址为测试窗口基址加 `0x1C`，`size=2`、`len=1`，写入两个 4B 拍：

| 拍号 | 相对窗口基址的偏移 | 使用的 lane | 数据按地址递增 | 对应 RTL WSTRB 位图 |
|---:|---:|---|---|---|
| 0 | `0x1C` | 28～31 | `11 22 33 44` | `0xF0000000` |
| 1 | `0x20` | 0～3 | `55 66 77 88` | `0x0000000F` |

表中地址偏移以窗口基址为参照。C++ 中应逐项设置 `strb[28..31]` 或 `strb[0..3]`，不能把整个位图写入一个数组元素。存储侧字段名是 `strobe`，与 SoC 侧 `strb` 不同，但字节选通含义相同。

当前简单内存的窄读只在相应 lane 返回有效数据，其余 lane 为零。CSV/VCD 的总线十六进制字符串最右侧对应 lane0；这与按地址递增打印连续 payload 字节的顺序不同。消息序列化字节顺序也不改变 AXI 上述字节位置。

## 4. 握手、顺序与完成

1. 每个时钟上升沿，独立 `VALID && READY` 表示接受一拍。VALID 连续为 1 时，每个满足条件的边沿都算一次，不能只按跳变计数。
2. `VALID && !READY` 时发送方保持 VALID 和完整载荷，直至握手。主机不能等待 READY 才开始提供一个已经可发送的请求，以免形成双方等待。
3. AW、W 独立握手。当前桥先接受 AW、取得写路由后才会接受相应 W；主机可以提前提供 WVALID 并等待，但必须独立推进 AW，不能等待 W 握手后才发 AW。
4. W 按 AW 接受顺序配对，一笔突发的写数据不能与另一笔交错。每笔交付恰好 `len+1` 拍，只有末拍 LAST=1。
5. 写在 BVALID/BREADY 握手时对主机完成；读在 RVALID/RREADY/RLAST 同时成立的边沿完成。仅看到 BVALID 或 RLAST 不足以回收请求上下文。
6. 同方向、同 ID 的未完成请求必须保持 RP 不变；同 ID 响应按顺序匹配，不同 ID 的先后不能依赖全局发起顺序。单 RP 并不意味着只有一笔 outstanding。
7. 读写间无隐含全局顺序。需要写后读依赖时，等待相应 B 完成再发依赖读；同一个 ID 本身不建立跨读写方向的依赖。

主机可提前拉高 BREADY/RREADY，也可制造有限背压；背压会占用链路响应资源。桥的 READY 随队列、路由和 credit 变化，接入方不应假设固定接受周期或固定返回延迟。

请求合法性由 [axi_contract.h](../systemc/include/axi_contract.h) 检查。非 INCR、超 SIZE、未对齐、跨 4KB 以及 WLAST 不匹配会在入口触发致命报告，不是自动返回 DECERR。对合法格式但越出后端窗口的访问，当前简单内存通过 B/R 返回 DECERR。同 ID 跨 RP 违例会记录在 `bridge.order_violations()`，顶层验收须检查为 0。

## 5. 顶层装配、启动与交接

当前没有可直接复用的统一 `FullLinkSystem` 导出模块，完整实例化位于 `FullLinkTb`。新增主机顶层应参考其模块和接线，替换 stimulus/ready_driver 的驱动职责，并保留监视器；不能让原 BFM 和新主机同时驱动同一组信号。

`tb_full_link.cpp` 中使用的端口绑定形式如下。这里只示例 AW，完整连接须覆盖第 1 节所有端口及内部帧接口：

```cpp
sc_signal<bool> aw_valid, aw_ready;
sc_signal<AxChannel> aw_payload;
bridge.aw_valid(aw_valid);
bridge.aw_ready(aw_ready);
bridge.aw_ch(aw_payload);
```

顶层在创建模型前设置时间分辨率，驱动公共时钟，启动时保持桥、Adapter 和 Target 复位，待 UCIe Active 后在确定的相位释放复位。现有测试使用 1fs 分辨率、2ns 时钟，并在下降沿驱动主机信号，在上升沿观察握手；接入方可复用这种方式避免同沿采样歧义。启动后的 credit 交换由现有链路模块完成，主机只需遵守 READY。

时钟周期、AXI 位宽和地址窗口是顶层配置，当前默认值不是 SoC 模型必须采用的硬件参数；修改周期后应重新核对容量预算和性能门限。不能在有在途事务时单独复位一端。

主机开发者负责交付：完整字段映射、请求/响应上下文、地址映射和数据初始化约定、并发/背压策略，以及 AXI 监视结果。模型结束后还要等待 B/R、链路和后端在途事务排空。两侧共同配置和分阶段验收见[联仿交接约定](全链路联合仿真接入指南.md#11-联仿交接约定)。
