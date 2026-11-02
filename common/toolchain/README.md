# 开发环境需求与手动配置

StorageStacked 同时包含 Python 脚本、C/C++ 模型和预编译工具。准备环境时，除了 Python 包，还要满足编译器、头文件、动态库和目标架构要求。

本目录的脚本提供一条已经验证过的 Conda/micromamba 配置途径。用户或 AI 也可以自行准备环境；是否可用应由实际编译和验收确认。当前平台入口仍有固定的工具路径约定，手动配置时需要同时满足下面列出的路径和二进制条件。

## 1. 基础环境需要什么

当前锁文件及预编译工具面向 Linux x86-64。在 WSL2 中应使用 Linux 工具；Windows 原生和 macOS 不属于当前已验证的完整流程。

| 需要的环境或工具 | 用途与要求 | 已验证的版本 |
|---|---|---|
| Bash、Git、curl、tar、patch、基础文件工具 | 执行入口、准备源码和解包依赖；离线时需提前备齐下载缓存 | 按宿主提供 |
| C/C++ 编译器、链接器和开发头文件 | 公共存储接入默认使用 C++17，mem_sim 使用 C++20；编译器需支持相应语言特性 | GCC/G++ 13.4.0 |
| CMake 和 CTest | 配置、构建并执行原生模型测试；AXI 和 CoralNPU 集成工程声明 CMake 至少为 3.20 | CMake/CTest 3.31.8 |
| GNU Make、Ninja | Makefile、CMake 和平台构建使用的后端 | Make 4.4.1、Ninja 1.12.1 |
| Python 及其标准库 | 运行配置、编译编排和结果检查；平台脚本使用 `Path.is_relative_to`、带 `filter` 参数的 tar 解包等接口，不能只检查“有 Python 3” | Python 3.12.13 |
| Python 开发头文件和嵌入库 | gem5 构建需要 Python 头文件及匹配的嵌入链接参数；`python3-config` 应对应所选 Python | 与上述 Python 同一套安装 |
| SCons | 构建 gem5 | 4.8.1 |
| micromamba | 固定环境安装脚本及当前 Vortex 环境激活入口使用；自配独立 AXI 时不要求它 | 2.3.3 |
| Boost 头文件 | AXI 独立 CLI 读取 JSON 所用的 Property Tree | 1.85.0 |
| SystemC | 运行 AXI 和链路模型，需与使用方的 C++ 标准、编译器 ABI 和关键宏匹配 | 独立入口默认 2.3.1/C++17；另已验证外部 2.3.4/C++20 |

“已验证版本”是复现基线，并非声称其他版本一定不可用。更换版本后应重新构建、检查和记录结果；CMake 等源码明确声明的下限仍须满足。

完整总验收还使用 CTest 的 `--output-junit` 输出测试证据。选择其他 CMake/CTest 版本时，要同时检查这些命令选项可用，不能仅凭 CMake 配置阶段满足 3.20 的下限判定全部测试入口兼容。

基础 Python 环境中锁定了 `scons==4.8.1`、`packaging==24.2`、`setuptools==75.8.0`、`wheel==0.48.0`。需要生成 gem5 结构图时，还要准备 `pydot==3.0.4`、`pyparsing==3.2.1` 和 Graphviz（基线为 14.1.2）。AXI 自身的运行与检查脚本主要使用 Python 标准库。各包及传递依赖的完整版本见 [conda-linux-64.lock](conda-linux-64.lock)。

uv、venv 或 Conda 都可以用来准备 Python 环境。选择 uv/venv 时，编译器、SystemC、Python 开发库等原生依赖仍需另外提供；Python 包能够导入不能代替原生库加载和编译检查。

## 2. 两个计算平台还需要什么

### Vortex/gem5

