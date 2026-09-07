# UCIe 接入组件

本目录交付 UCIe 侧适配器和参考链路扩展补丁。主仓库忽略 `reference/`，因此
补丁必须与 AXI2Flit 一起保存；只修改本机 reference 不足以交付。

## 1. 文件与基线

- `ucie_aou_endpoint.h`：双向 ready/valid ↔ FIFO、250B 编解码、训练门控及配置检查。
- `ucie-model-aou.patch`：基于 UCIe commit `58a44f3`，新增 AouFormat6、构造/检查/
  收集路径、CLI、SystemC include/library 路径和参考文档更新。
- `../include/aou_format6.h`：桥与参考模型共用的图4字节映射。
- `../include/simple_mem_if.h`：下一阶段整 burst 简单存储接口。

## 2. 在另一份工作区恢复

将原 UCIe 模型放在 `reference/ucie-model`，确认基线及本地修改后，在项目根目录执行：

```bash
git -C reference/ucie-model status --short
git -C reference/ucie-model apply --check ../../systemc/integration/ucie-model-aou.patch
git -C reference/ucie-model apply ../../systemc/integration/ucie-model-aou.patch
make reference-check
make preflight
```

`--check` 失败时应检查是否已经应用或基线不同，不要覆盖第三方仓库已有修改。
当前工作区已安装补丁，不要重复 apply。`reference-check` 用反向 dry-run 检查
补丁存在，属于只读门禁，不会撤销补丁。

SystemC 默认使用 `$HOME/.local/systemc-2.3.4-cxx17`，可通过
`make SYSTEMC_HOME=/实际安装目录 preflight` 覆盖。两侧统一 C++17。
独立移动 UCIe 目录时需设置 `AOU_INCLUDE` 指向包含 aou_format6.h 的目录。

## 3. 下一阶段的使用方式

`Config cfg = make_aou_ucie_config();` 生成与桥匹配的 x16/24G/NRZ 配置。
使用 `sc_fifo<FdiFlit>` 连接 `UcieAouEndpoint` 与 UcieLink 的 soc 两个数据端口；
`link_state` 使用 `sc_signal<unsigned>`。准确接线及边界时序见
[wire_contract.md](../doc/wire_contract.md)。

一个 FIFO slot 固定 250B；TX 在握手沿写 FIFO，RX 有一拍 holding。
FH/CRC 保留参考模型的行为算法。当前门禁不实例化 PHY，不验证 AoU 链路重放，
也不包含真实存储目标。下一阶段接上 UcieLink 与 Memory 模型后另做联合验收。
