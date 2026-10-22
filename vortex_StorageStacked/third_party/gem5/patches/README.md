# gem5 来源与集成扩展

上游版本记录见 [sources.json](../runtime/defaults/sources.json) 与 [.source-version](../.source-version)，许可证见 [LICENSE](../LICENSE)。
本目录保留 gem5 的 DMA 字节掩码、掩码写入和 VCD 时间处理补丁。设备 timing 回调、跟踪接口和平台构建支持随当前源码交付；补丁文件用于保留改动依据，构建直接使用已集成的源码。

`../runtime/` 提供本平台的环境、构建、运行与校验脚本；用户入口为仓库根目录 `build.sh` 和 `user/run.sh`。Vortex 的来源说明见 [Vortex 补丁记录](../../vortex/patches/README.md)。

运行侧对同一外部存储块的读写冲突等待真实完成，保留异步链路中的访问顺序；验收按事务时刻独立检查。

主机 ISA 通过用户配置 `host.isa` 选择 x86-64 或 AArch64。运行支持分别选择 gem5 构建、主机 ELF、stub/backend 运行库与 ARM 加载器；`src/arch/arm/mmu.cc` 和 `translation_trace.hh` 为 SE 模式添加有界的真实地址转换采样，与 x86 的验收证据保持同一格式，不修改翻译结果。
