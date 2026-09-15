# 验证方法与结果边界

## 可执行验收

`make acceptance` 先构建，再执行以下检查；任意失败都必须返回非零状态。
重复执行时，只有源码、依赖、二进制和输入均匹配的在线用例可以恢复并重新离线核验。
修改运行实现后，使用 `make acceptance ACCEPTANCE_DIR=results/acceptance-new` 保存新证据。

1. RTL：NumPy 参考独立计算 BF16 RNE 池化、FP32 顺序点积、稳定 Top-K。
   测试随机值、相同分数、负分数、早期因果位置、K 的边界、权重复用、错误 epoch、
   非法寄存器值、DMA 随机背压/延迟、传输错误恢复、非有限 Q、打分溢出、
   子正规数、RNE 舍入边界与 4096-token 块，共 24 个用例及 72 次正常查询。
2. 存储：原生 8 个 C++ 测试，以及 C ABI 的掩码读写、队列压力和非法请求检查。
3. 链路：原生 AXI 多位宽功能、固定字节向量、边界、credit、UCIe、全链路重放
   与负向记分板回归。
4. 在线系统：真实 VORTEX 指令执行与同一份 Verilated Logic Die RTL。
   通过独立参考核对返回掩码/数量。Software 模式在 VORTEX 上执行同一数值算法。
5. 离线：两端 FDI 字节逐个匹配；独立 CRC-16 和物理帧散布检查；
   AXI CSV 的读写记分板；本地 DMA 与主机读数据；原生完成时刻下界；
   AXI 五通道的 VCD 握手数与 CSV 一致。
6. 负向控制：损坏独立期望掩码和接收 Flit 字节，必须被检测并失败退出。

结果文件保存于各用例 results 子目录。summary.json 表示在线工作负载通过；
offline_verification.json 表示离线证据检查通过；memsim_core.json 表示 DRAM/DFI 检查通过。
缺少其中任一个都不能把整机用例判为 PASS。论文数据以最后核验的精简副本为准。
顶层 `results/acceptance/summary.json` 汇总各层结果和证据哈希；每次验收开始先删除
旧汇总，失败时不写入新的 PASS。原生内存日志与链路日志同时保存在该目录。

## 可信范围

- 这验证了 Logic Die 门控算子和真实在线访存，不代表完整注意力、完整 LLM 或模型精度。
- 软件对照为固定 VORTEX 配置、一个活跃 lane 的标量正确性基线；其时间不能泛化成
  商用 GPU 加速比。链路流量包含指令、描述符、完成、结果及实际 cache miss。
- 初始化上传单独标为 loader，双方使用相同输入；论文时间和流量从内核启动后统计。
- DC 是标准单元逻辑综合；寄存器实现权重存储。功耗缺少活动标注，
  布局布线、物理 SRAM-CIM 宏表征、封装热和硅后功耗不在已验证范围。
- mem_sim 的部分时序为 provisional；UCIe 是行为链路模型，不能据此声称协议/PHY signoff。
