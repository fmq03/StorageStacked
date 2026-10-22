# 从零添加 SMOKE：操作步骤

[返回文档目录](README.md)。

前提：项目已有可用的 AXI 存储链路和通用 `run` 入口。
当前项目已完成以下步骤，可以对照现有文件查看，无需重复添加。

## 第 1 步：准备并构建项目

```bash
# 在 axi_StorageStacked 仓库根目录执行
./run.sh setup
./run.sh build
```

需要调整构建时，使用以下选项；默认配置可以直接运行 SMOKE：

| 参数                       | 默认值  | 修改方式／用途                                                                         |
| -------------------------- | ------- | -------------------------------------------------------------------------------------- |
| `BUILD_JOBS`             | 12      | `BUILD_JOBS=4 ./run.sh build`：限制编译并行数                                        |
| `CMAKE_BUILD_TYPE`       | Release | `./run.sh build -DCMAKE_BUILD_TYPE=Debug`：调试构建                                  |
| `STORAGE_BUILD_CLI`      | ON      | `./run.sh build -DSTORAGE_BUILD_CLI=ON`：构建独立运行器，SMOKE 必须开启              |
| `STORAGE_MEMSIM_LIBRARY` | 空      | 留空时从本项目源码构建；填已有内存库的绝对路径时使用外部库，完整 test 使用默认本地构建 |

## 第 2 步：配置项目并添加测试输入

新建 [examples/smoke.json](../examples/smoke.json)。顶层 `config` 配置本次存储系统，
`transactions` 配置测试访问；没有另一个自动读取的全局 architecture.json。省略的 config 字段使用下表默认值。

### 2.1 项目运行参数：填写在 config 中

| 类别     | 参数                      | 默认值           | 作用和范围                                              |
| -------- | ------------------------- | ---------------- | ------------------------------------------------------- |
| 地址窗口 | `base`                  | `2415919104`   | 起址，即`0x90000000`；0～2^64−1                      |
| 地址窗口 | `size`                  | `805306368`    | 窗口字节数，即 768 MiB；1～2^64−1                      |
| AXI 时钟 | `period_fs`             | `2000000`      | 周期，默认 2 ns；2～10^9 fs，必须为偶数                 |
| AXI 反压 | `response_stall_cycles` | `3`            | 主设备观察到 B/R 有效后额外等待的 AXI 周期数；0～100000 |
| 仿真限制 | `max_ticks`             | `200000000000` | 全局仿真时间上限，默认 200 μs；1～10^15 fs             |
| 链路资源 | `planes`                | `2`            | AoU 资源平面数；1～4                                    |
| 链路重放 | `replay`                | `false`        | 是否启用链路错误注入／重放检查；布尔值                  |
| 内存标准 | `standard`              | `hbm4`         | 可选 hbm3、hbm4、lpddr5、lpddr6                         |
| 内存通道 | `channels`              | `2`            | 内存通道数；1～64                                       |
| 内存时序 | `scale`                 | `1`            | 内存时间倍率；1～1024，不改变 AXI 时钟                  |
| 内存队列 | `queue`                 | `4`            | 原生内存队列深度；1～1024                               |
| 在途容量 | `slots`                 | `8`            | 内存桥同时持有的 burst 槽位数；1～1023                  |
| 返回延迟 | `response_hold`         | `0`            | 内存完成后额外等待的内存周期数；0～1000000              |

要求 `base+size <= 2^64−1`，窗口大小还须不超过所选原生内存容量。
这些范围是文件入口的检查范围，具体组合仍需运行验收；默认 SMOKE 使用 HBM4。
JSON 参数改变后下一次运行生效，不需要重新编译。

### 2.2 本次访问参数：填写在 transactions 中

