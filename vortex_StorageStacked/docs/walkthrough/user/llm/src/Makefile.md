# Makefile：用户程序怎样编译

对应原文件：[vortex_StorageStacked/user/llm/src/Makefile](https://github.com/hy2581/vortex_StorageStacked/blob/2014e742e88dcd2d8209c4ddb86c0d6c2d0ce5c2/user/llm/src/Makefile)。

这里列出要编译的源文件，再调用 SDK 的规则生成设备程序。

## 每一项的作用

| 变量 | 取值 | 作用 |
| --- | --- | --- |
| `HOST_SOURCES` | `host.cpp` | 在 GEM5 x86 CPU 上运行的主机程序 |
| `KERNEL_SOURCES` | `kernel.cpp` | 在 Vortex 上运行的 RV32 设备程序 |
| `MODEL_HEADER` | `tinyllm.h` | 模型结构、权重与布局元数据，供编译和模型校验读取 |
| `SDK` | `../../../third_party/vortex/sdk` | 公共编译规则所在目录；路径以当前 src/ 为起点 |

## include 做什么

最后一行 `include $(SDK)/app.mk` 将 SDK 的编译规则纳入当前 Makefile。
这个文件只声明本项目有哪些输入，编译器选择、生成配置头文件和镜像处理由公共规则完成。
新项目沿用这份结构，修改源码文件列表即可复用编译流程。

## 输入与输出

输入：当前 `src/` 的源码、`CONFIG` 指定的 JSON，以及 SDK 的规则。

输出：`host.elf`：GEM5 执行的 x86-64 CPU 程序；`program.elf`：RV32 设备 ELF；`program.vxbin`：供 Vortex 加载的设备镜像。还会记录 `programs.json` 和源文件快照。

`CONFIG` 默认取 `../config.json`，`OUT` 默认取 `../result/build`。
`run.sh` 会改为传入本次运行的 `input.json`，并将产物放入本次结果目录的 `build/`。
`project_config.h` 自动生成，不需要在 `src/` 中手工维护。

## 怎样单独构建

在原项目的本目录执行：

```bash
make CONFIG=../config.json OUT=../result/build
```

前提是已在仓库根目录运行过 `build.sh`，平台工具已经准备好。
单独 make 只编译；完整运行与结果检查通过 `user/run.sh` 完成。

```mermaid
flowchart LR
  A["源码列表与模型头文件"] --> C["Makefile + SDK/app.mk"]
  B["CONFIG 指定的 JSON"] --> C
  C --> D["生成 project_config.h"] --> E["编译与链接"]
  E --> F["OUT 指定的目录"]
```

## 用目录层数算一次 SDK 路径

当前文件位于 `user/llm/src/`。
三个 `..` 依次回到 llm、user、仓库根目录，再进入 SDK。
所以复制新任务时，只要仍是 `user/新任务/src/`，这个相对层数不变。

`HOST_SOURCES` 列出 CPU 源码，`KERNEL_SOURCES` 列出 GPU 源码。两者分别编译成不同指令集的程序。

Makefile 中 `:=` 表示给变量赋值，`$(变量名)` 取变量的内容。
`include` 是把公共构建规则读进来；真正编译用的参数在 SDK 后端继续生成。

## CONFIG 与 OUT 的一次实际展开

从 src 看，`CONFIG=../result/first/input.json` 指向本次固定输入；
`OUT=../result/first/build` 指向本次产物目录。
运行入口传入这些值，所以不用手工改 Makefile 来切换每次结果目录。

`project_config.h` 来自 JSON，Vortex 会在本次 `build/sources/` 保留生成头文件与源码副本。
如果源码报缺少 `SMOKE_INPUT`，先核对 program.type 与源码是否一致。
LLM 的 `MODEL_HEADER` 必须指向 src 内的模型头文件；
增加新 .cpp 时，也要把它加入相应的源码变量，不能只把文件放进目录。
