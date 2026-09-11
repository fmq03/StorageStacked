#pragma once

// 内部统计数据结构。Stats 同时包含 system-level 和 controller-aggregate
// 字段，因此多 controller 实验需要注意每个指标的归一化口径。

#include <cstdint>
#include <iosfwd>

#include "hbm_sim/core/common.hpp"

namespace hbm_sim {

// 控制器运行期间累计的内部统计；ResultReport 单独定义公共指标集合。
struct Stats {
  // run_until_done() 结束时写入总 cycle。若 hit_cycle_limit=true，这里是上限值。
  Cycle cycles = 0;
  // enqueue() 阶段统计进入控制器的请求数。
  std::uint64_t reads = 0;
  std::uint64_t writes = 0;
  // complete_pending() 阶段统计真正完成的请求数。
  std::uint64_t completed_reads = 0;
  std::uint64_t completed_writes = 0;
  // row 状态按请求计数，而不是按命令计数；一个请求最多贡献一次。
  std::uint64_t row_hits = 0;
  std::uint64_t row_misses = 0;
  std::uint64_t row_conflicts = 0;
  // 以下命令计数由 CommandExecutor::issue() 更新，用于观察协议路径是否被触发。
  // 这些计数是“命令数”，不是请求数：一个请求可能贡献 ACT+RD 两条命令。
  std::uint64_t act = 0;
  std::uint64_t act1 = 0;
  std::uint64_t act2 = 0;
  std::uint64_t pre = 0;
  std::uint64_t prepb = 0;
  std::uint64_t preab = 0;
  std::uint64_t cas_rd = 0;
  std::uint64_t cas_wr = 0;
  std::uint64_t rd = 0;
  std::uint64_t wr = 0;
  std::uint64_t rda = 0;
  std::uint64_t wra = 0;
  std::uint64_t refab = 0;
  std::uint64_t refpb = 0;
  std::uint64_t refdb = 0;
  std::uint64_t rfmab = 0;
  std::uint64_t rfmpb = 0;
  std::uint64_t mrw = 0;
  std::uint64_t mrr = 0;
  std::uint64_t wck_sync = 0;
  std::uint64_t wck_train = 0;
  std::uint64_t dvfs = 0;
  std::uint64_t pde = 0;
  std::uint64_t pdx = 0;
  std::uint64_t srefen = 0;
  std::uint64_t srefex = 0;
  std::uint64_t ecc_scrub = 0;
  std::uint64_t ras_err = 0;
  // maintenance_requests 统计进入维护路径的内部请求，包括 refresh/RFM/row-policy PRE。
  std::uint64_t maintenance_requests = 0;
  // maintenance_served 只在终端维护命令 REF/RFM/PREpb 真正发出后增加。
  std::uint64_t maintenance_served = 0;
  std::uint64_t refresh_batches = 0;
  std::uint64_t refresh_per_bank_batches = 0;
  std::uint64_t refresh_all_bank_batches = 0;
  std::uint64_t refresh_postpones = 0;
  std::uint64_t refresh_pullins = 0;
  std::uint64_t refresh_credit_peak = 0;
  std::uint64_t rfm_events = 0;
  std::uint64_t rfm_per_bank_events = 0;
  std::uint64_t rfm_all_bank_events = 0;
  // HBM 双总线模式下，同一 cycle 成功发出 row+column 的次数。
  std::uint64_t dual_issue_cycles = 0;
  std::uint64_t row_bus_issues = 0;
  std::uint64_t column_bus_issues = 0;
  std::uint64_t unified_bus_issues = 0;
  std::uint64_t rising_edge_ticks = 0;   // HBM edge pairing 下 rising edge tick 数。
  std::uint64_t falling_edge_ticks = 0;  // HBM edge pairing 下 falling edge tick 数。
  std::uint64_t write_mode_cycles = 0;   // write-drain 模式持续的 controller cycle 数。
  std::uint64_t injected_requests = 0;   // frontend 成功进入 controller buffer 的请求数。
  std::uint64_t injection_stall_cycles = 0;  // frontend 因目标 buffer 满而停顿的 cycle。
  std::uint64_t read_forwards = 0;
  std::uint64_t write_coalesces = 0;
  std::uint64_t data_checked_reads = 0;
  std::uint64_t data_mismatches = 0;
  std::uint64_t data_uninitialized_reads = 0;
  std::uint64_t data_write_commits = 0;
  std::uint64_t data_masked_write_commits = 0;
  std::uint64_t data_forward_checks = 0;
  std::uint64_t storage_lines_allocated = 0;
  std::uint64_t unique_written_lines = 0;
  std::uint64_t storage_bytes_allocated = 0;
  std::uint64_t storage_topology_lines_scanned = 0;
  std::uint64_t storage_topology_scan_skipped = 0;
  // 从 spec 计算的可寻址行总数（ch * pc * sid * rank * bg * bank * rows * cols）
  std::uint64_t total_addressable_lines = 0;
  // unique_written_lines / total_addressable_lines，反映 workload 的存储覆盖率
  double storage_density_pct = 0.0;
  // Burst trace 统计：BW/BR 在 trace 中的原始行数，以及拆分后的请求数。
  std::uint64_t burst_trace_lines = 0;
  std::uint64_t burst_split_requests = 0;
  std::uint64_t burst_read_requests = 0;
  std::uint64_t burst_write_requests = 0;
  std::uint64_t burst_read_bytes = 0;
  std::uint64_t burst_write_bytes = 0;
  std::uint64_t storage_stacks_touched = 0;
  std::uint64_t storage_dies_touched = 0;
  std::uint64_t storage_layers_touched = 0;
  std::uint64_t storage_channels_touched = 0;
  std::uint64_t storage_pseudo_channels_touched = 0;
  std::uint64_t storage_sids_touched = 0;
  std::uint64_t storage_ranks_touched = 0;
  std::uint64_t storage_bank_groups_touched = 0;
  std::uint64_t storage_banks_touched = 0;
  std::uint64_t storage_rows_touched = 0;
  std::uint64_t storage_columns_touched = 0;
  std::uint64_t storage_subarrays_touched = 0;
  std::uint64_t storage_mats_touched = 0;
  std::uint64_t storage_cells_touched = 0;
  std::uint64_t storage_microbumps_touched = 0;
  std::uint64_t floorplan_tiles_touched = 0;
  std::uint64_t thermal_tiles_touched = 0;
  std::uint64_t thermal_grid_cells_touched = 0;
  std::uint64_t storage_read_line_accesses = 0;
  std::uint64_t storage_write_line_accesses = 0;
  std::uint64_t rowbuf_activations = 0;
  std::uint64_t rowbuf_precharges = 0;
  std::uint64_t rowbuf_dirty_writebacks = 0;
  std::uint64_t rowbuf_clean_precharges = 0;
  std::uint64_t rowbuf_hits = 0;
  std::uint64_t rowbuf_misses = 0;
  std::uint64_t rowbuf_lazy_loads = 0;
  std::uint64_t rowbuf_reads = 0;
  std::uint64_t rowbuf_writes = 0;
  std::uint64_t rowbuf_forced_closes = 0;
  std::uint64_t rowbuf_open_rows = 0;
  std::uint64_t rowbuf_dirty_rows = 0;
  std::uint64_t power_events = 0;
  std::uint64_t thermal_updates = 0;
  double power_energy_pj = 0.0;
  double power_act_energy_pj = 0.0;
  double power_pre_energy_pj = 0.0;
  double power_read_energy_pj = 0.0;
  double power_write_energy_pj = 0.0;
  double power_refresh_energy_pj = 0.0;
  double power_rfm_energy_pj = 0.0;
  double power_control_energy_pj = 0.0;
  double thermal_peak_temp_c = 40.0;
  // 查询时刻所有已实例化（即被热事件触达/耦合创建）的稀疏 thermal-grid
  // 节点空间平均值；不是全 die 未触达区域平均，也不是时间平均。
  double thermal_avg_temp_c = 40.0;
  int thermal_hotspot_layer = -1;
  int thermal_hotspot_x = -1;
  int thermal_hotspot_y = -1;
  std::uint64_t thermal_lateral_transfers = 0;
  std::uint64_t thermal_vertical_transfers = 0;
  std::uint64_t thermal_tsv_transfers = 0;
  double thermal_coupled_delta_c = 0.0;
  std::uint64_t ecc_shadow_updates = 0;
  std::uint64_t ecc_checked_reads = 0;
  std::uint64_t ecc_corrected_errors = 0;
  std::uint64_t ecc_uncorrectable_errors = 0;
  std::uint64_t ecc_injected_errors = 0;
  std::uint64_t ecc_parity_repairs = 0;
  std::uint64_t dfi_read_beats = 0;
  std::uint64_t dfi_write_beats = 0;
  std::uint64_t dfi_forwarded_read_beats = 0;
  std::uint64_t dfi_masked_write_beats = 0;
  std::uint64_t dfi_data_bytes = 0;
  std::uint64_t dfi_beat_bytes = 0;
  // 在线 Mem PHY 行为级统计。Direct 模式保持为 0。
  std::uint64_t phy_commands = 0;
  std::uint64_t phy_read_requests = 0;
  std::uint64_t phy_write_requests = 0;
  std::uint64_t phy_read_completions = 0;
  std::uint64_t phy_write_completions = 0;
  std::uint64_t phy_command_backpressure = 0;
  std::uint64_t phy_data_backpressure = 0;
  std::uint64_t phy_reset_cycles = 0;
  std::uint64_t phy_initialization_cycles = 0;
  std::uint64_t phy_training_cycles = 0;
  std::uint64_t phy_ca_edges = 0;
  std::uint64_t phy_hbm_row_commands = 0;
  std::uint64_t phy_hbm_column_commands = 0;
  std::uint64_t phy_lpddr_wck_events = 0;
  std::uint64_t phy_lpddr_split_act_events = 0;
  std::uint64_t phy_max_command_fifo = 0;
  std::uint64_t phy_max_read_fifo = 0;
  std::uint64_t phy_max_write_fifo = 0;
  std::uint64_t phy_total_read_service_cycles = 0;
  std::uint64_t phy_total_write_service_cycles = 0;
  std::uint64_t row_policy_ap_upgrades = 0;
  std::uint64_t row_policy_precharges = 0;
  std::uint64_t wck_syncs = 0;
  std::uint64_t wck_sync_skips = 0;
  std::uint64_t wck_training_events = 0;
  std::uint64_t mode_register_ops = 0;
  std::uint64_t dvfs_transitions = 0;
  std::uint64_t ras_ecc_events = 0;
  std::uint64_t act2_deadline_forced = 0;
  std::uint64_t rfm_decrements = 0;
  std::uint64_t low_power_entries = 0;
  std::uint64_t low_power_exits = 0;
  std::uint64_t low_power_cycles = 0;
  std::uint64_t low_power_exit_blocked_cycles = 0;
  // 命令/地址总线保护带来的 bit 级开销，例如 LPDDR6 CA parity。它和
  // interface_read/write_bytes 分开保存，避免把命令开销误认为 payload 数据。
  std::uint64_t interface_command_bits = 0;
  std::uint64_t interface_overhead_bits = 0;
  // frontend host/cache-line 请求与拆分后的物理 DRAM transaction 分开统计。
  // initialized/training 等 Maintenance 请求不计入这两个字段。
  std::uint64_t host_requests = 0;
  std::uint64_t dram_transactions = 0;
  std::uint64_t controller_count = 1;
  std::uint64_t active_controllers = 1;
  std::uint64_t stack_count = 1;
  std::uint64_t active_stacks = 1;
  // system 级 stack ingress/QoS 摘要；per_stack_stats 中同字段收窄到单 stack。
  std::uint64_t stack_ingress_stall_cycles = 0;
  std::uint64_t stack_ingress_peak = 0;
  std::uint64_t qos_priority_dispatches = 0;
  // system_cycles 是外层 MemorySystem 推进的周期数；aggregate_controller_cycles
  // 是所有 channel controller 周期数之和。单 controller 模式下二者相同。
  std::uint64_t system_cycles = 0;
  std::uint64_t aggregate_controller_cycles = 0;
  // 队列长度累计值是所有 controller 的总和；诊断均值统一按
  // aggregate_controller_cycles 归一化，不再重复输出全局平均值别名。
  std::uint64_t read_queue_len_sum = 0;
  std::uint64_t write_queue_len_sum = 0;
  std::uint64_t priority_queue_len_sum = 0;
  std::uint64_t active_queue_len_sum = 0;
  // 读延迟累计值，配合 completed_reads 计算平均值。
  std::uint64_t total_read_latency = 0;
  // 完成 DRAM transaction 折算出的 payload 数据量。HBM4 的一个 64B host
  // line 默认拆成两个 32B transaction，因此不能直接乘 host line_size。
  std::uint64_t read_bytes = 0;
  std::uint64_t write_bytes = 0;
  // payload 加按配置折算的保护/metadata byte；是记账量，不代表已增加总线拍。
  std::uint64_t interface_read_bytes = 0;
  std::uint64_t interface_write_bytes = 0;
  double interface_transfer_rate_gbps = 0.0;          // 每 pin 外部接口速率。
  double peak_bandwidth_GBps = 0.0;                   // data_rate * bus_width / 8。
  double achieved_bandwidth_GBps = 0.0;               // payload 字节换算的实际带宽。
  // payload+metadata/ECC 的记账等效带宽；当前不会据此延长数据总线占用。
  double achieved_interface_bandwidth_GBps = 0.0;
  double bandwidth_utilization = 0.0;                 // achieved / peak。
  double payload_efficiency = 0.0;                    // payload bytes / 记账 interface bytes。
  // 仿真结束时还留在 queue_/pending_ 的请求数，便于定位 cycle limit 问题。
  std::uint64_t remaining_requests = 0;
  std::uint64_t remaining_pending = 0;
  // true 表示 max_cycles 到达但仿真尚未完成。
  bool hit_cycle_limit = false;

  double avg_read_latency() const {
    return completed_reads == 0 ? 0.0 : static_cast<double>(total_read_latency) / completed_reads;
  }

  double read_queue_len_avg_per_controller() const {
    return aggregate_controller_cycles == 0 ? 0.0 :
        static_cast<double>(read_queue_len_sum) / aggregate_controller_cycles;
  }

  double write_queue_len_avg_per_controller() const {
    return aggregate_controller_cycles == 0 ? 0.0 :
        static_cast<double>(write_queue_len_sum) / aggregate_controller_cycles;
  }

  double priority_queue_len_avg_per_controller() const {
    return aggregate_controller_cycles == 0 ? 0.0 :
        static_cast<double>(priority_queue_len_sum) / aggregate_controller_cycles;
  }

  double active_queue_len_avg_per_controller() const {
    return aggregate_controller_cycles == 0 ? 0.0 :
        static_cast<double>(active_queue_len_sum) / aggregate_controller_cycles;
  }
};


}  // namespace hbm_sim