| 参数                  | 填写内容                                                 |
| --------------------- | -------------------------------------------------------- |
| `command`           | 必填，read 或 write                                      |
| `address`           | 必填，十进制字节地址；按每拍大小对齐，burst 不跨 4 KiB   |
| `id`                | 事务 ID，1～1023，默认 1                                 |
| `size`              | 每拍字节数的 log2，0～5；默认 5，即 32 字节              |
| `beats`             | 拍数，1～256；读默认 1，写默认 data 数组长度             |
| `data`              | 写数据，一拍一个十六进制字节串                           |
| `strobe`            | 写字节掩码，一拍一个整数；省略表示该拍所有字节有效       |
| `expected`          | 读回期望，一拍一个字节串；可选，仅用于检查               |
| `expected_response` | 期望响应码，默认 0（OKAY）；窗口外访问可指定 3（DECERR） |

按顺序安排六笔事务：

1. 读取 `0x90000000` 起的 64 字节，预期全零。
2. 向同一区域写入 `00..3f`，分两拍，每拍 32 字节。
3. 读回 64 字节，预期与写入一致。
4. 向 `0x9000001c` 写入 `aabbccdd`，设置 `strobe=5`，只更新两个字节。
5. 再读 64 字节，确认只有指定的两个字节改变。
6. 从 `0x9000001c` 读回 4 字节，预期为 `aa1dcc1f`。

完整输入示例直接查看 [smoke.json](../examples/smoke.json)。

### 2.3 其他模块的参数到哪里改

| 模块／参数                 | 当前默认值或配置位置                                                   | 使用方法                                                  |
| -------------------------- | ---------------------------------------------------------------------- | --------------------------------------------------------- |
| AXI 总线宽度               | 数据 256 位、地址 64 位、ID 信号 16 位；`storage_axi/axi_signals.hh` | 编译期接口，修改需同步协议／适配代码并重建                |
| 全局时间分辨率             | 1 fs；`examples/axi_storage.cc`                                      | 接口要求，保持该值                                        |
| UCIe lane 数               | `AOU_LINK_LANES=16`                                                  | 在下方 link_config.h 调整编译配置，重建后验收             |
| UCIe 每 lane 符号率        | `AOU_LINK_RATE_GTPS=24.0` GT/s                                       | 同上                                                      |
| UCIe 调制                  | `AOU_LINK_BITS_PER_SYMBOL=1`，即 NRZ；2 为 PAM4                      | 同上                                                      |
| 信用返回时间预算           | `AOU_LINK_TAT_NS=40.0` ns                                            | 同上                                                      |
| 重放注入概率               | replay=false 时为 0，true 时为 0.02；`storage_axi/aou_backend.cc`    | 当前 JSON 只有开关，改变概率需修改源码并重建              |
| 原生内存组织／时序         | `config/memory/hbm.cfg`、`lpddr.cfg` 的 organization、dram.timing  | 用于 mem_sim 独立实验，包含层数、bank、行列、数据率和时序 |
| 原生控制器／PHY            | 同一 cfg 的 controller、phy、refresh 等节                              | 用于独立实验，包含调度、队列、地址映射、PHY 延迟和刷新    |
| 存储后端／可靠性／功耗／热 | 同一 cfg 的 storage、reliability、power、thermal 等节                  | 用于独立实验，具体字段和默认值查看模板                    |

链路编译配置见 [link_config.h](../axi2flit/systemc/include/link_config.h)；
原生内存模板见 [hbm.cfg](../config/memory/hbm.cfg)、[lpddr.cfg](../config/memory/lpddr.cfg)，
完整字段见 [cfg 指南](../mem_sim/文档/cfg指南.md)。
**AXI 在线链路不读取这些 cfg**；它通过 [online.cpp](../mem_sim/integration/online.cpp) 用内置标准和
上面的 JSON 参数创建内存。要让更多原生参数影响 AXI 链路，需要扩展配置传递并重建，不能只改 cfg。

## 第 3 步：先用通用入口验证输入

```bash
./run.sh run --input examples/smoke.json
```

查看终端给出的结果目录，确认 `summary.json` 中 `passed` 为 `true`。

做参数实验时，复制输入并修改副本的 `config`：

```bash
cp examples/smoke.json examples/my_smoke.json
./run.sh run --input examples/my_smoke.json
# 也可以临时覆盖内存倍率，不改 JSON：
./run.sh run --input examples/my_smoke.json --scale 4
```

