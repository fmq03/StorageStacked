# StorageStacked

- 先读 README.md、docs/architecture.md 和 docs/verification.md。
- 主入口为 env/bootstrap.sh、env/build.sh、env/run.sh 和 make acceptance。
- 系统为 VORTEX SimX → AXI256 → AXI2Flit → UCIe → Logic Die → 在线 mem_sim。
- 一个 SystemC 调度线程，统一 1 fs 时间分辨率；VORTEX 与 RTL 按实际时钟推进。
- Logic Die 门控电路以 logic_die/rtl 为唯一实现；系统仿真使用该 RTL 的 Verilator 模型。
- 外部版本见 env/sources.lock.json。外部必要改动存成 vortex/patches 中的补丁。
- 新测试输出到 results/；保留 AXI 五通道 VCD、两端原始 Flit 日志和独立数据检查。
- 论文结果必须可追溯到实测数据。论文引用的宏指标不能冒充本项目综合或硅后结果。
- 不提交工艺库、凭证、构建目录或完整运行日志。论文图表及其精简原始数据可提交。
- 提交说明使用中文；推送须有用户明确授权。
