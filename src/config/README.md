# Config module

`document.cpp` 只实现配置文档的语法、standard/preset 选择、section 激活和固定
layer 排序，不直接修改 `DramSpec`。`fields.hpp` 保存普通字段和 Timing 别名的一对一映射；
`model.cpp` 负责
`build_model()` 和联动输入解析；CLI 的 `apply_option()` 只分发平台选项与模型输入。
外部前端可通过 `include/hbm_sim/config/model.hpp` 复用同一模型构建入口。

schema-v2/v3 分节 layer 为：meta/model 0、公共 section 10、family 20、standard 30、preset 40、
override 50；命令行由 CLI 记录为 60。inactive standard/preset section 不进入
解析结果。平面 `key=value` 文档也执行相同的参数联动。
Timing 来源按文档/section 绑定到字段，不能随扫描顺序跨段传播。

新增 section 或简写键时，应同步：

1. 更新 `is_common_section()`/`canonical_entry_key()`；
2. 普通模型字段在 `fields.hpp` 注册类型、别名、赋值和取值；有副作用的枚举或
   平台选项分别在模型解析器或 CLI 接线；
3. 更新两个家族主配置及 `configs/README.md`；
4. 为 active/inactive section、错误诊断和 resolved-config 重载增加测试。

配置不能创建新的算法实现。scheduler、row policy、PHY mode 和 backend 的陌生名称
必须失败；增加名称时还要实现对应 C++ 行为和回归测试。

所有版本共用 `resolve_coupled_inputs()`，在 profile 展开前核对几何、密度、速率和
时钟；`apply_spec_overrides()` 对失败覆盖保持调用方原对象不变。
HBM 的 `sids=auto` 按支持的 4/8/12/16Hi 组织选择 1/2/3/4，在容量和密度推导之前完成；
指定层数但省略 SID 时采用同一规则。其他层数需显式正整数 SID，不做截断猜测。
LPDDR auto 使用中性 SID=1。显式 SID 保留为研究几何，不能据此宣称标准器件一致。
库对现有模型的部分覆盖若没有层数/SID 输入，会保留现有 SID；显式 auto 可重新选择。
`model_config_tests` 检查小容量、部分 channel、分数密度、溢出、nRC 边界及旧版输入的同一契约。

普通字段的配置导出和结果记录使用同一 typed getter；Timing 字段使用同一
canonical name/别名表进行识别、nCK/ns/us 换算、重复别名检查和来源绑定。
来源按字段绑定，导出仍按来源分组；几何/密度/速率/时钟联动显式执行，
不把来源或联动规则隐含在普通字段 setter 中。`parse.hpp` 统一 CLI 和库的
整数、浮点、布尔解析，拒绝尾随字符、空白和非有限数值。
