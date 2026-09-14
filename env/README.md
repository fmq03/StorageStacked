# 统一构建与运行环境

首次配置和依赖包使用请先看[配置与交接指引](../docs/setup.md)。

当前五个内部模块是主仓库普通源码，外部gem5/Vortex/CoralNPU仍按sources.lock.json锁定。
构建不再对UCIe打补丁；GPU设备源码在gem5_new/gem5int/src/dev/vortex直接维护。

当前已验证的完整链路：

```text
X86TimingSimpleCPU / AxiPacketTester / CPU 控制的 Vortex GPU、CoralNPU
 → gem5_new HetAxiMonitor → gem5 原生 Gem5ToTlmBridge64
 → gem5_axi 原生 AXI256 Master → AXI256 信号/结构体绑定
 → AXI2Flit → 双向 UCIe → AouTarget
 → MemSimBackend → mem_sim MemorySystem / Controller / Behavioral MemPhy
 ← 真实读数据或写完成，沿原链路返回 CPU
```

单进程、gem5 主事件队列、gem5 自带 SystemC，统一 1fs；不链接外部 libsystemc。
CPU 使用 SE 模式，程序和栈在 gem5 主存，`0x90000000` 起的 8KiB 目标窗口不可缓存。
这套 CPU 基线 workload 验证访存和协议闭环，不是 cache 一致性测试。
GPU/NPU 使用下面的三源入口及更大的地址窗口。

当前 WDATA/RDATA 为 256bit，WSTRB 为 32bit；对齐的大请求每拍最多32B，
小请求仍按 SIZE 和字节选通传输，不会把每个8B访问自动合并为32B。
`Gem5ToTlmBridge64` 的名字和 TLM socket 的64参数保留用于原生端口绑定，
不限制 generic payload 长度，也不表示下游 AXI 数据仍是64bit。
AXI2Flit 的 Flit 格式不变，共享格式定义见
[aou_format6.h](../protocol/include/aou_format6.h)。

## GPU/NPU 与 CPU 联合运行

```bash
bash env/bootstrap_xpu.sh
bash env/build_xpu.sh
bash env/run_xpu.sh             # 也可传入一个尚不存在的结果目录
```

这三个入口包含基本环境的准备和构建。无需先单独执行 CPU 的三个命令。
运行 CPU+NPU、CPU+GPU、CPU+GPU+NPU、三源内存时间尺度×4 四组，保留每组完整
波形/Flit/DRAM/DFI/来源日志，执行独立校验、内存 C ABI 大地址测试及原生测试（当前19项）。
`summary.json` 是验收结果；`three/memsim_view.html` 按请求展示全过程。
HTML详情按需加载，推荐按配置指引通过本机HTTP服务打开；复制或交接时请带上同级 `view_store.js`、
`memsim_data/`、`trace_paths_data/`、`trace_flits_data/`，最方便是复制整个用例目录。
请求页每页50个选项、Flit页每页100行，不再将全部字节内嵌到HTML。
当前交接结果入口为 `results/handoff-20260911/report.html`；此前AXI256与monorepo报告保留。

主环境继续固定 GCC13.4/Python3.12.13，不额外安装第二套宿主 C++ 编译环境。
专用依赖在同一个 `SS_DEPS_ROOT` 下管理：

| 子目录 | 内容与锁定依据 |
|---|---|
| cmake-sources | Ramulator构建所需的yaml-cpp、spdlog、argparse，源码提交和压缩包SHA256固定在xpu-artifacts.lock.json |
| xpu-toolchains | Vortex v3.0 RV32 LLVM/GNU/libc/libcrt；固定下载提交与各分片 SHA256 |
| xpu-tools | Bazel8.6.0，SHA256见 xpu-artifacts.lock.json |
| xpu-native | GCC13 编译的 LZ4 1.10.0，供 NPU RTL 库使用 |
| xpu-sysroot | glibc2.34及patchelf0.19.1，确切包见 xpu-runtime-linux-64.lock |
| bazel | CoralNPU 的可重建依赖/编译缓存，包括项目锁定的 Verilator 和交叉工具链 |

上游标注 focal 的 Vortex LLVM 二进制实际需要 glibc2.34，本机系统为2.31。
`prepare_vortex_llvm.py` 给这些工具写入私有 interpreter/RPATH；不改变系统 libc，
不把该 glibc 加入 gem5 的 LD_LIBRARY_PATH。私有环境里的 GCC16运行库是 patchelf
自身依赖，不是构建仿真器的编译器。宿主编译仍用主环境 GCC13。
CoralNPU 使用 Bazel `--define=storagestacked_native_cpp=1` 构建纯 C++ Verilator 模型，
`record_xpu.py` 检查加载库中没有动态或静态引入的第二套 SystemC。

