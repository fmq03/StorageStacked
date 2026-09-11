# 流量前端实现

frontend 层当前负责 workload 语义，不负责 DRAM timing。它支持 synthetic stream/random、普通 trace、timed trace、`BW/BR` burst、`data=`、`expect=`、`mask=` 和 `check=last_write`，并把请求预解码后交给 controller/system。

本目录实现请求前端。frontend 把 synthetic pattern、trace 文件和初始化/训练序列转换成
controller 可接收的 `Request`。

主要文件：

- `traffic.cpp`：stream/random workload、trace reader、control sequence generator。

修改建议：

- 新增 trace 格式时，要保证注释、空行和错误行处理清晰。
- 新增初始化或训练流程时，应生成维护请求，让 controller 和 validator 走同一条路径。
- frontend 不负责判断命令 timing 合法性。
- trace 中的 `data=HEX`、`expect=HEX`、`mask=HEX` 会被解析进 `Request`，由 controller 完成路径和 `MemoryImage` 执行真实读写与校验。

## workload 生成路径

frontend 主要生成三类请求：

- synthetic stream/random：用于吞吐、调度、地址映射研究。
- trace 请求：用于固定访存序列和外部 benchmark/golden 对比。
- control sequence：用于初始化、mode register、WCK training、DVFS、低功耗、RAS/ECC 入口。

生成后的请求会带上注入周期、请求类型、地址和 decoded address。controller 收到请求后再决定
是否需要 ACT、CAS、RD、WR、PRE、REF、RFM 等具体命令。

## trace 解析约定

- `R addr`：读请求。
- `W addr`：写请求。
- `cycle R addr`：在指定 cycle 注入读请求。
- `cycle W addr`：在指定 cycle 注入写请求。
- `W addr data=HEX`：写请求携带真实 payload。
- `R addr expect=HEX`：读完成时比较 actual payload 和 expected payload。
- `W addr data=HEX mask=HEX`：masked write，mask 中非零 byte 才更新对应 payload byte。
- `data=`/`payload=` 只允许用于写，`expect=`/`check=` 只允许用于读，`mask=`
  只允许用于写；普通写的 mask 字节数必须和 payload 字节数完全相同。跨多个
  DRAM transaction 的写会先把 mask 规范化，再逐 transaction 切片，避免后续
  transaction 因空 mask 被误当成全字节写。
- cycle、address、stack、qos 和 burst length 都采用严格非负整数解析；负数、
  尾随字符以及越过模型容量的地址范围会报错，不会取模回绕到另一行。
- `#` 注释和空行会被跳过。

新增 trace 语法时要同步更新 examples、tests 和 README，避免用户不知道格式变化。

数据正确性 trace 的推荐用法：

```text
0   W 0x1000 data=0011223344556677
100 R 0x1000 expect=0011223344556677
200 W 0x1000 data=deadbeefdeadbeef
300 R 0x1000 expect=deadbeefdeadbeef
```

显式 cycle 可以避免连续同地址写被写缓冲合并，适合做“每条写都提交一次”的人工检查。

## control sequence 原则

控制序列不要绕过 controller。即使是 MRW、WCK_TRAIN、DVFS、PDE/PDX、SREFEN/SREFEX，
也应生成维护请求并进入统一调度路径。这样在线仿真、命令 trace 和离线 validator 才能看到同一套行为。
