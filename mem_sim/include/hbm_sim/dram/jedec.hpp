#pragma once

// JEDEC timing 单位换算工具。所有 ns/us 到 nCK 的转换都集中在这里，
// 保证不同 preset 和配置路径采用一致的向上取整规则。

namespace hbm_sim::jedec {

// JEDEC timing table 常同时给出 ns/us 和 nCK 形式。控制器内部统一保存 nCK，
// 因此所有标准表格值都先经过这里做向上取整，避免手写换算时出现 off-by-one。
int ps_to_nck(double time_ps, double tCK_ps);
int ns_to_nck(double time_ns, double tCK_ps);
int us_to_nck(double time_us, double tCK_ps);

// 表格里常见的 “Max(X ns, Y nCK)” 写法，例如 LPDDR6 的 tRCD/tRRD/tWTR。
int max_ns_or_nck(double time_ns, int cycles, double tCK_ps);

// HBM3/HBM4 tCCDL 在 JESD238/JESD270 中写成 Max(4 nCK, 2.5 ns/tCK)。
int hbm_tccdl_nck(double tCK_ps);

// HBM per-bank refresh interval：tREFIpb = 3.9 us / bank_count_in_refresh_rotation。
// HBM4 8-high 32-bank 设备传 32，12-high 传 48，16-high 传 64。
int hbm_trefipb_nck(double tCK_ps, int bank_count_in_refresh_rotation);

}  // namespace hbm_sim::jedec
