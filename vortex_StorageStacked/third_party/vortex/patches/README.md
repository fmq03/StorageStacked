# Vortex 来源与集成扩展

上游版本记录见 [.source-version](../.source-version) 与 [sources.json](../../gem5/runtime/defaults/sources.json)，许可证见 [LICENSE](../LICENSE)。

`simx_online.patch` 保留 SimX 在线存储 timing 接口的集成改动。构建直接使用已集成的源码，外部请求等待公共存储返回实际数据和完成时刻。

`../sdk/` 提供用户项目 Makefile、CPU 回读辅助函数与设备数学函数；平台环境和构建由 `../../gem5/runtime/` 管理。gem5 的来源说明见 [gem5 补丁记录](../../gem5/patches/README.md)。
