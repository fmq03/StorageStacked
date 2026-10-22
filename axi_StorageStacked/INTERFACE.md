# AXI 存储接口 v1

存储侧为 `storage_axi::AouBackend`。所有端口在 `axi_signals.hh` 中实际声明为
SystemC `sc_in` / `sc_out`，可以直接绑定信号，也可使用 `Signals` 与 `MasterPorts`。

| 通道 | 驱动 → 存储 | 存储 → 驱动 |
|---|---|---|
| AW | awaddr[63:0], awid[15:0], awlen[7:0], awsize[2:0], awburst[1:0], awvalid | awready |
| W | wdata[255:0], wstrb[31:0], wlast, wvalid | wready |
| B | bready | bid[15:0], bresp[1:0], bvalid |
| AR | araddr[63:0], arid[15:0], arlen[7:0], arsize[2:0], arburst[1:0], arvalid | arready |
| R | rready | rid[15:0], rdata[255:0], rresp[1:0], rlast, rvalid |

`clk` 上升沿采样 VALID && READY。VALID 遇到反压必须保持，负载也必须保持。
WSTRB 的第 i 位选择 WDATA 的第 i 个字节；窄拍按绝对地址低 5 位选择总线 lane。
AW/AR 的 burst 必须为 1（INCR），size ≤ 5，地址按每拍大小对齐，burst 不跨 4 KiB。
线上 ID 字段保留 16 位，AoU 可传输范围为 1～1023；驱动负责分配、回收和不重复使用活跃 ID。
写数据按 AW 顺序发送，WLAST/RLAST 精确标记末拍。读/写完成沿 B/R 返回。
未提供 AXI lock/cache/prot/user 外部扩展；资源平面由 ID 对 planes 取模。

调用方在构造模块之前设置全局 1 fs 时间分辨率；两个驱动的适配层分别保证其时钟映射。
启动时保持 resetn=false 至少 3 个 AXI 周期，并等待 `ready()` 表示 UCIe 训练完成，
在非采样沿解除复位。运行中重新复位/取消在途事务不在 v1 契约内。

`StorageConfig` 仅包含存储窗口、链路和内存参数、结果目录。
它不包含设备时钟、ELF、TLM payload、CPU 或 gem5 SimObject。
`trace()` 注册 UCIe/AXI2Flit 内部波形，`Signals::trace()` 注册外部 AXI 信号。
`AxiMonitor` 可记录五通道握手与反压稳定性；两个驱动还保留各自的来源事务检查。
`finish(period, &stats)` 可选接收 `AxiMasterStats`，把主端接收／完成数、最大在途数、
容量拒绝数和排空状态写入协议摘要；原有 `finish(period)` 调用方式仍然有效。
调用方必须等所有 B/R 完成后再执行 `finish(directory)`，否则不得判定运行完成。

整个请求必须在 `[base, base+size)` 内，否则返回 DECERR（3）。
不支持的协议结构在入口失败，不通过私有 RAM 或 functional 旁路完成。
有效读写调用在线 mem_sim；其完成时间决定 AXI 返回时机，反压可向前传播。

本项目提供源码级集成接口，不承诺跨 SystemC 版本的二进制 ABI。
gem5 与独立 SystemC 必须分别编译 `storage_axi/*.cc`；它们可以链接各自的
`libstoragestacked_memsim.so`，该库自身不包含 SystemC。

## gem5 的编译配置

`storage_axi/SConscript` 注册公共存储与 `AxiMonitor` 实现。可选环境变量
`STORAGE_LINK_CONFIG` 指向一个包含 `lanes`、`rate_gtps`、`bits_per_symbol`、`tat_ns`
的 JSON 文件，`link_options.py` 验证后为相关源文件设置 UCIe 编译定义。
未设置时使用 `link_config.h` 的默认值。该设置只选择当前驱动构建的参数，不修改公共头文件；
多个驱动可用独立构建目录选择不同链路参数。
