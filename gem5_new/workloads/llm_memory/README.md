# Decoder-LLM 访存 benchmark

这是可复现的 LLM-like **访存行为** benchmark，不执行 Transformer 数值计算。生成器产生
host、CoralNPU 和 Vortex 的 HETTrace v2：host 初始化权重/KV 与提交结果，NPU 流式读取
Q/K/V、attention output 和 SwiGLU 权重，Vortex 扫描并追加 FP16 KV cache。

默认微模型为 `hidden=128, intermediate=512, layers=4, context=128, decode=8`，记录粒度 64 B。
每层权重字节数为

```text
(4 × hidden² + 3 × hidden × intermediate) × weight_bytes
```

每层每 token 新增 KV 字节数为 `2 × hidden × kv_element_bytes`。

## 运行

先构建固定版本的外部 `mem_sim/hbm_sim`，并加载 `scripts/native_env.sh`。快速端到端回归：

```bash
make test-memsim-smoke
```

它固定使用 16 B 请求粒度；当前基线生成 596 个请求，并同时检查请求数、字节数、response ID
和外部模型的 data mismatch。当前实测数字见
[验证报告](../../docs/05-validation-report.md)。较大的默认实验为：

```bash
make benchmark-llm-memory
```

默认规模在当前参考服务器上可能运行几十分钟，不属于快速 CI。两种入口都会生成/校验 trace，
以 `convert --preset memsim` 生成请求和 mapping CSV，运行真实外部
`hbm_sim`，最后把 response ID 与 manifest/mapping 一一对齐，并要求 manifest 与 mapping
逐源请求数和字节数守恒。默认输出在
`build/llm_memory/`：

- `traces/`：三源 HETTrace 和 benchmark manifest；
- `mem_sim.trace`、`mem_sim.map.csv`：降级请求及来源映射；
- `hbm_sim.responses.csv`：外部模型响应；
- `validate.txt`、`stats.txt`、`hbm_sim.txt`、`summary.md`：检查与汇总。

`mem_sim.trace` 中的零值 `data=`/`expect=` 仅用于让外部解析器识别精确请求大小，写请求的
`mask=` 来自 HETTrace WSTRB；HETTrace 不保存真实 WDATA/RDATA，因此这些字段不是功能回放。
外部模型会真实写入/比较零值替身，所以 response status 和 data mismatch 也不能证明原始数据
正确，只用于这个合成 benchmark 的替身一致性检查。

修改规模：

```bash
workloads/llm_memory/run.sh \
  --hidden-size 256 --layers 6 \
  --context-tokens 256 --decode-tokens 16
```

可用环境变量包括 `LLM_BENCH_OUT`、`MEMSIM_CONFIG`、`MEMSIM_STANDARD`、
`MEMSIM_TICKS_PER_CYCLE`、`MEMSIM_MAX_CYCLES` 和 `MEMSIM_STATS_VIEW`。

## 结论边界

结果能说明固定请求流的读写量、footprint、局部性、来源交接，以及指定 controller/DRAM 配置
下的注入等待和完成延迟。它不能说明模型精度、真实 tokens/s、IPC、端到端推理时间或反馈后
会改变的请求序列；外部 completion 不回灌生成器。`arrival_cycle` 可以晚于
`requested_cycle`，比较脚本会把差值作为 injection wait 报告。
