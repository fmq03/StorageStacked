> 本目录现为主仓库普通源码，改动与其他内部模块一起提交和合并。
> 当前 CPU/GPU/NPU 均已接入在线 mem_sim；统一入口及协作方式见
> [环境](../env/README.md)和[开发说明](../docs/development.md)。

> 2026-09-11：前端已改为原生 AXI256（32B/拍），见
> [宽度验证](../integrate_doc/16_native_axi256.md)。旧结果已按后续要求清理，当前报告保留。
> HTML按需分块加载，复制整个用例目录即可交接，见[可视化说明](../integrate_doc/17_visualization.md)。
> 新增在线 mem_sim 后端，完整入口为根目录 `env/run_memsim.sh`。
> 手动配置需 `--backend aou --memory-backend memsim --mode cpu`；默认 ram 仍是旧路径。
> 统一环境见 [env/README.md](../env/README.md)，完整验收见
> [14_online_memsim.md](../integrate_doc/14_online_memsim.md)。下文原测试内存说明保留。

# gem5 → AXI SystemC 原型

当前默认使用 gem5_new 锁定的官方 gem5 v25.1.0.1 / c8222cc67a399bfc01e8658dd14b30d5bfd634f9，
统一入口见 [环境与验收](../env/README.md)。通过 EXTRAS 加载本目录和 gem5_new 的观察器，
使用 gem5 内置 SystemC，应用 masked-write 和 VCD 整数字段补丁。
旧 zhongxing revision 2721ed751eda 仅用于显式 `AXI_PROFILE=legacy` 的历史对照。

    gem5 AxiPacketTester ────────────────┐
                                        ├→ Gem5ToTlmBridge64
    gem5 X86TimingSimpleCPU → SystemXBar ┘       ⇅ nb_transport
                                          SystemC Master
                                      AW / W / B / AR / R
                                          sc_signal
                                          ├─ SystemC Ram（--backend ram）
                                          └─ 原生 AXI256 → AXI2Flit → UCIe → AouTarget → 内存后端

两种源分别运行。新增 `--backend aou` 路径已纳入联合调试，运行
`bash gem5_axi/scripts/run_aou_tests.sh`；详见[联调说明](../integrate_doc/07_axi2flit_joint_debug.md)。
存储对端仍是测试内存，尚未接 mem_sim，周期数不代表 DRAM 性能。

可以直接打开[运行记录查看页](results/aou/directed/trace_view.html)，按 AXI ID 查看实际
握手、对应 Flit、两端时间戳和完整原始字节；[阅读方法](../integrate_doc/08_observability_and_expected_behavior.md)
包含一笔请求的实测时间线及 GTKWave 使用方法。

## 环境与运行

在项目根目录执行：

    bash gem5_axi/scripts/setup.sh
    bash gem5_axi/scripts/build.sh
    bash gem5_axi/scripts/run_tests.sh

当前推荐入口（含完整 UCIe 路径和延迟反馈验证）：

    bash env/bootstrap.sh
    bash env/build.sh
    bash env/run.sh

新工具环境位于 ~/.local/share/storagestacked-unified/，gem5 构建位于同级 gem5/build/AXI。
下面的环境说明是旧 `AXI_PROFILE=legacy` 路径的历史记录。
旧环境在 ~/.local/share/storagestacked/ 放置源代码、构建产物和 Python venv。
测试先在该Linux文件系统运行，验证通过后复制结果到项目results，避免WSL挂载盘
频繁写VCD带来的明显减速。日志中的trace_dir保留实际执行路径。
setup 校验固定 SHA256，不覆盖没有 revision 标记的已有目录。首次完整编译耗时
较长，默认 6 个任务。可用 AXI_ENV_ROOT、AXI_GEM5_HOME、AXI_CXX、AXI_JOBS 覆盖。

