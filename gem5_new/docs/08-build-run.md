# 从四棵外部源码到可复现实验

按本页顺序执行。命令假定 Ubuntu 24.04 x86-64、Bash，且已经进入本项目根目录；文件系统操作
都在运行 Codex 的服务器上完成。固定版本来自 [upstream.lock.json](../upstream.lock.json)，
源码许可和远端访问条件见 [UPSTREAM.md](../UPSTREAM.md)。

## 1. 环境与目录

仓库自身只包含集成增量。四棵完整源码、工具链、产物目录都需要另外准备。默认布局为：

```text
workspace/
├── zhongxing/                 本项目
├── gem5/                     官方 v25.1.0.1 + 项目增量
├── coralnpu/                 固定源码 + 项目增量
├── mem_sim/                  固定 Git 树或离线源码包
└── vortex-gpu/
    ├── vortex/               固定源码及三个 submodule + 项目增量
    └── vxbuild/              configure 生成的独立构建树
```

默认路径下：

```bash
source scripts/native_env.sh
export HET_JOBS=8
```

`HET_JOBS` 是本页构建并发数，可按机器内存调整。gem5 的并发 C++ 编译和链接较占内存，
小机器应降低到 2～4。不要把全部编译器都默认开到 `nproc`。

使用新目录复现、保护已有设备树时，在 source **之前**设置：

```bash
export HET_DEPS_ROOT="$PWD/build/dependencies"
export GEM5_HOME="$HET_DEPS_ROOT/gem5"
export VORTEX_HOME="$HET_DEPS_ROOT/vortex"
export VORTEX_BUILD="$HET_DEPS_ROOT/vxbuild"
export CORALNPU_HOME="$HET_DEPS_ROOT/coralnpu"
export MEMSIM_HOME="$HET_DEPS_ROOT/mem_sim"
export MEMSIM_BUILD="$MEMSIM_HOME/build"
export MEMSIM_BIN="$MEMSIM_BUILD/hbm_sim"
source scripts/native_env.sh
```

修改 `MEMSIM_HOME` 后应同时修改 `MEMSIM_BUILD/BIN`；环境加载器会保留已经导出的值。

Ubuntu 系统依赖的准备命令（需要服务器的软件包安装权限）：

```bash
sudo apt-get update
sudo apt-get install -y build-essential git patch curl wget ca-certificates \
  python3 python3-dev python3-venv python3-pip cmake pkg-config \
  zlib1g-dev libprotobuf-dev protobuf-compiler libboost-all-dev \
  libhdf5-dev libpng-dev libelf-dev libssl-dev libffi-dev liblz4-dev clang \
  autoconf automake libtool flex bison ninja-build ccache \
  iverilog verilator gcc-riscv64-unknown-elf binutils-riscv64-unknown-elf
```

本项目 Python 工作流统一推荐系统 Python 3.12。C++ writer 用 C++17，hbm_sim 用 C++20。
独立 `vortex_smoke` 使用系统 RISC-V GCC；三源 vecadd 内核使用 Vortex 的专用 LLVM，二者用途不同。

