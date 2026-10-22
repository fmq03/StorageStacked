#pragma once

#include "hbm_sim/config/document.hpp"

// hbm_sim 公共聚合头文件。
//
// 外部小实验、smoke test 或辅助工具可以直接 include 这个文件，避免关心
// core/dram/controller/frontend/stats/validation 的内部头文件分布。
// 项目内部源码仍建议 include 自己真正依赖的窄头文件，减少编译耦合。

#include "hbm_sim/controller/executor.hpp"
#include "hbm_sim/controller/controller.hpp"
#include "hbm_sim/controller/command.hpp"
#include "hbm_sim/controller/refresh.hpp"
#include "hbm_sim/controller/rfm.hpp"
#include "hbm_sim/controller/row_policy.hpp"
#include "hbm_sim/controller/scheduler.hpp"
#include "hbm_sim/controller/timing.hpp"

#include "hbm_sim/core/addr_map.hpp"
#include "hbm_sim/core/common.hpp"
#include "hbm_sim/core/data.hpp"
#include "hbm_sim/core/stack_model.hpp"
#include "hbm_sim/core/system.hpp"
#include "hbm_sim/core/request.hpp"

#include "hbm_sim/dram/bank_state.hpp"
#include "hbm_sim/dram/semantics.hpp"
#include "hbm_sim/dram/state.hpp"
#include "hbm_sim/dram/interface.hpp"
#include "hbm_sim/dram/jedec.hpp"
#include "hbm_sim/dram/spec.hpp"
#include "hbm_sim/dram/profiles.hpp"

#include "hbm_sim/frontend/traffic.hpp"

#include "hbm_sim/stats/stats.hpp"

#include "hbm_sim/validation/dfi.hpp"
#include "hbm_sim/validation/trace.hpp"
#include "hbm_sim/validation/validator.hpp"