新机器还需要 `make`、`patch`、标准 Linux 开发工具；Bazel 首次会下载其已锁定的
Java/Scala/Chisel/Verilator及NPU交叉编译依赖。当前验证平台是 Ubuntu20.04 WSL2 x86-64。
首次 NPU 库构建本机约8.5分钟；已验证构建入口可重复执行。整个四组验证会产生数 GB
波形/日志，主要来自动态链接 CPU host 启动期间的周期采样与 DRAM refresh。

run_xpu.py 复用 gem5_new 的 CPU/cache/PIO/device 创建逻辑，目标缓冲区均经新链路。
地址和设备构成见[运行配置](../gem5_axi/configs/run_xpu.py)，当前限制见[配置指引](../docs/setup.md)。

## 新机器的三个命令

宿主：Linux x86-64、glibc ≥ 2.28、Bash、Git、curl、tar/bzip2，建议 16–32GiB 内存。
本机 Ubuntu 20.04 / WSL2 验证，无需 Docker、物理 GPU 或外部 SystemC。
基础系统工具需预装；项目工具环境安装无需sudo。首次准备需要网络，依赖只安装到用户目录。

```bash
cd /path/to/StorageStacked
# 可选：安装前指定；以后 build/run 保持相同值。
export SS_DEPS_ROOT="$HOME/.local/share/storagestacked-unified"
bash env/bootstrap.sh
bash env/build.sh
bash env/run_memsim.sh
```

`bootstrap.sh` 用 SHA256 固定的 micromamba 2.3.3 安装 `conda-linux-64.lock` 的确切包。
版本为 GCC 13.4.0、Python 3.12.13、SCons 4.8.1、CMake 3.31、Boost 1.85，
含统一的 glibc 2.28 sysroot、binutils 和 C/C++ 运行库。`environment.yml` 记录选版意图；
交接使用 explicit lock，不重新求解。安装目录默认 `~/.local/share/storagestacked-unified/`。

`build.sh` 检查 `sources.lock.json`，应用外部 gem5 补丁并安装系统内的设备源码，先以 C++20 构建
`mem_sim/build-unified/libstoragestacked_memsim.so`，再通过 EXTRAS 构建
`gem5/build/AXI/gem5.opt`，含 gem5_axi 和 gem5_new 的 HETTrace 观察器。
两者使用同一套编译器和运行库；C ABI 隔开 gem5 C++17 与 mem_sim C++20。
默认六个编译任务，可设 `AXI_JOBS`。首次 gem5 全量构建本机约 22 分钟。
HDF5/protobuf/capstone/tcmalloc 为未启用的可选组件，相关构建提示不影响此配置。
KVM 编译支持满足 Python 导入，仿真不使用 `/dev/kvm`。

源码版本：gem5 `c8222cc...`（gem5_new 锁定的 v25.1.0.1）；mem_sim 已同步远端
`c383152...` 并合并在线接口。内部源码版本统一由主仓库提交标识，导入来源见
`env/internal_imports.json`；不再使用旧 gem5_new 锁文件控制内部模块。

## 运行与证据

`run_memsim.sh [新结果目录]` 编译一个无 libc 启动过程的 CPU 校验程序，执行：

- mem_sim 原生测试（当前19项）及 C ABI 掩码/队列/完成测试。
- 五组定向链路测试：默认、双平面 CRC 重放、深度 1 队列、响应保持背压、3ns AXI 时钟。
- 两组 CPU 测试：默认内存时间尺度与放慢 4 倍，比较同一程序的 452 次访问和完成时间。
- AXI 五通道 VCD、两端完整 Flit、DRAM/DFI 字节与时间、最终内存镜像的独立校验。
- HETTrace 时间和字节检查，以及六种 mem_sim 日志篡改、既有 AXI/Flit 故障检查。

