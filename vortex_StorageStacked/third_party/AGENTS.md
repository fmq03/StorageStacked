# 内部维护

用户入口为根目录 build.sh 和 user/run.sh；配置在 user/<项目>/config.json。
用户项目显式提供 host.cpp 与 kernel.cpp，Makefile 分别声明 HOST_SOURCES 与 KERNEL_SOURCES；SDK 不注入隐藏的主机 main。
CPU 多核验收同时检查线程任务分片、每核实际指令、L1 访问和 MMU 样本，不能用配置的核数代替实际执行核数。
third_party/gem5/runtime/ 提供平台环境、构建、运行与验证；third_party/vortex/sdk/ 提供用户应用 SDK；两者的 patches/ 各自保留集成补丁。integration/ 负责 gem5/TLM 到公共 AXI 存储的接入。
只用 gem5 自带的一套 SystemC，1 fs 时间基准，保留真实异步响应、字节掩码、反压与在线内存反馈。
不得以退出码或文件存在代替计算、AXI/VCD、Flit、MEMSIM 验收。
禁止 submodule，不生成摘要算法清单。配置与报告使用相对路径，内部缓存由 build.sh 重建。
根目录 ./build.sh --test 执行回归。只有得到用户明确授权才上传。