本机为 Ubuntu 20.04、Python 3.8、g++-10、SCons 4.5.2。g++-10 低于 gem5
声明的受支持版本，警告保留在日志，以实测编译/回归为准。不需要 sudo 或外部
libsystemc；系统需要 C++ 工具链、Python 开发头、zlib、静态 libc。
本机缺 ensurepip，setup 优先使用已有 ~/.local/bin/uv 建 venv。

USE_KVM=y 用于满足包内 Python 无条件导入，运行使用 TimingSimpleCPU，不需要
KVM 设备。Ruby、PNG/HDF5/protobuf/capstone 功能本测试未使用。

只运行 CPU 测试：

    source gem5_axi/scripts/env.sh
    mkdir -p gem5_axi/build
    "${AXI_CC:-gcc}" -O2 -static -fno-pie -no-pie gem5_axi/workloads/memory_check.c \
        -o gem5_axi/build/memory_check
    "$AXI_GEM5_BIN" -d gem5_axi/results/cpu gem5_axi/configs/run.py \
        --mode cpu --binary "$AXI_PROJECT_DIR/build/memory_check"
    "$AXI_PYTHON" gem5_axi/scripts/check.py gem5_axi/results/cpu

memory_check 的 0x90000000 指针由模拟配置映射，只在 gem5 中运行，不能直接在
宿主机执行。

## 模块职责

| 文件 | 作用 |
|---|---|
| axi_signals.hh | 五通道 sc_in/sc_out/sc_signal、绑定和波形 |
| axi_master.hh/.cc | 非阻塞 TLM → AXI、准入控制、分 burst、响应聚合 |
| axi_ram.hh/.cc | 独立 AXI RAM，AW/W 分别排队，延迟和背压 |
| axi_demo.hh/.cc | gem5 包装、时钟、复位、属性转换、协议监视 |
| packet_tester.hh/.cc | gem5 Timing RequestPort 测试源、独立参考数据 |
| configs/run.py | 单进程 gem5/内置 SystemC 组装 |
| scripts/check.py | 从握手重建事务、独立字节记分板、时序校验 |
| scripts/report.py | 延迟对照、故障注入、可交互 HTML |
| scripts/audit_wave.py | 直接从VCD重建握手，检查保持、响应依赖和CSV一致性 |
| scripts/patch_gem5.py | 允许桥读取 masked-write 原始数据；字节掩码由转换 hook 保留 |

Master 不依赖 CPU/RAM 实现。Demo 按 backend 参数实例化 RAM 或 AoU 路径；
`aou_backend.hh/.cc` 完成宽度适配、链路及测试内存连接，`scripts/check_aou.py`
核对两侧 AXI 数据、VCD 和端点统计。

## 接口契约与范围

- 地址64bit、数据256bit、WSTRB32bit、ID16bit；普通读写、INCR、SIZE=0..5、最多256 beats，
  burst 不跨4 KiB。父 Packet 长度1..65536 B，非对齐用合法窄传输覆盖。
- 默认4个 active slots，另有最多1个等待准入的 BEGIN_REQ。slot 保留至
  END_RESP。同一父事务各 burst 串行，不同父事务可并发。
- AXI ID 在父事务存活期间独占；释放后可循环复用，不是永久唯一的全局ID。
- AW/W独立握手；W无ID，按AW顺序发送。RID/BID路由响应，最后R/B完成才聚合。
- byte enable 经TLM扩展保留，写转WSTRB，读只更新启用字节。
  requestor/stream/substream保留在扩展和日志内，尚未编码为AXI USER。
- SLVERR/DECERR映射TLM address error及Packet error。不支持的命令返回错误，
  原子、LL/SC、locked RMW不会作为普通读写执行。
- 不支持一致性、exclusive/atomic、FIXED/WRAP、AxCACHE/AxPROT/AxUSER传播、
  多源顺序域、运行中复位、checkpoint/restore和drain/resume。
  不同ID之间的依赖由上游等待响应或单outstanding约束，不用于通用强顺序MMIO。