- 需要带 Vortex 扩展的 LLVM、RISC-V GNU 工具链及配套 `libc32`、`libcrt32`。普通系统 Clang 不能直接代替专用 GPU 编译器。当前预编译工具固定在上游修订 `86c525b57eecc4fc44b9a4494db02771ea779b52`，归档、大小和其他源码依赖见 [xpu-artifacts.lock.json](../../vortex_StorageStacked/third_party/gem5/runtime/defaults/xpu-artifacts.lock.json)。
- 预编译 LLVM 使用私有 glibc 加载器及 C++ 运行库。当前 [prepare_vortex_llvm.py](../../vortex_StorageStacked/third_party/gem5/runtime/prepare_vortex_llvm.py) 会设置 ELF 解释器和库搜索路径；配套 sysroot 为 glibc 2.34，版本见 [xpu-runtime-linux-64.lock](../../vortex_StorageStacked/third_party/gem5/runtime/defaults/xpu-runtime-linux-64.lock)。基础编译器锁文件里的 sysroot 与这套运行环境用途不同，不能直接互换。
- gem5 使用自身的 SystemC 实现。它接入 gem5 的事件调度并提供额外接口，不能直接链接外部 Accellera SystemC 替代。
- gem5、SimX、主机运行库以及 mem_sim 库需要针对当前源码、架构和编译配置构建。复制旧二进制或仅通过 `--version` 不算运行验收。

现有 Vortex 环境脚本从 `vortex_StorageStacked/third_party/.cache/tools/` 下查找 `toolchain/`、`bootstrap/bin/micromamba`、`xpu-toolchains/` 和 `xpu-sysroot/`；它还会调用 micromamba 激活环境。手动准备后若仍使用原入口，需要保留这些约定，或另行适配入口并完整验收。没有必须安装完整 Miniconda 发行版的要求。

选择模拟的 AArch64 主机时，另需 `aarch64-linux-gnu-g++` 及 `/usr/aarch64-linux-gnu/` 下的加载器和开发运行库；当前平台构建按这个路径检查。默认 x86-64 场景不要求这套交叉工具链。

### CoralNPU

- 需要 Bazel 8.6.0，版本与 `.bazelversion` 一致；同时需要 [WORKSPACE](../../coralnpu_StorageStacked/coralnpu/WORKSPACE) 声明的源码、构建规则及外部仓库。
- RTL 生成和仿真还涉及 Java、Scala/Chisel 和 Verilator。当前 Bazel 配置选择 `remotejdk_11`，其余工具由对应构建规则准备；仅安装一个系统 Verilator 不能证明 Bazel 依赖已经齐全。离线构建须提前准备完整的外部仓库和工具缓存。
- 需要 LZ4 1.10.0 头文件和共享库；当前入口放在 `coralnpu_StorageStacked/coralnpu/.cache/tools/xpu-native/{include,lib}`。下载来源见 [xpu-artifacts.lock.json](../../coralnpu_StorageStacked/coralnpu/runtime/defaults/xpu-artifacts.lock.json)。
- 需要独立 SystemC 和实际生成的 NPU 原生 RTL 模型库，保持与所选模型、编译器和存储接入配置一致。

CoralNPU 当前从 `coralnpu_StorageStacked/coralnpu/.cache/tools/` 下的 `toolchain/`、`xpu-tools/`、`xpu-native/` 和 Bazel 缓存查找依赖。两个平台都没有把“激活任意 Python 虚拟环境”实现为完整工具链替换接口。

mem_sim 的 sanitizer 回归另需 `clang++-18`；普通原生验收缺少它时会明确跳过该项。商业仿真、综合工具不是上述原生计算与存储验收的必需依赖。

## 3. 不运行自动安装脚本，怎样准备环境

### 手动创建或复用固定环境

若已有 micromamba，可自行按锁文件创建环境。以下命令在仓库根目录执行，包缓存和安装位置都在仓库内：

```bash
micromamba --no-rc create -y \
  --root-prefix "$PWD/.cache/toolchain-packages" \
  --prefix "$PWD/.cache/toolchain" \
  --file common/toolchain/conda-linux-64.lock

export SS_TOOLCHAIN_PREFIX="$PWD/.cache/toolchain"
python3 common/toolchain/check.py "$SS_TOOLCHAIN_PREFIX" common/toolchain/conda-linux-64.lock
source common/toolchain/env.sh
```