CoralNPU 需要 Bazel。推荐安装 [Bazelisk](https://github.com/bazelbuild/bazelisk#installation)，
并让 `bazel` 命令指向它；进入固定 CoralNPU 树后会读取 `.bazelversion` 的 `8.6.0`。
若服务器已有 Bazel，先在 CoralNPU 树内运行 `bazel --version`，不能拿树外默认版本作为依据。
本机现有工具可以直接复用，不需要 Vivado/Vitis 或 Synopsys 综合工具。

本轮服务器的 dpkg 已有未完成事务，系统安装返回错误。验证没有修复与本任务无关的 dpkg 状态，
而是把 LZ4 开发包解到 `build/repro-20260908/lz4-sysroot`，通过下面的局部配置编译 CoralNPU。
普通新环境安装好 `liblz4-dev` 后无需这些选项：

```bash
mkdir -p "$HET_PROJECT_ROOT/build/lz4-download"
cd "$HET_PROJECT_ROOT/build/lz4-download"
apt-get download liblz4-dev=1.9.4-1build1.1 liblz4-1=1.9.4-1build1.1
dpkg-deb -x liblz4-dev_1.9.4-1build1.1_amd64.deb ../lz4-sysroot
dpkg-deb -x liblz4-1_1.9.4-1build1.1_amd64.deb ../lz4-sysroot
export HET_LZ4_SYSROOT="$HET_PROJECT_ROOT/build/lz4-sysroot"
# 完成下文的源码获取与 make install 后，在 CoralNPU 树内代替普通 bazel build：
cd "$CORALNPU_HOME"
bazel build --jobs=8 \
  --repo_env="CPLUS_INCLUDE_PATH=$HET_LZ4_SYSROOT/usr/include" \
  --action_env="CPLUS_INCLUDE_PATH=$HET_LZ4_SYSROOT/usr/include" \
  --host_action_env="CPLUS_INCLUDE_PATH=$HET_LZ4_SYSROOT/usr/include" \
  --linkopt="-L$HET_LZ4_SYSROOT/usr/lib/x86_64-linux-gnu" \
  --host_linkopt="-L$HET_LZ4_SYSROOT/usr/lib/x86_64-linux-gnu" \
  //gem5int:libcoralnpu-gem5.so //gem5int:ddr_touch.elf
cd "$HET_PROJECT_ROOT"
```

`--repo_env` 使 Bazel 工具链探测能识别该 include 路径；单独传一个仓库外绝对 `-I` 会被
Bazel 的 execution-root 检查拒绝。这是本机无系统安装的替代路径，包版本针对上述 Ubuntu 基线。

## 2. 添加四棵外部源码（自动入口）

先获取交付的 mem_sim 源码包，放到任意本机目录；它是单独附件，不由本项目 Git clone 自动带入。
在接收方执行：

```bash
python3 scripts/upstreams.py fetch \
  --memsim-archive /path/to/mem_sim-7945650579c44713ddec80acc2c82ae34e1f19a5.tar.gz
python3 scripts/upstreams.py check
```

该入口依次获取 gem5、Vortex（含递归 submodule）、CoralNPU，并校验、导入 mem_sim。
Git 下载禁止弹出交互登录，失败会给出明确的离线/权限提示。已有源码树只检查版本，不自动
checkout、reset 或覆盖；相同版本的项目补丁可保留。缺 submodule 的已有 Vortex 树需要按下一节
补齐后重试。`check` 仅核对源版本（快照还检查清单文件是否存在），不证明文件内容或编译产物正确。

期望出现 `ok ... revision ...` 和 `ok memsim snapshot ... (160 files)`。源码包声明的 commit
不符时不导入，不创建目标树；只使用可信来源的包。仅获取某一棵可加 `--only gem5|vortex|coralnpu|memsim`。

## 3. 四棵源码的显式 Git/导入命令

下面与自动入口二选一；目标目录必须不存在。若已存在，请先检查 `git status` 并选一个新目录。

### 3.1 gem5

```bash
git clone --branch v25.1.0.1 --depth 1 https://github.com/gem5/gem5.git "$GEM5_HOME"
git -C "$GEM5_HOME" checkout --detach c8222cc67a399bfc01e8658dd14b30d5bfd634f9
git -C "$GEM5_HOME" rev-parse HEAD
```

输出必须为 `c8222cc67a399bfc01e8658dd14b30d5bfd634f9`。旧文档的 `2721ed...` 是私有 fork
提交，不能在官方仓库 checkout。已有浅 clone 缺对象时先
`git -C "$GEM5_HOME" fetch origin tag v25.1.0.1`，不要用清空工作树的方式解决。

### 3.2 Vortex 与 submodule

```bash
git clone https://github.com/vortexgpgpu/vortex.git "$VORTEX_HOME"
git -C "$VORTEX_HOME" checkout --detach d76b7f24e658867ab57e3942d7c648c3e6af072d
git -C "$VORTEX_HOME" submodule sync --recursive
git -C "$VORTEX_HOME" submodule update --init --recursive
git -C "$VORTEX_HOME" submodule status --recursive
```

`softfloat`、`ramulator`、`cocogfx` 的 commit 必须与 lock 相同，状态前缀不应出现 `-`（未初始化）
或 `+`（版本不同）。Ramulator 是 SimX 的依赖，不能省略。

### 3.3 CoralNPU

```bash
git clone https://github.com/google-coral/coralnpu.git "$CORALNPU_HOME"
git -C "$CORALNPU_HOME" checkout --detach fcb74cfe79dbd184b9c53539490994e701981f80
git -C "$CORALNPU_HOME" rev-parse HEAD
cd "$CORALNPU_HOME"
bazel --version
cd "$HET_PROJECT_ROOT"
```

该基线的 `.bazelrc` 启用 WORKSPACE、关闭 Bzlmod，首次构建按 WORKSPACE 及其引用的 `.bzl`
下载依赖与工具链，不能仅查看 MODULE.bazel 判断实际版本。本轮 CoralNPU 使用 Bazel 构建的
Verilator 5.050，与系统命令 `verilator` 的 5.020 不同；“Git clone 完成”不等于离线构建环境齐备。

### 3.4 mem_sim

无登录方式使用与第 2 节相同的固定源码包：

```bash
python3 scripts/upstreams.py fetch --only memsim \
  --memsim-archive /path/to/mem_sim-7945650579c44713ddec80acc2c82ae34e1f19a5.tar.gz
python3 scripts/upstreams.py check --only memsim
```

源码持有方生成附件的命令是 `python3 scripts/upstreams.py package-memsim`，见
[版本与访问说明](../UPSTREAM.md)。若获得有权访问的 Git 镜像，也可：

```bash
python3 scripts/upstreams.py fetch --only memsim --memsim-url /path/to/authorized/mem_sim.git
```

这里的路径需要换成实际的镜像位置；脚本仍检出 `794565...`。不要把原来返回 401 的地址当作
公开下载入口，也不要把访问令牌写进文档或 clone URL。

## 4. 配置 Vortex 工具链并安装增量

先生成 Vortex build tree：

```bash
export HET_TOOLDIR="${HET_TOOLDIR:-$HOME/tools}"
mkdir -p "$VORTEX_BUILD"
cd "$VORTEX_BUILD"
"$VORTEX_HOME/configure" --xlen=32 --tooldir="$HET_TOOLDIR"
```

若指定目录尚无工具链，在专用下载工作目录运行：

```bash
mkdir -p "$VORTEX_BUILD/toolchain-download"
cd "$VORTEX_BUILD/toolchain-download"
TOOLCHAIN_REV=v3.0 OSVERSION=ubuntu/focal TOOLDIR="$HET_TOOLDIR" \
  "$VORTEX_BUILD/ci/toolchain_install.sh" --llvm --libc32 --libcrt32 --riscv32
```

脚本位于 **configure 生成的 build tree**；源码树中对应的是 `.sh.in` 模板。上游安装器会替换
所选组件目录，因此已有其他项目共用 `$HOME/tools` 时复用现有组件，或为本项目选择新的
`HET_TOOLDIR` 并重新 configure；不要在有其他文件的目录执行下载器。

然后安装本项目增量：

```bash
cd "$HET_PROJECT_ROOT"
make install
```

输出应依次为 `[1/4] Vortex 项目增量`、`[2/4] Vortex gem5 SimObject`、
`[3/4] 统一 gem5 增量`、`[4/4] CoralNPU 项目增量`。它只安装源码，不编译四棵外部树。
项目 patch 的维护、重复安装和撤销语义见[集成说明](04-integration.md)。

## 5. 构建顺序、命令与成功产物

### 5.1 mem_sim

```bash
cmake -S "$MEMSIM_HOME" -B "$MEMSIM_BUILD" -DCMAKE_BUILD_TYPE=Release
cmake --build "$MEMSIM_BUILD" -j"$HET_JOBS"
"$MEMSIM_BIN" --help
ctest --test-dir "$MEMSIM_BUILD" --output-on-failure
```

生成 `hbm_sim` 和 CTest 测试程序。若切换生成器/编译器，请使用新 `MEMSIM_BUILD`，避免复用
不兼容 CMake cache。离线源码包与 Git clone 使用同一构建命令。

### 5.2 Vortex

```bash
make -C "$VORTEX_HOME/third_party" -j"$HET_JOBS"
env -u DEBUG make -C "$VORTEX_BUILD/sim/simx" \
  USE_GEM5=1 libvortex-gem5 -j"$HET_JOBS"
make -C "$VORTEX_BUILD/sw/runtime/stub" -j"$HET_JOBS"
make -C "$VORTEX_BUILD/sw/runtime/gem5" HOST_ARCH=x86_64 -j"$HET_JOBS"
make -C "$VORTEX_BUILD/tests/regression/vecadd" -j"$HET_JOBS"
```

应得到 SimX `libvortex-gem5.so`、runtime `libvortex.so` 与 `libvortex-gem5-x86_64.so`、
vecadd 的 host 可执行程序和 `kernel.vxbin`。这些对象分别运行在宿主 gem5 进程、被仿真的
Host 用户进程、被仿真的 GPU 中，不可相互替代。

必须分别构建 `runtime/stub` 与 `runtime/gem5`。固定 Vortex 版本的顶层 `runtime all`
会构建 rtlsim/OPAE/XRT 等后端，既需要额外 FPGA/RTL 工具，又**不包含 gem5 driver**；
因此仅执行旧手册中的 `make -C sw/runtime` 在新环境上不能获得完整 gem5 运行产物。

### 5.3 CoralNPU

```bash
cd "$CORALNPU_HOME"
bazel build --jobs="$HET_JOBS" //gem5int:libcoralnpu-gem5.so //gem5int:ddr_touch.elf
cd "$HET_PROJECT_ROOT"
```

成功库位于 `bazel-bin/gem5int/`。跨工具链的 `ddr_touch.elf` 实际位置可能在 `bazel-out/`
的另一配置子目录，验收脚本会查找，不要手写 Bazel cache 的绝对路径。

### 5.4 gem5 与本项目 workload

```bash
cd "$GEM5_HOME"
/usr/bin/python3 -m venv .venv
.venv/bin/python -m pip install -r requirements.txt
.venv/bin/python -m pip install scons
.venv/bin/scons build/X86/gem5.opt -j"$HET_JOBS"
cd "$HET_PROJECT_ROOT"
make -C workloads/shared_buffer
make -C workloads/vortex_smoke
make -C workloads/three_source VORTEX_HOME="$VORTEX_HOME" VORTEX_BUILD="$VORTEX_BUILD"
make preflight
```

gem5 requirements 不应被当作 SCons 已安装的保证；上面显式安装构建工具。预检最终应为
`READY`，并发现 `gem5.opt`、4 个 SimObject params 头、两套设备库、runtime、kernel 和 hbm_sim。
`WARN gem5 使用历史私有 fork` 仅表示兼容旧环境，不表示这是推荐的新安装基线。

## 6. 从回归到真实三源重放

```bash
cd "$HET_PROJECT_ROOT"
make check
make test-storage-chain
make test-memsim-smoke
coralnpuint/tests/run_smoke.sh
gem5int/tests/run_het.sh
gem5int/tests/run_vortex_shared.sh

export HET_RUN_ROOT="$HET_PROJECT_ROOT/build/acceptance-$(date +%Y%m%d-%H%M%S)"
mkdir -p "$HET_RUN_ROOT"
set -o pipefail
HET_THREE_SOURCE_OUT="$HET_RUN_ROOT/traces" \
HET_THREE_SOURCE_M5OUT="$HET_RUN_ROOT/m5out" \
  gem5int/tests/run_three_source.sh | tee "$HET_RUN_ROOT/three-source.log"

python3 scripts/replay_trace.py "$HET_RUN_ROOT/traces" \
  --out "$HET_RUN_ROOT/hbm4" --ticks-per-cycle 1000 --allow-uninitialized
```

`run_three_source.sh` 成功意味着真实功能、来源、共享区、活动重叠和投影检查通过；它本身
不调用 hbm_sim。最后的 `replay_trace.py` 才进行真正重放，并逐请求核对 ID、地址、类型、
状态、时间关系以及逐源请求/字节数；失败返回非零，保留出错阶段日志。

真实 gem5 trace 不包含加载器通过 functional 路径预置的初始内存镜像，重放中会出现
`uninitialized_data`。上面的选项只显式容忍并统计这一状态，表示通过请求/时序审计；
其他错误仍失败。合成 LLM smoke 已产生初始化写，不使用这个选项，继续要求所有 status=ok。
不要用它掩盖真实 workload 数据校验失败。

| 阶段 | 主要输出 | 成功含义 |
|---|---|---|
| 仓库自检 | 终端计数 | Python、writer、地址生成物一致 |
| RTL 测试 | `AXI TRACE PASS` / lint 结果 | 边界协议字段和反压检查通过 |
| gem5 三源 | `host: 全部通过`；3 份 trace/meta；m5out `config.ini`、`stats.txt` | workload 字节和统一观察点验证通过 |
| validate / stats | `validate.txt`、`stats.txt` | 时间、地址、五通道因果与统计 |
| convert | `mem_sim.trace`、`mem_sim.map.csv` | 每个数据拍对应一个请求与来源映射 |
| hbm_sim | `hbm_sim.txt`、`hbm_sim.responses.csv`、`resolved.cfg` | 模型完成与响应原始证据 |
| 对齐审计 | `summary.md`、`trace_manifest.json`、`run.json` | 请求/字节守恒与逐源延迟；记录输入清单、程序路径与命令 |

`replay_trace.py` 要求新输出目录，防止旧 response 混进新结果。`input.cfg` 保留输入配置，
`resolved.cfg` 保存继承和 CLI 覆盖后实际参数；`run.json` 记录执行程序路径和完整命令。
其中的 lock 仅是期望基线，不能拿它冒充该二进制的构建证明；还应保留本轮构建日志。

复用已有 Python 构建环境时，可通过 `GEM5_SCONS=/absolute/path/to/scons` 指定预检使用的
SCons 路径；实际构建也要使用同一命令，Python ABI 保持一致。

较大合成实验另用 `make benchmark-llm-memory`，默认可能运行几十分钟。完整参数与分析见
[实验方法](09-experiments.md)，不要把合成 LLM-like 流当成真实 Transformer 推理。

## 7. 故障定位

| 症状 | 原因检查与处理 |
|---|---|
| gem5 `reference is not a tree` / `pathspec` | 官方树使用 `c8222cc...`；浅克隆先 fetch 对应 tag；历史 fork commit 不存在于官方 remote |
| mem_sim `sign in` / `could not read Username` | 匿名接口不可用；导入固定源码包或使用已有授权镜像 |
| `FAIL ... revision` | 先查看实际 HEAD 与 submodule；脚本不会切换已有目录；使用独立新目录复现 |
| snapshot 版本/文件清单检查失败 | 源包声明版本不符、清单文件缺失或路径无效；保留已有树，在新目录重新导入 |
| SCons/Python link 错误 | 使用系统 Python 创建 venv；确保 python3-dev，避免不同 Python ABI 混用 |
| SimObject 参数不存在 | 重新安装增量，再重编 gem5；只复制 Python 配置不能生成 C++ params |
| Vortex 缺头文件/符号 | 检查 submodule、third_party 构建、专用 LLVM、动态库加载路径 |
| Vortex 队列创建失败 | SE 至少 2 个线程上下文；主配置默认 4 个 CPU |
| NPU 编译缺 `lz4.h` 或链接缺 `-llz4` | 安装 `liblz4-dev`；Bazel 使用的 Verilator 需要其开发头与链接库 |
| NPU 库找不到符号 | Bazel 库和 gem5 适配源码是否来自同一补丁栈；先做 dlopen smoke |
| `dlopen ... __atomic_is_lock_free` | 更新并重装 `coralnpuint/BUILD.bazel`；库已显式链接 `libatomic` 并禁止未解析符号 |
| 功能通过但 trace 为空 | `HETTRACE_DIR`、`--no-axi-trace`、来源启用参数、monitor close 统计 |
| 重放未完成/ID 缺失 | 查 hbm_sim 日志、最大周期、输入单调性与响应队列；不能仅看进程退出码 |

综合/实现工具的长任务按项目 AGENTS 的 30 分钟轮询规则执行；本文是 C++/RTL 仿真构建，
没有启动 FPGA/ASIC 综合流程。
