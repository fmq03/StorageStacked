# StorageStacked

gem5 CPU / Vortex GPU / CoralNPU 与 AXI2Flit、UCIe、mem_sim 的集成工作区。
主仓库当前只提交必要的工作区说明、忽略规则和基础子模块版本。

| 子模块路径 | 内容 |
|---|---|
| gem5_new | 三源集成增量、配置、workload 和 HETTrace 工具 |
| axi2flit | AXI2Flit 协议转换与测试 |
| mem_sim | 存储控制器及 DRAM 模型 |
| gem5 | 固定公开基线的完整 gem5 源码 |
| coralnpu | 完整 CoralNPU 源码 |
| vortex-gpu/vortex | 完整 Vortex 源码及 8 个递归子模块 |

gem5_axi、ucie-model、integrate_doc、旧 zhongxing 交付包和原始仿真输出暂留本地，
不在本次主仓库提交范围内。已有 AXI/UCIe 联调源码改动未提交；axi2flit 本次只提交
必要的忽略规则。主仓库显示 axi2flit 为 modified 时，应检查并保留其中的本地补丁。

```bash
git status --short
git submodule status --recursive
git diff --submodule=log
```

子模块的实际版本由 gitlink 固定；上游锁文件与本地模块版本存在差异时，不自动 reset。
尚未发布主仓库及本地新提交，不能保证仅从原上游地址递归 clone 能得到全部固定版本。
Git 建仓后的环境工作见 [统一环境说明](env/README.md)。默认入口已切换到
gem5_new 锁定的新 gem5 基线及统一工具链，旧环境保留用于历史对照。

```bash
bash env/bootstrap.sh
bash env/build.sh
bash env/run_memsim.sh
```

CPU 控制 Vortex GPU、CoralNPU 的三源链路也已接通，设备共享缓冲区经
AXI2Flit → UCIe → 在线 mem_sim 访问，真实响应沿原路径返回。完整入口：

```bash
bash env/bootstrap_xpu.sh
bash env/build_xpu.sh
bash env/run_xpu.sh
```

三源接入、地址布局和验收范围见 [交接说明](integrate_doc/15_xpu_online.md)。
CPU 验收见 results/axi256-20260911/memsim/summary.json。
当前前端已改为原生 AXI256，宽度变更和新旧报告入口见
[AXI256 验证说明](integrate_doc/16_native_axi256.md)。旧结果已按用户后续要求清理，
当前HTML使用按需加载，交接时复制整个结果用例目录，见[可视化说明](integrate_doc/17_visualization.md)。

得到提交授权后，先提交子仓库的必要变更，再在主仓库更新 gitlink。
**本次及后续由助手创建的提交说明统一使用中文；未经授权不 push。**
本地复制需保留根 .git（含 .git/modules）及需要交接的未提交/被忽略文件。