这只准备基础工具，不会自动补齐上面的 Vortex SDK、私有运行库或 CoralNPU/Bazel 依赖。复用已有安装时可直接设置 `SS_TOOLCHAIN_PREFIX` 后校验、加载，省去创建步骤。`check.py` 严格核对 Conda 包集合，它是固定环境校验器，不是任意自备环境的通用检查器。

### 使用自备编译器和 Python

可以先通过公开 CMake 工程构建独立 AXI 模型，避开 `run.sh` 中自动加载固定工具链的步骤。下面展示配置方式；将示例绝对路径换成实际工具位置，并保证所选 CMake/CTest 已在 `PATH` 中：

```bash
export CC=/absolute/path/to/gcc
export CXX=/absolute/path/to/g++
export PYTHON_BIN=/absolute/path/to/python3

cmake -S axi_StorageStacked -B .cache/axi-manual \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER="$CC" -DCMAKE_CXX_COMPILER="$CXX" \
  -DCMAKE_ASM_COMPILER="$CC" -DPython3_EXECUTABLE="$PYTHON_BIN" \
  -DSTORAGE_BOOST_INCLUDE=/absolute/path/to/boost/include
cmake --build .cache/axi-manual -j 4

export STORAGE_BUILD_DIR="$PWD/.cache/axi-manual"
export PYTHONPATH="$("$PYTHON_BIN" axi_StorageStacked/scripts/storage_manifest.py --pythonpath)"
"$PYTHON_BIN" axi_StorageStacked/scripts/test.py
```

这个例子默认从仓库源码构建 SystemC。使用外部安装时，在 CMake 配置中传入 `STORAGE_SYSTEMC_PREFIX` 和匹配的 `STORAGE_CXX_STANDARD`；头文件和库必须来自同一套安装，库可位于 `lib/` 或 `lib64/`。gem5 仍使用它自己的内核。更换工具链或 SystemC 时使用新的构建目录，避免复用旧 ABI 的缓存。

这说明独立 AXI 存在直接配置与验收入口，不代表任意 Python/编译器组合都已经验证，也不代表 Vortex/CoralNPU 已经支持同样的通用环境接入。自配完整平台时，先满足第 2 节的 SDK、路径和运行库条件，再调整环境适配；保留当前私有运行库是一种可行选择，不要仅因更换 Python 就移除它们。

## 4. 配好以后怎样确认

先核对工具身份和 Python 模块，例如 `"$CXX" --version`、`cmake --version`、`"$PYTHON_BIN" --version`；gem5 构建还应确认所选 Python 的 `Python.h`、嵌入库和 `python3-config` 一致。按实际用途检查 SCons、pydot 等模块，不需要为只运行 AXI 的环境补齐所有平台工具。

遇到原生库问题时，用 `readelf -l` 检查预编译程序的加载器，`readelf -d` 检查库搜索路径，`ldd` 检查依赖是否出现 `not found`；路径存在以后还需通过实际执行验证。`GLIBC_*`、`GLIBCXX_*`、SystemC 或 Python 符号错误通常涉及运行库或编译配置，不能仅靠重复安装 Python 包解决。

最后执行所用模块的实际验收。通过现有入口配置的平台可运行：

```bash
./axi_StorageStacked/run.sh test
./vortex_StorageStacked/build.sh --storage ../axi_StorageStacked --test
./coralnpu_StorageStacked/build.sh --storage ../axi_StorageStacked --test
```

自配 AXI 使用上一节的直接测试命令。自配平台的验收应使用已经适配的环境入口。以新结果目录中的 `summary.json`、计算结果和原始协议检查判定成功；记录实际编译器、Python、SystemC、运行库来源及版本。当前脚本会生成 `environment.json` 保存部分身份信息，额外自配依赖需自行补充记录。

Docker 镜像的构建与容器验收需另行执行，不能由原生环境通过推定。不要在未执行验证时把新环境写成“已兼容”。
