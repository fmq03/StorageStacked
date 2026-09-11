# 验证元数据

本目录保存四个标准的可执行验证参数集，以及不参与模型解析的来源清单：

- `hbm3.cfg`：Ramulator2 HBM3 共同面和 DRAMsim3 较老 HBM 辅助面。
- `hbm4.cfg`：项目内 HBM4 单通道路径和 Ramulator2 HBM4 共同面。
- `lpddr5.cfg`：Ramulator2 LPDDR5 共同面。
- `lpddr6.cfg`：Ramulator2 LPDDR6 共同面。
- `project_identity.csv`：项目原创能力及其验证证据绑定。
- `vendor_parameters.csv`：器件级结论仍缺少的厂商参数。

四份 cfg 都用 `[meta] extends` 继承标准主配置，只保存比较时需要改变的组织、Timing、
维护、PHY 和 workload 条件。配置本身不会运行外部仿真器；`tools/config_selection.py`
负责选择它们，`ramulator2_differential.py` 和 `dramsim3_aux_validation.py` 才会分别
执行外部模型并保存外部版本、入口、原始结果和比较判据。

验证夹具也采用 schema 3；不再借旧版本保留与几何矛盾的显式密度或时钟。
单通道 HBM 夹具的 density 是其实际几何按层数折算的值，不是完整 Ramulator 器件的标称密度。
LPDDR5/6 夹具统一使用事务槽：参考端原始 `column=1024` 除以地址预取因子 16，
得到 `columns=64`；每槽 32 B，每行 2 KiB。当前单 Channel、单 Subchannel 共同面
容量为 2 GiB，density 为 16 Gibit。LPDDR6 的地址预取因子 16 不是 BL24，不能用 24 去除。
旧夹具的 1024 事务槽导致容量扩大 16 倍，已修正，不再用免责声明替代单位换算。
实际数值与外部型号分别记录在
[profile_index.csv](../profile_index.csv) 的数值列和 `reference_model` 列。
关闭刷新/RFM 的共同命令比较不会使用它来生成周期性维护命令。
HBM3 的参考 preset 显式保存 `nRFC=560 nCK`（参考器件 350 ns / 625 ps），
用于时序表和显式命令的外部对照；不再依赖旧的独立 density 输入选中这行时序。
`dramsim3_hbm2_common` 显式保留 1000 ps CK，并以本模型 HBM 的时间公式配置
4000 Mb/s/pin；这是 HBM3 执行语义下的旧协议公共面抽象，不声明真实 HBM2 接口速率。
该夹具按 DRAMsim3 的 HBM 原始列数×2/BL 换算为 32 个 64 B 事务槽，
每行 2 KiB、单 Channel 512 MiB；`stack_height=4` 对齐参考 `num_dies=4`。

差分脚本检查参考组织换算、项目实际执行组织和容量，并在 Ramulator 命令场景中覆盖行尾、
下一行、最后一个 Bank 与最后一个事务。参考端仍使用 pass-through 坐标映射，
这些测试不声称证明双方任意字节地址 mapper 等价；本项目首尾地址数据闭环及越界拒绝另由
`tests/result_tests.py` 覆盖。配置不自行启动外部引擎，也不代表完整器件级校准。

LPDDR6 夹具还将来源拆为 timing_reference、timing_derived、timing_jedec 和
timing_project：只有实际外部字段标 external_reference；nAADMin/共同面公式标 derived，
REFdb S/L 标 JEDEC，未纳入比较的旧项目扩展保留 research_default。
分段只修正来源声明、不改变这些显式数值；不要把关闭自动刷新/RFM 的差分夹具直接用作器件配置。
