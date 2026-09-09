# 链路与存储接入

## 1. 组件

| 文件 | 职责 |
|---|---|
| [ucie_aou_endpoint.h](ucie_aou_endpoint.h) | 桥帧握手与 FDI FIFO 的双向适配、250 字节编解码、训练门控 |
| [aou_target.h](aou_target.h) | 存储侧请求解包、写突发组装、后端提交与响应打包 |
| [simple_burst_memory.h](simple_burst_memory.h) | 串行突发存储、字节选通、窄访问及错误响应 |
| [ucie-model-aou.patch](ucie-model-aou.patch) | 链路格式、长度检查、物理帧收发、命令行和构建依赖适配 |
| [aou_format6.h](../include/aou_format6.h) | 共享的 250 字节逻辑内容与 256 字节物理帧映射 |
| [simple_mem_if.h](../include/simple_mem_if.h) | 存储请求与响应公共类型 |

`reference/` 不纳入主仓库，链路接入补丁须随项目交付。补丁只包含构建与代码适配，接口约定由本项目文档描述。

## 2. 准备依赖

SystemC 默认目录为 `$HOME/.local/systemc-2.3.4-cxx17`，桥与链路统一使用 C++17。链路默认目录为 `reference/ucie-model`。在项目根目录检查依赖内容：

```bash
git -C reference/ucie-model status --short
make reference-check
```

`reference-check` 使用反向补丁检查，只读确认所需适配内容存在，不会撤销补丁。如果依赖尚未包含这些适配，先检查可应用性，再安装：

```bash
git -C reference/ucie-model apply --check ../../systemc/integration/ucie-model-aou.patch
git -C reference/ucie-model apply ../../systemc/integration/ucie-model-aou.patch
make reference-check
make preflight
make full-link-all
```

已经安装时不要重复应用。检查失败需要核对依赖代码和补丁上下文，保留依赖项目已有修改；目录可配置不意味着任意链路 API 都兼容。

接入代码依赖 `UcieLink`、`Config`、`FdiFlit`、`Stats`、`LinkState`、`FlitFormat::AouFormat6` 及四个 FDI FIFO 端口。配置由 `make_aou_ucie_config()` 生成，并由 `require_aou_ucie_config()` 检查。

## 3. 接线

```text
Axi2Flit.flit_out / flit_ready ⇄ Endpoint.tx / tx_ready
Axi2Flit.flit_in / flit_in_ready ⇄ Endpoint.rx / rx_ready
Endpoint.fifo_tx → soc_tx → UcieLink.soc_tx_in
Endpoint.fifo_rx ← soc_rx ← UcieLink.soc_rx_out
Target.link_rx  ← mem_rx ← UcieLink.mem_rx_out
Target.link_tx  → mem_tx → UcieLink.mem_tx_in
Target.mem_req  → requests  → Memory.request
Target.mem_rsp  ← responses ← Memory.response
```

前两行使用 `sc_signal<FlitTransfer>` 和独立就绪信号，中间四行使用 `sc_fifo<FdiFlit>`，最后两行使用 `SimpleMemRequest/Response` FIFO。Endpoint 的 `link_state` 连接链路同名输出，类型为 `sc_signal<unsigned>`。

SoC 侧适配器处理信号与 FIFO 的时序转换；响应端直接使用 FIFO，因此存储侧不再需要同类适配器。启动时等待链路训练完成后释放桥和响应端复位；运行期间不支持单端热复位。字段、精确字节布局及握手规则见[接口文档](../doc/wire_contract.md)和[设计文档](../doc/design.md)。

## 4. 路径迁移与独立交付

更换链路模型位置时，在根目录执行：

```bash
make UCIE_DIR=/work/models/ucie-model full-link-all
make UCIE_DIR=/work/models/ucie-model ucie-unit
```

`UCIE_DIR` 默认 `../reference/ucie-model`，命令行或环境变量可覆盖；相对路径始终相对于 `systemc/`。构建入口按自身目录定位补丁，并向链路子构建自动传入共享头文件的绝对路径。无需修改桥的 C++ 包含路径或补丁中的默认路径。

在其他目录安装补丁可使用绝对路径：

```bash
git -C /work/models/ucie-model apply --check /work/axi2flit/systemc/integration/ucie-model-aou.patch
git -C /work/models/ucie-model apply /work/axi2flit/systemc/integration/ucie-model-aou.patch
```

直接在链路工程构建时，显式传入共享头文件位置：

```bash
make -C /work/models/ucie-model AOU_INCLUDE=/work/axi2flit/systemc/include \
  SYSTEMC_HOME=/work/systemc all unit-test
```

桥核心包含 `systemc/src/` 和 `systemc/include/`。交付可复现仿真环境还需根 Makefile、`systemc/Makefile`、`tb/`、`integration/`、`scripts/` 及文档。`sim/` 产物可以重新生成。桥独立功能和性能测试不需要 UCIe 依赖；联合测试需要接口匹配的链路源码和补丁内容。

## 5. 测试与产物

`endpoint` 测试适配器和字节处理组件，`ucie-unit` 运行链路独立单元测试，`full-link-all` 实例化链路物理行为、响应端与内存完成端到端检查。波形、事务日志及结果判读见[全链路使用说明](../../doc/UCIe全链路仿真计划与使用.md)，性能口径见[验证文档](../doc/verification.md)。
