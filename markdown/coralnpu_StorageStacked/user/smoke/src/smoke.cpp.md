# smoke.cpp：NPU 怎样把 41 变成 42

对应原文件：[coralnpu_StorageStacked/user/smoke/src/smoke.cpp](https://github.com/hy2581/coralnpu_StorageStacked/blob/02d3644d126d96d0da52f368ff75ec61d62b4f57/user/smoke/src/smoke.cpp)。

这个小程序把 41 写进存储，再读回来加一，最后写出并读回 42。

## 输入与输出

输入来自自动生成的 `project_config.h` 中的 `SMOKE_INPUT`，当前 JSON 选择 41。
NPU 写入和读回外部内存，期望输出 42；最终用 mailbox 告知仿真器程序完成。

## 三个地址

| 变量 | 地址 | 用途 |
| --- | --- | --- |
| `input` | `0x90000000` | 输入 uint32 字 |
| `output` | `0x90010000` | 输出 uint32 字 |
| `mailbox` | `0xc0000000` | 设备完成状态寄存器 |

## main 按顺序做什么

1. 将三个固定地址转换为 volatile uint32 指针。
2. 将 SMOKE_INPUT 写入 input。
3. 从 input 读回 value，计算 value + 1，再写入 output。
4. 从 output 读回 observed，与 SMOKE_INPUT + 1 比较。
5. 成功写 mailbox=`0x600d0000`，失败写 `0xbad00001`，执行 `wfi` 结束设备任务。

输入输出区共有四次源码级读写：写输入、读输入、写输出、读输出。
`volatile` 要求编译器保留这些访问；原生适配层以 16 字节请求和字节掩码传输，AXI 数据总线宽度为 32 字节。
源码的 4 字节元素访问、原生请求长度和 AXI 总线宽度是不同概念。mailbox 走设备状态路径。

## 怎样核对

正常输入 41 的期望为 42；输入 `0xffffffff` 时按 uint32 运算回绕为 0。
查看结果目录的 `smoke_summary.json`、`npu_requests.csv`、`transactions.csv` 及完整 `summary.json`。
mailbox 表示程序自检结果，链路与返回字节仍由后续校验器检查。

```mermaid
flowchart LR
  A["SMOKE_INPUT"] --> B["NPU 写 input"] --> C["NPU 读 input"] --> D["加一并写 output"] --> E["读 output 比较"] --> F["mailbox + wfi"]
```

## 从 41 到 42，程序每一步做什么

```cpp
*input = SMOKE_INPUT;
const uint32_t value = *input;
*output = value + 1u;
const uint32_t observed = *output;
```

| 执行到哪一行 | 发生什么 | 此时应有的值 |
|---|---|---|
| 第 1 行 | 向输入地址写入配置值 | 输入位置保存 41 |
| 第 2 行 | 从输入地址读回 | value 为 41 |
| 第 3 行 | 计算并写入输出地址 | 输出位置保存 42 |
| 第 4 行 | 从输出地址读回 | observed 为 42 |

`input` 保存的是“去哪里找数”，`*input` 表示“那个位置里的数”。
`reinterpret_cast` 把一个地址值解释成指定类型的指针，不会在这行自动完成读写。
`const` 表示该局部变量初始化后不再修改；`uint32_t` 表示 4 字节无符号整数。

最后的条件表达式 `条件 ? 成功值 : 失败值` 根据比较结果选一个状态码。
`wfi` 让设备进入等待，运行器结合 mailbox 识别任务结束。
它不是让你在终端上再输入数据。

## 数字 41 在内存里是什么字节

小端存储下，41 对应字节 `29 00 00 00`，42 对应 `2a 00 00 00`。
NPU 原生接口一次返回 16 字节块，可能包含这个 4 字节整数和旁边字节。
桥又把这 16 字节放在 32 字节 AXI 总线的相应位置。
判断数值时先定位地址与有效字节，不能把整条总线当成单个 uint32。

**为什么既写又读？** 写响应只能说明写操作完成；读回可以检查后续实际拿到的数据。
**能否直接把 expected 写到输出？** 自定义负载应实际完成约定计算，期望值用于独立核对；
只把答案写入会失去验证该运算和输入读取的意义。
