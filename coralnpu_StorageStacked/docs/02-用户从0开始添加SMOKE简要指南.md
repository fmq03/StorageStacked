# 用户从 0 开始添加 SMOKE 简要指南

[返回文档目录](README.md)。

目标：新增一个 `my_smoke` 项目，输入 41，通过外部内存写入、读回和加一，最终得到 42。
先按 [环境配置说明](01-用户环境配置说明.md) 执行一次根目录 `./build.sh`。
下面按顺序操作，使用一个尚不存在的项目目录名。

## 第 1 步：创建项目目录

从仓库根目录进入 `user/`，后续命令都在这里执行：

```bash
cd user
mkdir my_smoke
mkdir my_smoke/src
```

## 第 2 步：创建完整配置

```bash
cat > my_smoke/config.json <<'JSON'
{
  "program": {"type": "smoke", "input": 41},
  "npu": {"clock_mhz": 500},
  "axi": {
    "period_ns": 2, "outstanding": 16, "planes": 2,
    "stalls": true, "replay": false
  },
  "ucie": {"lanes": 16, "rate_gtps": 24, "bits_per_symbol": 1, "tat_ns": 40},
  "memsim": {
    "standard": "hbm4", "channels": 2, "scale": 1,
    "queue": 4, "slots": 8, "response_hold": 0
  },
  "simulation": {"max_ticks": 20000000000000}
}
JSON
```

这个命令只创建 JSON 文本文件。真正的外部内存读写发生在后面执行设备程序时。
各参数含义见环境文档中的配置表。

## 第 3 步：编写设备程序

```bash
cat > my_smoke/src/smoke.cpp <<'CPP'
#include "project_config.h"
#include <cstdint>

int main() {
    auto *input = reinterpret_cast<volatile uint32_t *>(0x90000000u);
    auto *output = reinterpret_cast<volatile uint32_t *>(0x90010000u);
    auto *mailbox = reinterpret_cast<volatile uint32_t *>(0xc0000000u);

    *input = SMOKE_INPUT;
    const uint32_t value = *input;
    *output = value + 1u;
    const uint32_t observed = *output;
    const uint32_t expected = uint32_t(SMOKE_INPUT) + 1u;
    *mailbox = observed == expected ? 0x600d0000u : 0xbad00001u;
    asm volatile("wfi");
    return 0;
}
CPP
```

`SMOKE_INPUT` 由 SDK 根据 JSON 自动写入 `project_config.h`，无需手工创建该头文件。
`mailbox` 用于通知设备完成，不属于外部存储的四笔读写。

## 第 4 步：添加 Makefile

```bash
cat > my_smoke/src/Makefile <<'MAKE'
SOURCES := smoke.cpp
SDK := ../../../coralnpu/sdk
include $(SDK)/app.mk
MAKE
```

入口会给 Makefile 传入 `CONFIG` 和 `OUT`，编译成果是 `$(OUT)/program.elf`。

## 第 5 步：运行

```bash
./run.sh my_smoke --output result/first-run
```

入口自动发现目录，无需登记项目名。终端依次显示编译、仿真、校验，成功后显示：

```text
PASS: my_smoke/result/first-run/report.md
```

## 第 6 步：查看输出

```bash
cat my_smoke/result/first-run/report.md
```

| 查看项 | 默认预期 |
|---|---|
| 输入 | 41 |
| 输出 | 42 |
| 外部访存 | 写输入 → 读输入 → 写输出 → 读输出，共 4 笔 |
| `summary.json` | `passed` 为 true |
| `smoke_summary.json` | 输入、实际输出、独立期望和逐笔链路时间 |

```mermaid
flowchart LR
    A["JSON：input=41"] --> B["Makefile 编译 ELF"]
    B --> C["NPU：写41 → 读41 → 加一"]
    C --> D["写42 → 读42"] --> E["独立校验计算与链路"] --> F["result/first-run/report.md"]
```

修改 `my_smoke/config.json` 后再次执行 `./run.sh my_smoke` 即可，默认创建新的结果目录。
输入采用 uint32；最大值加一会回绕为 0。
若要改变“加一”的计算规则，应改用 `program.type="custom"` 并配置期望输出，
SMOKE 专项校验固定检查加一行为。

## 把地址、指针和数据串起来

`uint32_t` 是占 4 字节的无符号整数类型。
`0x90000000` 是地址，41 是这个地址里要保存的数，两者含义不同。

`*input = SMOKE_INPUT` 把数写到 input 指向的地址；`value = *input` 从该地址读数。
随后向 output 地址写入 `value+1`，再读出 observed。
四次数据读写对应写输入、读输入、写输出、读输出。mailbox 是设备结束通知寄存器。

`volatile` 告诉编译器保留这里的读写操作。实际传输还受设备接口、缓存和桥接粒度影响，
每条语句不一定对应一笔独立的外部 AXI 请求。应到本次日志中查看请求数。

## 先做这三个小检查

1. **不改运算，只改输入**：输入 100，预期 101，运行后看 `smoke_summary.json`。
2. **检查回绕**：输入 4294967295，32 位加一后预期为 0。
3. **检查自己没有看错目录**：打开本次 `input.json`，确认里面保存的是这次输入。

如果把源代码改成“加二”却仍使用 `program.type="smoke"`，固定 SMOKE 校验会拒绝。
自己定义算法时同时设计 `custom` 的输入、输出地址和期望值；
完整路径规则和实验命令见 [改配置、运行程序和看结果](05-配置实验与USER负载详解.md)。