- functional/debug与RAM共用数据，只允许桥为空时访问。DMI禁用；b_transport
  提供相同的零时间功能访问，不用于性能评估。
- 非阻塞接口针对本版本Gem5ToTlmBridge：BEGIN_REQ返回ACCEPTED；backward
  END_REQ/BEGIN_RESP；零延迟END_RESP后释放。不是通用TLM initiator兼容层。

AXI握手及burst规则参考
[Arm AMBA AXI/ACE规范](https://developer.arm.com/-/media/Arm%20Developer%20Community/PDF/IHI0022H_amba_axi_protocol_spec.pdf)。
gem5行为以包内src/systemc/tlm_bridge/gem5_to_tlm.cc为准。

## 时间规则

instantiate前设置m5.ticks.setGlobalFrequency(10**15)，1ns=1,000,000 ticks。
AXI默认2ns，CPU默认0.5ns。每个AXI上升沿断言：

    sc_time_stamp().value() == gem5::curTick()

BEGIN_REQ保存effective_begin=now+delay；转换hook在gem5清空延时字段前保存
payloadDelay。准入要求active_count<slots且：

    effective_begin + payloadDelay < current_AXI_edge

恰好同沿到达的请求留到下一拍；准入发END_REQ，同沿写出的VALID经delta update
生效，最早下个上升沿握手。

RAM以AR握手或AW/W全部到达且队列消费完为起点。latency=L时，起点+L拍驱动
响应VALID，最早下一拍握手；无背压最小接口延迟L+1拍。读burst后续可每拍一个。
背压等待通过事件/握手自然产生，返回时不重复加延迟。

transactions.csv记录BEGIN_REQ生效、准入、最后AXI响应、END_RESP；
packet_lifecycle.csv记录gem5首次尝试、接受、最终响应接受。重试不覆盖首次
时间；拒收响应不算交付。检查逐笔对应以及分段延迟之和等于端到端延迟。

## 验证入口

- directed：17个父事务，4 outstanding，五通道背压，每笔响应拒收一次并等待
  7ns重试；2080B、4KiB拆分、非对齐、掩码及两笔DECERR。
- serial_l3/serial_l9：单outstanding、无主动背压；每个父事务准入到完成
  的延迟变化应等于其burst数×12ns。
- cpu：真实X86TimingSimpleCPU、SE、无内核/磁盘镜像；程序/栈在host内存，
  目标8KiB不可缓存窗口走AXI。192B写、64B修改、192B读及两组64bit读写，
  共452个Packet。
- period_3ns：改变AXI周期后重复校验。

输出VCD、握手/事务CSV、配置/stats及JSON。SystemC监视器逐沿检查阻塞时字段
稳定；Python独立重建AW/W、检查数据、ID、LAST、边界、响应时序和事务守恒。
另篡改WLAST/WSTRB/RDATA验证检测能力。

Python记分板针对本测试“重叠读写分阶段”的负载，不是同时同地址读写的一致性
检查器。所有检查通过才生成results/report.html和regression_summary.json。
实测情况见[实现与验证记录](../integrate_doc/04_implementation_and_validation.md)。

默认开启的stalls会主动延后AW/W、按固定周期拉低五通道READY，属于协议压力测试。
观察普通传输节奏或进行性能建模时使用--no-stalls；READY仍受真实队列容量约束。
详细波形对照、逐沿分析和末尾采样修复见
[波形专项复核](../integrate_doc/05_waveform_review.md)。

验证真实CPU的运行结果以及下游延迟对CPU执行时间的影响：

    bash gem5_axi/scripts/run_cpu_timing.sh

该脚本关闭人为停顿，分别运行RAM latency=3/9拍，检查452笔目标访问的数据、
AXI握手、VCD，以及CPU结束时间是否增加452×12ns。结果保存在results/cpu_timing。
详见[CPU验证记录](../integrate_doc/06_cpu_execution_validation.md)。