新结果默认写到 `results/memsim-<UTC时间>/`，已有目录会拒绝覆盖。
先看 `summary.json` 和 `cpu/memsim_view.html`；后者可按 burst 选择全过程。
若直接打开HTML无法加载数据分块，使用[配置指引中的本机HTTP服务](../docs/setup.md#查看报告)。
`trace_view.html` 查看原 AXI/UCIe 细节，`axi_wave.vcd` 用 GTKWave/Verdi 查看五通道。
`memsim_commands.csv` 是实发 DRAM 命令；`memsim_dfi_signals.csv` 由实发命令及其数据
构建行为级 DFI 轨迹，不是外部 RTL 引脚采样。
`memsim_image.csv` 是原生 MemoryImage 的最终内容；桥本身不维护替代 RAM。
`environment/manifest.json` 记录源码/未提交文件、工具包、二进制和共享库哈希及实际链接库；
内部目录（包括公共 protocol）记录同一个主仓库提交及各自文件哈希，外部目录记录子模块提交。
同目录patch仅记录对应目录的未提交差异；内部导入来源另见env/internal_imports.json。

手动跑一个 CPU 场景：

```bash
source env/activate.sh
# workload 已由 run_memsim.sh 编译在此处。
"$AXI_GEM5_BIN" --listener-mode=off -d results/my-cpu gem5_axi/configs/run.py \
  --backend aou --memory-backend memsim --mode cpu \
  --binary "$PWD/gem5_axi/build/memsim_check" --het-trace
```

参数说明：`--memsim-channels 2`、`--memsim-queue 4`、`--memsim-slots 8` 为默认值；
`--memsim-scale 4` 将宿主时间与 mem_sim tick 的映射放慢4倍，保持原生速率/tCK参数一致；`--memsim-response-hold`
仅用于背压测试，单位是 mem_sim tick，正常值为 0。HBM4 默认 tCK=500ps、tick_multiplier=2，
因此每次原生 step 对应 250ps；主事件队列驱动 SystemC 在该时刻调用一次 step。
每次运行将原生 tCK、缩放后 effective_tCK、周期、容量和解析后的配置写入
`memsim_config.json`。HBM4 preset 含 provisional 时序，具体数量以该文件为准；
用于功能及模型内时序验证，尚未经特定硬件标定。

旧验证入口 `bash env/run.sh [新结果目录]` 继续验证 SimpleBurstMemory/RAM、ID 复用和
观察器透明性。`--backend ram` 仍是底层配置默认值；完整链路必须显式选
`--backend aou --memory-backend memsim`。`--mode tester` 不是 CPU 执行程序。
原 `gem5_axi/scripts/setup.sh/build.sh/env.sh` 默认转入统一环境。
验收脚本显式关闭gem5调试监听，避免交互终端默认打开7000端口后被误连接而停住；
需要调试时另行手动启动gem5并配置监听，不修改正式验收的运行参数。

HETTrace 单独校验：

```bash
source env/activate.sh
python -m hettrace validate /path/to/case/hettrace \
  --allow-single-source --ticks-per-second 1000000000000000
```

HETTrace 使用在途 packet 合成 ID，原 gem5_new 配置默认 1ps 不变；此环境显式使用 1fs。
真实 AXI ID、RP、Flit seq、mem_sim 子请求 ID 各自独立，关联见 `memsim_journeys.json`。
CPU 旧 libc workload 的默认 watchdog 为 10ms；新的 freestanding workload 避免无关启动开销。

## 迁移与交接

五个内部模块和必要的外部适配补丁由主仓库管理。新机器使用
`git clone --recurse-submodules https://github.com/fmq03/StorageStacked.git`，
再执行上述bootstrap/build；构建会将补丁和系统内设备源码安装到外部依赖。
原维护机器上的results和integrate_doc不会出现在新克隆中，文档里的这些路径是本地历史证据。

直接复制工作区时，保留根 `.git/modules`、三个外部子模块的 `.git` 文件及必要的
未提交源码。运行结果与 integrate_doc 不随 clone 分发，需要时另行复制。
禁止删除原 `/mnt/d/storagestacked`。

新机器用锁文件重新创建工具环境并构建，不直接搬 venv 或编译缓存。源码和
`SS_DEPS_ROOT` 可更换路径；迁移时不带 gem5/build、mem_sim/build-unified 等编译目录，
重新构建会写入新的 RPATH。源码树内未提交文件应完整复制，不要用 Git reset 清理。
离线交付还需准备完整源码和锁文件所列包缓存；GPU/NPU 还需 xpu-downloads 中的
固定工具包、私有 runtime 包及 Bazel 已锁定外部依赖。完整离线重建尚未验收。

历史环境仅用于对照：在新 shell 中设置 `AXI_PROFILE=legacy` 再使用旧脚本。
不要将旧 GCC 10/Python 3.8 环境叠加到已激活的新环境。

## 验收记录

最新交接验收见[验收说明](../docs/handoff-validation.md)，本地总报告为results/handoff-20260911/report.html，
覆盖当前工作区完整流程以及新路径/新环境恢复后的构建与闭环。

以下结果仅保留在原维护机器，不随Git克隆分发。此前仓库整合基线：results/monorepo-20260911/summary.json。
新版 mem_sim 的19项原生测试、7组CPU/定向完整链路、4组GPU/NPU及5组RAM兼容场景通过。

- 在线内存：989笔父请求、1784个原生子请求。
  CPU两组各452次访问，内存尺度×4使完成增加16286ns，严格等于逐请求延迟差之和。
- 设备场景：29253笔父请求、29887个原生子请求。
  三源内存尺度×4使GPU周期612→1200、NPU周期5068→8359、CPU完成增加103882ns。
- RAM兼容：5组通过，包含逐拍与负例检查。

此前AXI256基线及可视化仍在results/axi256-20260911/report.html。
本次同步了mem_sim上游并改用配置解析器，相关时序会重算，不能把新旧数字差异归因于仓库布局。
原始波形、字节日志、源码/二进制哈希和分块HTML均保存在各用例目录。
本轮未在另一台干净机器或完整离线环境重建，迁移时仍需依锁文件准备依赖。
