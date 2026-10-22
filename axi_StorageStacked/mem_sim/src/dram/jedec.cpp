// DRAM 标准单位换算：把 JEDEC 表格常见的 ns/us/Max(ns,nCK) 转成模型内部 nCK。
// 这里集中处理向上取整，避免各 preset 或配置解析路径各自实现一套换算逻辑。
#include "hbm_sim/dram/jedec.hpp"

#include <algorithm>
#include <cmath>

namespace hbm_sim::jedec {

int ps_to_nck(double time_ps, double tCK_ps) {
  if (time_ps <= 0.0 || tCK_ps <= 0.0) {
    return 0;
  }
  // JEDEC timing 表通常要求向上取整到完整 nCK；向下取整会让模型过早发命令。
  return static_cast<int>(std::ceil(time_ps / tCK_ps));
}

int ns_to_nck(double time_ns, double tCK_ps) {
  // ns/us 转换都先进入 ps_to_nck，保证取整策略只有一处。
  return ps_to_nck(time_ns * 1000.0, tCK_ps);
}

int us_to_nck(double time_us, double tCK_ps) {
  return ps_to_nck(time_us * 1000000.0, tCK_ps);
}

int max_ns_or_nck(double time_ns, int cycles, double tCK_ps) {
  // JEDEC 表常见写法是 Max(x ns, y nCK)。模型内部只保存 nCK，因此这里取两者较大值。
  return std::max(cycles, ns_to_nck(time_ns, tCK_ps));
}

int hbm_tccdl_nck(double tCK_ps) {
  // HBM tCCDL 采用 Max(2.5ns, 4nCK) 的常见公式，HBM3/HBM4 preset 都复用。
  return max_ns_or_nck(2.5, 4, tCK_ps);
}

int hbm_trefipb_nck(double tCK_ps, int bank_count_in_refresh_rotation) {
  if (bank_count_in_refresh_rotation <= 0) {
    return 0;
  }
  // 3.9us refresh interval 按参与 per-bank rotation 的 bank 数均分，得到每个
  // per-bank refresh slot 的间隔。
  return ps_to_nck(3900000.0 / static_cast<double>(bank_count_in_refresh_rotation), tCK_ps);
}

}  // namespace hbm_sim::jedec
