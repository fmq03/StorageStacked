# build.sh：第一次构建和重新构建

对应原文件：[vortex_StorageStacked/build.sh](https://github.com/hy2581/vortex_StorageStacked/blob/977cfe8acb737d2f99a6304135a861ba8112ca9c/build.sh)。

运行 `build.sh` 会检查公共存储项目的位置，准备仿真平台，再编译自带的用户程序。

## 输入与输出

输入为 `--storage`、`--jobs`、`--test`，以及内部路径配置。
`--storage` 必须是相对于仓库根目录的路径，并且能找到公共项目的 `storage_axi/storage_config.hh`。
`--jobs` 支持 1..256，设置编译并行度。

输出为 gem5/Vortex/公共存储组成的平台，以及 smoke、llm 的 `host.elf`、`program.elf`、`program.vxbin`。
平台缓存位于 `third_party/.cache/`，用户编译出的文件位于 `user/项目/result/build/`。

## 脚本运行时依次做什么

| 步骤 | 作用 |
| --- | --- |
| 解析命令行 | 读取选项，检查缺失参数，提供 --help。 |
| 更新 paths.json | 校验依赖路径和 jobs，再保存到 third_party/gem5/runtime/paths.json。 |
| 获取构建锁 | 对 third_party/.cache/build.lock 加独占锁，保护平台缓存。 |
| setup.sh + environment.sh | 准备工具并加载内部编译、运行环境。 |
| build.py --force | 按 user/smoke/config.json 构建 gem5/Vortex 和存储平台。 |
| 遍历 smoke、llm | 分别调用 src/Makefile，CONFIG 指向各自 JSON，OUT 指向各自 result/build。 |
| 解锁并可选测试 | 释放锁；有 --test 时执行一次平台回归，SMOKE 和 LLM 的默认场景各运行一次，没有时打印下一步运行命令。 |

## 后续切换硬件配置

这个入口首先按 SMOKE 的平台配置建立环境。日后运行 LLM 或其他用户项目时，
`user/run.sh` 会检查该项目所需平台是否就绪，必要时构建匹配版本。
因而两个项目的 JSON 可以独立保存自己的参数，不能将首次构建的参数当作所有后续运行的参数。

```mermaid
flowchart LR
  A["相对依赖路径与 jobs"] --> B["paths.json"] --> C["获取锁并准备工具"]
  C --> D["构建 gem5 + Vortex + 存储平台"] --> E["编译 smoke 与 llm"] --> F["释放锁"]
  F --> G["可选回归测试"]
```

## 常用命令

在原仓库根目录执行：

```bash
./build.sh --storage ../axi_StorageStacked --jobs 12
./build.sh --test
cd user
./run.sh smoke
```

## 第一次构建和以后运行的区别

`build.sh` 负责准备公共平台，让编译器、设备模型和存储库能一起工作。
`user/run.sh` 负责具体一次任务，包括新建结果目录、运行和验收。
构建完成后看到 ELF，只说明程序文件已经产生，仍需执行任务得到实际输出。

在原项目根目录，可以按以下顺序操作：

```bash
./build.sh --help
./build.sh --storage ../axi_StorageStacked --jobs 4
./user/run.sh smoke
```

`--storage` 从仓库根目录解析；上例指向旁边的公共存储目录。
`--jobs 4` 限制现实机器上的并行编译任务数。它不改变模拟处理器核数。

## 改了哪些文件需要重新构建

| 改动 | 推荐操作 |
|---|---|
| 用户输入或用户 C++ 程序 | 重新运行对应 user 项目 |
| integration、设备源码、公共 AXI/内存源码 | 执行根目录构建，再运行相关检查 |
| 仓库移动位置、依赖路径改变 | 重新保存相对路径并构建 |
| 想做完整平台回归 | `./build.sh --test` |

配置切换与源码编辑有区别：自动平台检查识别配置，不保证识别所有源码变动。
修改平台实现后不要只靠旧缓存运行。

## 构建失败先查哪里

Vortex 的构建输出直接显示在终端。先区分 setup 的依赖准备错误、gem5 编译错误，
还是用户 host/kernel 编译错误。用户运行中的构建细节则保存在该次 `build.log`。

工具还没准备齐时，离线开关不能凭空补齐依赖。
首次准备环境与日常重跑的工作量不同；应按报错定位缺少的工具或文件。
