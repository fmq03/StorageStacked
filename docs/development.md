# 原生开发约定

手动准备环境前，先读[开发环境需求与手动配置](../common/toolchain/README.md)，确认基础工具、平台 SDK、原生运行库要求和现有入口的路径约定。

公共基础、组件模型、链路集成和处理器平台分层放置。组件不能依赖兄弟组件；跨组件胶水与全链路测试归链路集成层。与模块格式相关的检查器归模块；不被构建和验收依赖的辅助工具归 `tools/`。

`common/` 保留 SystemC 提供方选择、工具链、通用验证机制和共享协议接口；AXI manifest 的 CMake 读取器位于 `axi_StorageStacked/cmake/`。

基础工具链使用仓库锁定版本，安装可以保持各自独立。SystemC 由宿主或独立调试入口选择，不统一版本或安装位置；库与其使用方的编译器 ABI、C++ 标准和关键宏必须匹配，每个进程只有一套内核，时间基准 1 fs。

每个模块 README 应写明职责、对外接口、构建与测试、文档目录及负责人。模块测试位置由维护者决定；输出目录由调用方指定或加入忽略规则。

目录移动与逻辑修改分开审阅，保持可恢复的基线和变更记录。任何提交、推送与发布都服从用户当前授权。

新运行使用新结果目录；以真实计算、返回字节、AXI/VCD、Flit 和在线内存检查为准，不能用退出码或文件存在代替验收。环境与工具链记录进入结果目录，Docker 仅承担交付，不替代原生开发调试。

## 从干净 checkout 开始

在仓库根目录执行；所有工具安装到仓库自己的 .cache，平台也可保留各自工具前缀。不会改 shell 启动文件或系统包。

```bash
./axi_StorageStacked/run.sh setup
./axi_StorageStacked/run.sh build
./axi_StorageStacked/run.sh smoke
./axi_StorageStacked/run.sh test
./vortex_StorageStacked/build.sh --storage ../axi_StorageStacked --test
./coralnpu_StorageStacked/build.sh --storage ../axi_StorageStacked --test
```

`common/toolchain/conda-linux-64.lock` 是基础工具版本的唯一锁文件；`SS_TOOLCHAIN_PREFIX` 可复用已匹配的安装，`SS_OFFLINE=1` 要求已有完整包缓存。平台的 setup 继续管理各自 SDK、Bazel 与上游依赖。

## 选择 SystemC

AXI 默认从自身 third_party/systemc 构建 2.3.1。外部安装必须同时提供匹配的 include 和 lib/libsystemc；显式错误直接终止。切换环境建议使用新的 `STORAGE_BUILD_DIR`：

```bash
export STORAGE_SYSTEMC_PREFIX=/absolute/path/to/systemc
export STORAGE_CXX_STANDARD=20  # 必须与该库的构建设置匹配
export STORAGE_BUILD_DIR="$PWD/.cache/axi-systemc-custom"
./axi_StorageStacked/run.sh build
./axi_StorageStacked/run.sh test
```

CoralNPU 使用同样两个 SystemC 参数，runner 缓存区分提供方与标准；Vortex 使用 gem5 自己的内核。宿主的 CMake 工程可先声明自己的 target，再通过 `STORAGE_SYSTEMC_TARGET` 接入存储子工程；它与 PREFIX 互斥，头文件与链接依赖由该 target 提供。

兼容组合须记录编译器、SystemC 版本与 C++ 标准，并以相应实际回归为依据；不将尚未执行的组合写为兼容。

组件和全链路独立调试使用调用方选定的兼容环境：

```bash
source common/toolchain/env.sh
export SYSTEMC_HOME=$(./axi_StorageStacked/run.sh systemc-prefix)
export SYSTEMC_CXX_STANDARD=17
make -C components/axi2flit CXX="$CXX" SYSTEMC_HOME="$SYSTEMC_HOME" SYSTEMC_CXX_STANDARD="$SYSTEMC_CXX_STANDARD" preflight
make -C components/ucie-model CXX="$CXX" SYSTEMC_HOME="$SYSTEMC_HOME" SYSTEMC_CXX_STANDARD="$SYSTEMC_CXX_STANDARD" validate
./axi_StorageStacked/run.sh link-test preflight
```

`./axi_StorageStacked/run.sh systemc-prefix` 确保 AXI 构建完成，并在标准输出只返回 SystemC 前缀。默认可重建 `.cache/systemc-debug`；显式 `SYSTEMC_HOME` 或 `STORAGE_SYSTEMC_PREFIX` 优先，错误安装直接失败，不回退；两者同时设置时必须是同一安装，PREFIX 与 TARGET 仍互斥。

`./accept.sh` 通过该入口选择组件自测内核，读取 AXI2Flit、UCIe 和 mem_sim 的实际测试 summary；需要 `passed: true` 且 `executed == expected`。组件证据写到新建的 `results/components-*/`，每次子测试日志保留在 `runs/` 下。mem_sim 使用实际 CTest JUnit 结果；缺少 clang++-18 时 sanitizer 明确跳过。组件测试与宿主程序保持各自独立的进程和内核。

## 调试与结果

每次运行使用新目录。顶层 `summary.json` 记录是否通过；配置、日志、原始 trace、检查结论、视图分别存放。用 `artifact(result, logical_name)` 读取新旧布局；重新验证时使用 `working_result()` 临时恢复生产者的平铺接口。

AXI 输入的 `config.trace_level`、处理器输入的 `simulation.trace_level` 支持 `full`（默认）和 `light`。light 保留实际仿真和返回数据，关闭 VCD 与三份 UCIe 原始帧 CSV，不进行完整链路验收；summary 明确包含 `passed: false, link_verified: false`。smoke/test 等验收要求 full。

`SC_REPORT` 的 msg_type 使用所属组件名，例如 `AXI2Flit`、`UCIe`、`MemSimBackend`；模拟器标准输出与错误流进入 logs/。

## Vortex 的路径敏感性

被模拟主机的命令行、动态库环境与 ELF RPATH 含有实际路径。不同 checkout 或输出目录会改变这些输入，进而可能改变栈布局、缓存计数和时间；不能只把数值时间差作为重构回归。

前后对比必须固定同一份 guest 程序、kernel、库和路径，或在同一路径下运行；不对时间差设置额外容差。比较架构参数或性能时，同样应固定这些 guest 输入。

## 文档检查和交付

```bash
python3 tools/check_links.py
python3 tools/compare_results.py <reference> <candidate> --kind axi
```

链接检查覆盖维护中的 Markdown，排除上游、生成结果和 .vscode（IDE 导航由专用脚本检查）。历史外部报告未随 checkout 提供时保留为文字引用。Docker 在 delivery/ 中负责交付；镜像构建与容器验收须另行记录实际结果。
