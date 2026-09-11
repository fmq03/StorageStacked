#pragma once

// 标准/供应商 timing profile 数据库入口。
// 配置流程是：
// 1. make_spec() 提供协议基准；
// 2. 配置库接收主输入，解算几何密度、速率和最终 CK；
// 3. apply_standard_timing_profile() 展开完整 organization/timing；
// 4. 用户逐项 override 覆盖 profile；
// 5. finalize_spec() 校验并生成 constraints/table。

#include "hbm_sim/dram/spec.hpp"

namespace hbm_sim {

// 根据 DramSpec 中的 timing_profile/vendor_profile/speed_bin_mbps/density_gb/
// stack_height/mode_profile/lpddr_dvfs_mode 等字段，更新 timing、组织参数和来源元数据。
// profile 只给有来源标记的默认表，不冒充具体 vendor；vendor_profile/mode_profile
// 是审计名称，不改变来源和功能开关。只有显式表来源可以标为 Vendor。本函数不
// 重建派生表；调用方完成显式覆盖后必须调用 finalize_spec()。
void apply_standard_timing_profile(DramSpec& spec, double resolved_tck_ps = 0.0);

}  // namespace hbm_sim
