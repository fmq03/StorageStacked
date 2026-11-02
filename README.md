# StorageStacked

StorageStacked 提供可独立运行的 AXI256 → AXI2Flit → UCIe → 在线内存模型链路，并接入 Vortex/gem5 与 CoralNPU 原生 RTL 模型。

```text
独立 AXI 驱动 ───────────────────────────┐
gem5/Vortex → TLM/AXI 适配器 ────────────┼→ AXI2Flit → UCIe → MemSimBackend → mem_sim
CoralNPU RTL → 原生请求/AXI 适配器 ──────┘                        响应沿原路返回
```

## 模块

| 模块 | 职责 | 文档 | 测试入口 |
|---|---|---|---|
| mem_sim | 在线内存与 DRAM/DFI 模型 | [说明](components/mem_sim/README.md) | CTest / `make test` |
| AXI2Flit | AXI 与 AoU/Flit 转换 | [说明](components/axi2flit/README.md) | `make preflight` |
| UCIe | 双向链路、重试与 PHY 行为模型 | [说明](components/ucie-model/README.md) | `make validate` |
| AXI 链路集成 | 独立驱动、接线和跨层验证 | [说明](axi_StorageStacked/README.md) | `./axi_StorageStacked/run.sh test` |
| Vortex/gem5 | 主机、GPU 和公共存储联合运行 | [说明](vortex_StorageStacked/README.md) | `./vortex_StorageStacked/build.sh --test` |
| CoralNPU | NPU RTL 与公共存储联合运行 | [说明](coralnpu_StorageStacked/README.md) | `./coralnpu_StorageStacked/build.sh --test` |

负责人以各模块 README 和维护者确认为准。

## 原生开发

```bash
./axi_StorageStacked/run.sh setup
./axi_StorageStacked/run.sh build
./axi_StorageStacked/run.sh test
./vortex_StorageStacked/build.sh --storage ../axi_StorageStacked --test
./coralnpu_StorageStacked/build.sh --storage ../axi_StorageStacked --test
```

独立运行使用各入口自己的配置和 SystemC 环境。Vortex 使用 gem5 内置 SystemC；CoralNPU 和独立链路可选择兼容的 Accellera 环境，同一进程只使用同一套内核。

```bash
./axi_StorageStacked/run.sh run --input axi_StorageStacked/examples/roundtrip.json
./vortex_StorageStacked/user/run.sh smoke
./vortex_StorageStacked/user/run.sh llm
./coralnpu_StorageStacked/user/run.sh smoke
./coralnpu_StorageStacked/user/run.sh llm
./accept.sh
```

以新结果目录的 `summary.json` 和实际数据、协议、计算检查判断是否通过。

## 文档与交付

- [文档导航](docs/README.md)、[架构](docs/architecture.md)、[接口](docs/interfaces.md)、[开发约定](docs/development.md)。
- Docker 用于交付，使用方式见 [交付说明](delivery/README.md)。克隆后的交付包装入口位于 `delivery/clone/`，下载与结果仍位于仓库根的 `downloads/` 和 `results/`。
- 各组件历史导入保留在 `archive/*/before-monorepo` 分支；历史交付记录位于 `delivery/records/`。
