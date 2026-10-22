# 统一系统中的链路接入

AXI2Flit、UCIe 和 mem_sim 作为本公共存储工程的普通目录维护。处理器前端位于各自的独立驱动仓库。
UCIe的AoU支持和观察接口已直接纳入ucie-model源码，不需要额外安装补丁。

| 文件 | 职责 |
|---|---|
| ucie_aou_adapter.h | Flit握手与UCIe FDI FIFO的双向适配 |
| aou_target.h | 存储请求解包、后端提交及响应打包 |
| simple_burst_memory.h | 独立链路测试使用的内存 |
| [aou_format6.h](../../../protocol/include/aou_format6.h) | 共享帧映射，实际位于系统根protocol/include |
| ../include/simple_mem_if.h | 存储请求与响应类型 |

完整在线入口是系统根目录 `./run.sh run`，验收入口是 `./run.sh test`。
文件输入或外部处理器的 AXI master 经本链路和 MemSimBackend 访问在线 mem_sim。
调用方只使用一套 SystemC，时间分辨率为 1 fs；对外接口见 [INTERFACE.md](../../../INTERFACE.md)。

AXI2Flit自己的独立SystemC测试入口仍保留，需另备独立SystemC测试环境：

```bash
make -C axi2flit/systemc reference-check
make -C axi2flit/systemc preflight
make -C axi2flit/systemc full-link-all
make -C axi2flit/systemc ucie-unit
```

默认UCIE_DIR=../../ucie-model（相对systemc目录），公共协议头来自../../protocol/include。
reference-check只检查依赖文件存在；模型接口由编译和回归验证，不再依赖git apply。

```text
Axi2Flit ⇄ UcieAouAdapter ⇄ 双向UcieLink ⇄ AouTarget ⇄ Memory
```

Adapter连接sc_signal握手与四个sc_fifo<FdiFlit>端口，Target与内存通过
SimpleMemRequest/Response FIFO连接。训练完成后释放复位；存储响应走原链路返回。
详细接口见../doc/wire_contract.md及../doc/design.md。
交接建议复制整个系统及所需结果目录，不再单独克隆或给内部模块打补丁。
