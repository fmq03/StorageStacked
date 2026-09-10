# StorageStacked 集成交接

接手时先读 `integrate_doc/HANDOFF.md` 和 `integrate_doc/09_migration.md`，再按任务查阅其余文档。
这些文件是跨会话交接依据；不要假定能访问先前聊天内容。

- 默认代码改动保留在工作区，不自行 git add / commit / push。用户已收窄本次建仓范围：不提交 integrate_doc，只提交必要的主仓库文件、未改源码的子模块及 axi2flit 的必要忽略规则；gem5_axi、ucie-model 和现有联调源码修改暂留本地。不执行 push。
- 本次及今后由助手创建的提交说明必须使用中文；不改写既有上游提交的说明。
- 工作目录为 /home/fmq03/StorageStacked；原目录 /mnt/d/storagestacked 必须保留，不要清理或删除。迁移时保留五个集成相关代码目录和交接文档，随后用户提供了 gem5_new；不要把它作为多余目录清理。不包含 tt-oca-harness-aou、根目录绘图/PDF/外层压缩包及 Verdi 会话文件。
- 当前路径：gem5 → 原生 TLM bridge → 自写 AXI64 Master → AXI256 适配 → AXI2Flit → UCIe → AouTarget → 测试内存。
- 2026-09-10 用户指定后续使用 gem5_new（用户称 gem_new）的 CPU / Vortex GPU / CoralNPU 集成增量，接自己的链路和 mem_sim；目前 GPU/NPU/mem_sim 尚未接入本地 AoU 链路。
- 新系统按 gem5_new/upstream.lock.json 分析，gem5 公开基线为 c8222cc67a399bfc01e8658dd14b30d5bfd634f9（v25.1.0.1）；旧 2721ed751edac7d4cf3df574c6e0293343a14ba2 只用于已通过的历史回归，尚未切换构建。gem5_0730 是过期版本。环境分析见 integrate_doc/10_unified_environment_plan.md。
- 完整 gem5/、coralnpu/、vortex-gpu/vortex/ 已就位，Vortex 的 8 个递归子模块已补齐。主仓库当前登记 6 个直接 submodule：gem5_new、axi2flit、mem_sim、gem5、coralnpu、vortex-gpu/vortex；gem5_axi 和 ucie-model 暂不纳入主仓库提交，源码及 Git 数据保留。
- gem5_new 当前 mem_sim 流程是离线重放；其 NPU 库记录了静态带入第二套 SystemC 的问题，需处理后才能满足本项目约定。本地 mem_sim revision 与 gem5_new 锁文件不同，不擅自 reset/checkout。
- 联调采用一个进程、gem5 主事件队列及 gem5 原生 SystemC；不要链接第二套 libsystemc 或引入独立时钟推进器。
- 默认 `--backend ram` 是旧测试路径。完整链路必须用 `--backend aou`；`--mode cpu` 才是CPU执行workload，`tester`是gem5定向请求源。
- 复制/迁移必须保留根 .git（含 .git/modules）、子模块的 .git 文件和未提交/被忽略但需保留的文件。integrate_doc 仅保留在本地；gem5_axi 撤回初始提交后保留为尚未提交源码的本地仓库。暂不登记的模块仍可能使用根 .git/modules 内的 Git 数据，不能删除这些数据。旧交付包和仿真输出保留在本地。
- 用户关注实际可观测性：保留 AXI 五通道周期波形、两端带时间戳的完整 Flit 字节日志，以及独立离线校验。