`--input` 选择输入，`--output` 指定新结果目录，`--scale` 和 `--replay` 覆盖当次配置。
重放验证建议运行完整 test 的长序列；六笔短输入不保证实际触发重放。
专用 `./run.sh smoke` 只支持 `--output`，使用固定向量和覆盖计数；自定义参数用通用 run 验证。

## 第 4 步：添加 SMOKE 检查脚本

新建 [scripts/smoke.py](../scripts/smoke.py)，完成以下工作：

1. 读取 `examples/smoke.json`。
2. 调用现有 `scripts/run.py` 的 `run_case()`，执行真实仿真和链路检查。
3. 检查六笔事务、读回数据、掩码和反压；握手数应为 AW=2、W=3、B=2、AR=4、R=7。
4. 检查 10 个内存子请求全部完成，RD=7、WR=3，命令和 DFI 错误为零。
5. 写出 `smoke_summary.json`，并在 `summary.json` 中标记 `smoke_passed`。
6. 任何检查失败时，将两个摘要的 `passed` 都设为 `false`，并非零退出。

复用现有脚本实现，保留“结果目录不能覆盖”和“不能关闭 Python 断言”的检查。

## 第 5 步：添加统一命令

修改 [run.sh](../run.sh)：

- 把 `run|test` 分支扩展为 `run|test|smoke`，沿用现有 Python 脚本转发逻辑。
- 在帮助信息中加入 `smoke`。

## 第 6 步：接入完整回归

修改 [scripts/test.py](../scripts/test.py)：

- 导入 `from smoke import run_smoke`。
- 将 `cases = {}` 改为 `cases = {'smoke': run_smoke(output/'smoke')}`。
- 保留原有场景和汇总逻辑。

## 第 7 步：运行 SMOKE 并查看输出

```bash
./run.sh smoke
```

在终端给出的新结果目录中查看：

- `input.json`：本次实际输入。
- `responses.json`：实际读回数据、响应状态和完成时间。
- `smoke_summary.json`：SMOKE 覆盖和检查结果。
- `summary.json`：整个运行的验收结果。

核对参数是否生效：查看 `input.json.config` 的完整输入，以及 `memsim_config.json` 中实际内存标准、
通道数、周期、队列与窗口；AXI 时钟见 `protocol_summary.json.period_ticks`，单位 fs。

要求两个摘要的 `passed` 均为 `true`，且 `summary.json` 中 `smoke_passed` 为 `true`。
最后一笔读回应为 `aa1dcc1f`。

## 第 8 步：检查失败情况并运行回归

用输入副本故意修改读回期望，确认检查失败；再把部分写改成全写并调整对应期望，确认 SMOKE 仍会因缺少掩码覆盖而失败。

```bash
./run.sh test
```

确认总摘要和包含 SMOKE 在内的八个场景全部通过。
上述命令默认创建新结果目录；指定 `--output` 时也必须使用不存在的目录。

字段详情见 [输入说明](INPUTS.md) 和 [输出说明](OUTPUTS.md)。

## 日常新增输入与开发专项检查的区别

只是想试自己的地址、数据或内存参数时，复制 `examples/smoke.json` 为新文件，
再执行 `./run.sh run --input 新文件` 即可。无需重新添加 `scripts/smoke.py` 或修改 `run.sh`；
前面的第 4～6 步说明固定 SMOKE 功能如何接入，目前已经存在。

| 你的目标 | 需要改哪些内容 |
|---|---|
| 多写几拍，读回来验证 | 新 JSON 的 `transactions` 与读回期望 |
| 观察慢内存或更长等待 | 新 JSON 的 `config` |
| 固定检查新的协议场景 | 输入向量、专项检查脚本、测试入口及结果说明 |
| 接入自己的 AXI 主设备 | SystemC 信号连接和驱动 |

专项检查应能拒绝“不完整但看起来成功”的输入。
原 SMOKE 要求实际出现部分字节写；如果把掩码改成全写并相应修改 expected，
通用读写仍可能正确，但已不再覆盖“未使能字节保持原值”，专项检查就应失败。

先跑通最小输入的步骤见 [独立负载入门](01-环境配置与独立负载入门.md)。
