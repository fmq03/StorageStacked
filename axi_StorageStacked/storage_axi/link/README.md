# UCIe 与存储的链路接线

`ucie_aou_adapter.h` 连接 AXI2Flit 的 Flit 握手与 UCIe FDI FIFO；`aou_target.h` 解包存储请求、提交后端并打包响应。`simple_burst_memory.h` 只供独立全链路测试。

共享格式见 [aou_format6.h](../../../common/interfaces/aou_format6.h)，请求/响应类型见 [simple_mem_if.h](../../../components/axi2flit/systemc/include/simple_mem_if.h)。

从仓库根目录运行：

```bash
./axi_StorageStacked/run.sh run
./axi_StorageStacked/run.sh test
./axi_StorageStacked/run.sh link-test preflight
```

独立调试明确设置 `SYSTEMC_HOME`、`CXX` 和 `SYSTEMC_CXX_STANDARD`；公共源码路径由 [manifest](../../storage_manifest.json) 读取。组件自身回归保留在 components/ 下。

```text
Axi2Flit ⇄ UcieAouAdapter ⇄ UcieLink ⇄ AouTarget ⇄ MemSimBackend ⇄ mem_sim
```

Adapter 连接信号握手与四个 FDI FIFO；Target 与内存通过整 burst 请求/响应 FIFO 连接。链路训练完成后释放复位。每个进程只使用一套 SystemC，时间分辨率为 1 fs。公开端口见 [AXI 接口](../../INTERFACE.md)，桥编码见 [字节格式](../../../components/axi2flit/docs/systemc/wire_contract.md)。
