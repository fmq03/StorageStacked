# VS Code 使用

从 Remote-SSH 窗口打开 `zhongxing.code-workspace` 或 `zhongxing` 文件夹。也可以单独打开 `vortex_StorageStacked` 或 `coralnpu_StorageStacked`，这两个子项目各有编辑器配置。打开上一级目录时，请改用本工作区文件，否则项目设置不会加载。

本次主要验收范围是两个平台的 `user/` 和 `integration/`：20 个 C/C++ 源文件及头文件、gem5 集成 Python 脚本和用户运行 Shell 脚本。另外检查 CoralNPU 原生设备库源码的前端解析及 wrapper 类型定义跳转；不据此宣称覆盖全部 third_party 源码。

F12 / Ctrl+单击跳到定义，Alt+F12 查看定义，Alt+← 返回。推荐扩展已写入 `extensions.json`；远程端已安装 clangd、Python/Pylance 和 Bash IDE。
2026-10-22 核查的工具及八个推荐扩展均已就绪，版本记录见 [依赖核查](editor_dependencies.json)。

## C/C++ 配置

各用户程序分别提取真实构建命令，并在 `.vscode/projects/<平台>/<程序>/sources` 保存各自的生成头文件。Vortex 主机与设备使用各自的架构参数，CoralNPU 用户程序使用 RISC-V 参数。源码头文件优先指向原始 `user` 目录，只有生成的 `project_config.h` 跳到对应程序的专用快照。这个范围禁止用相邻源文件的猜测参数代替真实构建命令。

两个平台各有 `storage_axi::Master` 等同名但不同实现的类型。总工作区和单独打开子项目都通过 `clangd_router.py` 将集成代码、各个独立用户程序交给不同的 clangd 进程，并使用独立的编译数据库，避免跨平台串跳，也避免 smoke 的 `main`、`kernel_main` 等跳到 llm。各程序目录的 `.clangd` 指定对应编译数据库；路由层只转发真实语言服务器响应。

CoralNPU 原生库索引按当前 L0/TCM 变体提取，移除 clangd 不接受的 GCC 路径选项，并保留真实构建的头文件搜索路径。Bazel 的外部依赖指向其持久目录，避免后续编译用户程序时清理 execroot 链接导致 Abseil 等头文件失联。

clangd 固定为 22.1.0，安装在 `.vscode/tools/`，该版本包含 RVV intrinsic 预编译头修复。CoralNPU 的向量头使用此版本提供的头文件，普通标准库沿用真实工具链的头文件；没有通过虚构函数声明或改变硬件 ISA 来消除报错。上游问题与修复：[LLVM #109634](https://github.com/llvm/llvm-project/issues/109634)、[LLVM #171981](https://github.com/llvm/llvm-project/pull/171981)。

## 刷新与验证

修改 `user/*/config.json`、生成头或构建参数后，在总工作区根目录运行：

```bash
bash .vscode/refresh_user_navigation.sh
python3 .vscode/verify_user_navigation.py
```

刷新会准备固定版本的验证工具，编译各用户程序，并提取每个程序的实际编译命令；同时按当前 NPU L0/TCM 配置刷新原生库编译动作和 Chisel 标签。它不重跑平台仿真。生成文件保存在 `.vscode/`，不写入用户结果目录。对应任务是“索引：刷新 user 和 integration”和“索引：逐文件验证 user 和 integration 跳转”。刷新后执行 `clangd: Restart language server`；首次应用新路径时执行 `Developer: Reload Window`。

验证逐个解析全部 20 个 C/C++ 文件，再通过真实 LSP 定义请求检查项目声明、调用、成员、变量及宏，检测跳到另一个平台、另一个用户程序或源码快照的错误；还检查总工作区和单独打开子项目的方式。结果保存为 `.vscode/user_navigation_verification.json`，总览在 `summary`，每个请求都保留源文件、行号和实际目标。该检查不自动操作 VS Code 窗口。

Python 由 Pylance 负责。每个子项目的 `pyrightconfig.json` 隔离同名运行时模块；gem5 的动态导入通过只供编辑器使用的映射连接到真实源码。验证包含 CoralNPU 的 `result_layout`、`rtl_build`、配置与会话模块，检查在总工作区和单独打开 CoralNPU 时都跳到其自身源码。SCons 注入的 `Source`、`SimObject` 等构建全局名称，以及配置中的普通字符串，不等同于源码中的静态函数引用。

Shell 的函数和变量由 Bash IDE 提供跳转。构建、结果和缓存目录不参与自动扫描。源码中 RVV intrinsic 由编译器内建生成，没有普通 C++ 函数体可跳转；已通过完整前端解析验证其类型与调用。

## 其他已有入口

`bash .vscode/refresh_navigation.sh` 还会刷新 AXI、第三方构建元数据、gem5 Python 映射及 Chisel 标签，可能需要等待构建锁。`verify_navigation.py`、`verify_hdl_navigation.cjs` 保留为依赖库和 RTL 的补充检查，不替代上述 user/integration 验证。

Verilog/SystemVerilog 使用 SystemVerilog 扩展；Scala/Chisel 使用 ctagsx 的名称索引。补充验证包含 `SCore → UncachedFetch → FetchL0Cache` 的真实定义提供器查询，记录在 `hdl_navigation_verification.json`；不等同于整个 RTL 的类型检查或综合验证。

用户结果已分类为 config/logs/build/traces/checks/views。编辑器生成头和数据库使用 `.vscode/projects/` 的独立快照，不依赖某个历史 result 目录；改配置或生成 RTL 后仍需执行上述刷新。新增文档入口为 [result 文件导读](../coralnpu_StorageStacked/docs/06-result文件导读.md) 和 [项目更新说明](../coralnpu_StorageStacked/docs/07-项目更新说明.md)。
