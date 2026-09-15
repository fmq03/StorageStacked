# 统一系统中的链路接入

AXI2Flit 和 UCIe 为普通源码目录，公共帧映射在 protocol/include。
整机入口是根目录 env/run.sh，宿主在 systemc/，使用单个 SystemC 内核。

```text
VORTEX AXI256 ⇄ Axi2Flit ⇄ UcieAouAdapter ⇄ UcieLink ⇄ AouTarget ⇄ Logic Die ⇄ mem_sim
```

Adapter 在信号握手和 FDI FIFO 之间转换。AouTarget 在 Flit 消息和
SimpleMemRequest/Response FIFO 之间转换。Logic Die 实现普通存储访问、
门控寄存器以及本地 DMA 仲裁。SimpleBurstMemory 仅供桥的独立测试。

```bash
make -C axi2flit/systemc SYSTEMC_HOME=/usr preflight
make -C axi2flit/systemc SYSTEMC_HOME=/usr full-link-all full-link-negative
```

项目整体接口见 [架构说明](../../../docs/architecture.md)；
桥的字节契约见 ../doc/wire_contract.md。
