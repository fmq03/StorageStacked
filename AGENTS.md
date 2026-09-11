# StorageStacked 集成交接

接手时先读 `integrate_doc/HANDOFF.md` 和 `integrate_doc/09_migration.md`，再按任务查阅其余文档。
这些文件是跨会话交接依据；不要假定能访问先前聊天内容。

- 默认代码改动保留在工作区，不自行 git add / commit / push。用户已收窄本次建仓范围：不提交 integrate_doc，只提交必要的主仓库文件、未改源码的子模块及 axi2flit 的必要忽略规则；gem5_axi、ucie-model 和现有联调源码修改暂留本地。不执行 push。
- 本次及今后由助手创建的提交说明必须使用中文；不改写既有上游提交的说明。
- 工作目录为 /home/fmq03/StorageStacked；原目录 /mnt/d/storagestacked 必须保留，不要清理或删除。迁移时保留五个集成相关代码目录和交接文档，随后用户提供了 gem5_new；不要把它作为多余目录清理。不包含 tt-oca-harness-aou、根目录绘图/PDF/外层压缩包及 Verdi 会话文件。
- 当前路径：gem5 → 原生 TLM bridge → 自写原生 AXI256 Master → AXI256 信号绑定 → AXI2Flit → UCIe → AouTarget → MemSimBackend → 在线 mem_sim（HBM4 Behavior PHY）。WDATA/RDATA=256bit，WSTRB=32bit；TLM Bridge64 名称保留，不限制 payload 长度。用户已随后授权清理旧结果，当前保留 results/axi256-20260911；宽度对照见 integrate_doc/16_native_axi256.md，HTML 按需加载说明见 integrate_doc/17_visualization.md。
- 2026-09-10 用户指定后续使用 gem5_new（用户称 gem_new）的 CPU / Vortex GPU / CoralNPU 集成增量，接自己的链路和 mem_sim；目前 CPU 控制的 Vortex SimX GPU、CoralNPU RTL 已接入本地 AoU/在线 mem_sim 链路。最新交接见 integrate_doc/15_xpu_online.md。
- 当前默认已使用 gem5_new 锁定的 gem5 公开基线 c8222cc67a399bfc01e8658dd14b30d5bfd634f9（v25.1.0.1）。统一入口 env/bootstrap.sh、env/build.sh、env/run_memsim.sh；env/run.sh 保留测试内存回归；用户目录工具环境为 GCC 13.4.0 / Python 3.12.13 / SCons 4.8.1，二进制为 gem5/build/AXI/gem5.opt。旧 2721ed751eda 仅用于显式 AXI_PROFILE=legacy 历史回归；gem5_0730 过期。
- CPU/tester→UCIe→在线 mem_sim 完整链路已验收，见 integrate_doc/14_online_memsim.md、env/README.md 和 results/axi256-20260911/memsim/summary.json。七组链路、14项原生测试、独立字节/命令/DFI/波形校验通过；CPU 内存时间尺度 ×4 使完成时间增加17596ns。HETTrace 使用1fs和在途packet合成ID，CPU目标窗口不可缓存，不能作为GPU/NPU或cache一致性验证。
- 完整 gem5/、coralnpu/、vortex-gpu/vortex/ 已就位，Vortex 的 8 个递归子模块已补齐。主仓库当前登记 6 个直接 submodule：gem5_new、axi2flit、mem_sim、gem5、coralnpu、vortex-gpu/vortex；gem5_axi 和 ucie-model 暂不纳入主仓库提交，源码及 Git 数据保留。
- gem5_new 原 mem_sim 流程是离线重放；当前本地新增 C ABI + SystemC 协议桥，使用 mem_sim 的原生在线完成和数据。NPU 必须使用 env/build_xpu.sh 的 storagestacked_native_cpp=1，移除第二套 SystemC；env/record_xpu.py 同时检查动态依赖和静态符号。本地 mem_sim revision 与 gem5_new 锁文件不同，不擅自 reset/checkout。
- 三源入口：env/bootstrap_xpu.sh、env/build_xpu.sh、env/run_xpu.sh。沿用 GCC13/Python3.12 主环境，另锁定 Vortex RV32 工具链、Bazel8.6/LZ4；私有 glibc2.34 仅供 Vortex LLVM，不替换系统 libc。GPU CP 使用同线程 gem5 Coroutine 等待真实 DMA 响应；NPU 启动在每个 gem5 设备事件只推进一个 RTL 周期。
- run_xpu.py 的 CPU/NPU 共享区、NPU 工作区及 GPU BAR 走完整链路；程序/栈仍在 gem5 本地主存。GPU BAR 在4GiB以上，mem_sim 使用64位稀疏窗口及8通道容量；不是8KiB容量上限。当前未实现任意 functional/atomic 访问、缓存一致性、checkpoint、NPU重复启动或GPU与NPU直接共享缓冲区。
- 联调采用一个进程、gem5 主事件队列及 gem5 原生 SystemC；不要链接第二套 libsystemc 或引入独立时钟推进器。
- 默认 `--backend ram` 是旧测试路径。完整链路必须用 `--backend aou --memory-backend memsim`；`--mode cpu` 才是CPU执行workload，`tester`是gem5定向请求源。
- 复制/迁移必须保留根 .git（含 .git/modules）、子模块的 .git 文件和未提交/被忽略但需保留的文件。integrate_doc 仅保留在本地；gem5_axi 撤回初始提交后保留为尚未提交源码的本地仓库。暂不登记的模块仍可能使用根 .git/modules 内的 Git 数据，不能删除这些数据。旧交付包和仿真输出保留在本地。
- 用户关注实际可观测性：保留 AXI 五通道周期波形、两端带时间戳的完整 Flit 字节日志，以及独立离线校验。
