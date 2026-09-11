// Core 层 MemorySystem：对应 Ramulator2.1 GenericDRAMSystem 的轻量版本。
// 它负责 frontend 请求注入、stack/channel 选择、入口反压/QoS 和
// stack_count*channels 个 Controller 的并行 tick；bank/timing 状态仍由各
// Controller 独立维护。
#include "hbm_sim/core/system.hpp"
#include "hbm_sim/core/stack_model.hpp"
#include "hbm_sim/dram/semantics.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace hbm_sim {
namespace {

std::uint64_t rounded_bytes_from_bits(std::uint64_t bits) {
  return (bits + 7) / 8;
}

std::uint64_t checked_multiply_u64(std::uint64_t lhs, std::uint64_t rhs,
                                   const char *context) {
  if (lhs != 0 && rhs > std::numeric_limits<std::uint64_t>::max() / lhs) {
    throw std::overflow_error(std::string(context) + " exceeds uint64 range");
  }
  return lhs * rhs;
}

MemorySystemOptions legacy_options(ControllerOptions controller) {
  MemorySystemOptions options;
  options.controller = std::move(controller);
  options.channel_mapper = ChannelMapperKind::Decoded;
  return options;
}

void merge_stats(Stats &dst, const Stats &src) {
  // merge_stats 是 controller-aggregate 口径：命令数、队列长度累计值、
  // read/write bytes 等都直接相加。system-level cycle 在 finalize_run_stats()
  // 中单独写入 stats_.system_cycles，避免和 aggregate_controller_cycles 混淆。
  dst.reads += src.reads;
  dst.writes += src.writes;
  dst.completed_reads += src.completed_reads;
  dst.completed_writes += src.completed_writes;
  dst.row_hits += src.row_hits;
  dst.row_misses += src.row_misses;
  dst.row_conflicts += src.row_conflicts;
  dst.act += src.act;
  dst.act1 += src.act1;
  dst.act2 += src.act2;
  dst.pre += src.pre;
  dst.prepb += src.prepb;
  dst.preab += src.preab;
  dst.cas_rd += src.cas_rd;
  dst.cas_wr += src.cas_wr;
  dst.rd += src.rd;
  dst.wr += src.wr;
  dst.rda += src.rda;
  dst.wra += src.wra;
  dst.refab += src.refab;
  dst.refpb += src.refpb;
  dst.refdb += src.refdb;
  dst.rfmab += src.rfmab;
  dst.rfmpb += src.rfmpb;
  dst.mrw += src.mrw;
  dst.mrr += src.mrr;
  dst.wck_sync += src.wck_sync;
  dst.wck_train += src.wck_train;
  dst.dvfs += src.dvfs;
  dst.pde += src.pde;
  dst.pdx += src.pdx;
  dst.srefen += src.srefen;
  dst.srefex += src.srefex;
  dst.ecc_scrub += src.ecc_scrub;
  dst.ras_err += src.ras_err;
  dst.maintenance_requests += src.maintenance_requests;
  dst.maintenance_served += src.maintenance_served;
  dst.refresh_batches += src.refresh_batches;
  dst.refresh_per_bank_batches += src.refresh_per_bank_batches;
  dst.refresh_all_bank_batches += src.refresh_all_bank_batches;
  dst.refresh_postpones += src.refresh_postpones;
  dst.refresh_pullins += src.refresh_pullins;
  dst.refresh_credit_peak =
      std::max(dst.refresh_credit_peak, src.refresh_credit_peak);
  dst.rfm_events += src.rfm_events;
  dst.rfm_per_bank_events += src.rfm_per_bank_events;
  dst.rfm_all_bank_events += src.rfm_all_bank_events;
  dst.dual_issue_cycles += src.dual_issue_cycles;
  dst.row_bus_issues += src.row_bus_issues;
  dst.column_bus_issues += src.column_bus_issues;
  dst.unified_bus_issues += src.unified_bus_issues;
  dst.rising_edge_ticks += src.rising_edge_ticks;
  dst.falling_edge_ticks += src.falling_edge_ticks;
  dst.write_mode_cycles += src.write_mode_cycles;
  dst.injected_requests += src.injected_requests;
  dst.injection_stall_cycles += src.injection_stall_cycles;
  dst.read_forwards += src.read_forwards;
  dst.write_coalesces += src.write_coalesces;
  dst.data_checked_reads += src.data_checked_reads;
  dst.data_mismatches += src.data_mismatches;
  dst.data_uninitialized_reads += src.data_uninitialized_reads;
  dst.data_write_commits += src.data_write_commits;
  dst.data_masked_write_commits += src.data_masked_write_commits;
  dst.data_forward_checks += src.data_forward_checks;
  dst.storage_lines_allocated += src.storage_lines_allocated;
  dst.unique_written_lines += src.unique_written_lines;
  dst.storage_bytes_allocated += src.storage_bytes_allocated;
  dst.storage_topology_lines_scanned += src.storage_topology_lines_scanned;
  dst.storage_topology_scan_skipped += src.storage_topology_scan_skipped;
  dst.storage_stacks_touched += src.storage_stacks_touched;
  dst.storage_dies_touched += src.storage_dies_touched;
  dst.storage_layers_touched += src.storage_layers_touched;
  dst.storage_channels_touched += src.storage_channels_touched;
  dst.storage_pseudo_channels_touched += src.storage_pseudo_channels_touched;
  dst.storage_sids_touched += src.storage_sids_touched;
  dst.storage_ranks_touched += src.storage_ranks_touched;
  dst.storage_bank_groups_touched += src.storage_bank_groups_touched;
  dst.storage_banks_touched += src.storage_banks_touched;
  dst.storage_rows_touched += src.storage_rows_touched;
  dst.storage_columns_touched += src.storage_columns_touched;
  dst.storage_subarrays_touched += src.storage_subarrays_touched;
  dst.storage_mats_touched += src.storage_mats_touched;
  dst.storage_cells_touched += src.storage_cells_touched;
  dst.storage_microbumps_touched += src.storage_microbumps_touched;
  dst.floorplan_tiles_touched += src.floorplan_tiles_touched;
  dst.thermal_tiles_touched += src.thermal_tiles_touched;
  dst.thermal_grid_cells_touched += src.thermal_grid_cells_touched;
  dst.storage_read_line_accesses += src.storage_read_line_accesses;
  dst.storage_write_line_accesses += src.storage_write_line_accesses;
  dst.rowbuf_activations += src.rowbuf_activations;
  dst.rowbuf_precharges += src.rowbuf_precharges;
  dst.rowbuf_dirty_writebacks += src.rowbuf_dirty_writebacks;
  dst.rowbuf_clean_precharges += src.rowbuf_clean_precharges;
  dst.rowbuf_hits += src.rowbuf_hits;
  dst.rowbuf_misses += src.rowbuf_misses;
  dst.rowbuf_lazy_loads += src.rowbuf_lazy_loads;
  dst.rowbuf_reads += src.rowbuf_reads;
  dst.rowbuf_writes += src.rowbuf_writes;
  dst.rowbuf_forced_closes += src.rowbuf_forced_closes;
  dst.rowbuf_open_rows += src.rowbuf_open_rows;
  dst.rowbuf_dirty_rows += src.rowbuf_dirty_rows;
  dst.power_events += src.power_events;
  dst.thermal_updates += src.thermal_updates;
  dst.power_energy_pj += src.power_energy_pj;
  dst.power_act_energy_pj += src.power_act_energy_pj;
  dst.power_pre_energy_pj += src.power_pre_energy_pj;
  dst.power_read_energy_pj += src.power_read_energy_pj;
  dst.power_write_energy_pj += src.power_write_energy_pj;
  dst.power_refresh_energy_pj += src.power_refresh_energy_pj;
  dst.power_rfm_energy_pj += src.power_rfm_energy_pj;
  dst.power_control_energy_pj += src.power_control_energy_pj;
  if (src.thermal_peak_temp_c > dst.thermal_peak_temp_c) {
    dst.thermal_peak_temp_c = src.thermal_peak_temp_c;
    dst.thermal_hotspot_layer = src.thermal_hotspot_layer;
    dst.thermal_hotspot_x = src.thermal_hotspot_x;
    dst.thermal_hotspot_y = src.thermal_hotspot_y;
  }
  dst.thermal_avg_temp_c =
      std::max(dst.thermal_avg_temp_c, src.thermal_avg_temp_c);
  dst.thermal_lateral_transfers += src.thermal_lateral_transfers;
  dst.thermal_vertical_transfers += src.thermal_vertical_transfers;
  dst.thermal_tsv_transfers += src.thermal_tsv_transfers;
  dst.thermal_coupled_delta_c += src.thermal_coupled_delta_c;
  dst.ecc_shadow_updates += src.ecc_shadow_updates;
  dst.ecc_checked_reads += src.ecc_checked_reads;
  dst.ecc_corrected_errors += src.ecc_corrected_errors;
  dst.ecc_uncorrectable_errors += src.ecc_uncorrectable_errors;
  dst.ecc_injected_errors += src.ecc_injected_errors;
  dst.ecc_parity_repairs += src.ecc_parity_repairs;
  dst.dfi_read_beats += src.dfi_read_beats;
  dst.dfi_write_beats += src.dfi_write_beats;
  dst.dfi_forwarded_read_beats += src.dfi_forwarded_read_beats;
  dst.dfi_masked_write_beats += src.dfi_masked_write_beats;
  dst.dfi_data_bytes += src.dfi_data_bytes;
  dst.dfi_beat_bytes = std::max(dst.dfi_beat_bytes, src.dfi_beat_bytes);
  dst.phy_commands += src.phy_commands;
  dst.phy_read_requests += src.phy_read_requests;
  dst.phy_write_requests += src.phy_write_requests;
  dst.phy_read_completions += src.phy_read_completions;
  dst.phy_write_completions += src.phy_write_completions;
  dst.phy_command_backpressure += src.phy_command_backpressure;
  dst.phy_data_backpressure += src.phy_data_backpressure;
  dst.phy_reset_cycles += src.phy_reset_cycles;
  dst.phy_initialization_cycles += src.phy_initialization_cycles;
  dst.phy_training_cycles += src.phy_training_cycles;
  dst.phy_ca_edges += src.phy_ca_edges;
  dst.phy_hbm_row_commands += src.phy_hbm_row_commands;
  dst.phy_hbm_column_commands += src.phy_hbm_column_commands;
  dst.phy_lpddr_wck_events += src.phy_lpddr_wck_events;
  dst.phy_lpddr_split_act_events += src.phy_lpddr_split_act_events;
  dst.phy_max_command_fifo =
      std::max(dst.phy_max_command_fifo, src.phy_max_command_fifo);
  dst.phy_max_read_fifo =
      std::max(dst.phy_max_read_fifo, src.phy_max_read_fifo);
  dst.phy_max_write_fifo =
      std::max(dst.phy_max_write_fifo, src.phy_max_write_fifo);
  dst.phy_total_read_service_cycles += src.phy_total_read_service_cycles;
  dst.phy_total_write_service_cycles += src.phy_total_write_service_cycles;
  dst.row_policy_ap_upgrades += src.row_policy_ap_upgrades;
  dst.row_policy_precharges += src.row_policy_precharges;
  dst.wck_syncs += src.wck_syncs;
  dst.wck_sync_skips += src.wck_sync_skips;
  dst.wck_training_events += src.wck_training_events;
  dst.mode_register_ops += src.mode_register_ops;
  dst.dvfs_transitions += src.dvfs_transitions;
  dst.ras_ecc_events += src.ras_ecc_events;
  dst.act2_deadline_forced += src.act2_deadline_forced;
  dst.rfm_decrements += src.rfm_decrements;
  dst.low_power_entries += src.low_power_entries;
  dst.low_power_exits += src.low_power_exits;
  dst.low_power_cycles += src.low_power_cycles;
  dst.low_power_exit_blocked_cycles += src.low_power_exit_blocked_cycles;
  dst.interface_command_bits += src.interface_command_bits;
  dst.interface_overhead_bits += src.interface_overhead_bits;
  dst.read_queue_len_sum += src.read_queue_len_sum;
  dst.write_queue_len_sum += src.write_queue_len_sum;
  dst.priority_queue_len_sum += src.priority_queue_len_sum;
  dst.active_queue_len_sum += src.active_queue_len_sum;
  dst.total_read_latency += src.total_read_latency;
  dst.read_bytes += src.read_bytes;
  dst.write_bytes += src.write_bytes;
  dst.interface_read_bytes += src.interface_read_bytes;
  dst.interface_write_bytes += src.interface_write_bytes;
  dst.remaining_requests += src.remaining_requests;
  dst.remaining_pending += src.remaining_pending;
  dst.hit_cycle_limit = dst.hit_cycle_limit || src.hit_cycle_limit;
}

} // namespace

MemorySystem::MemorySystem(DramSpec spec, MemorySystemOptions options)
    : spec_(std::move(spec)), options_(options),
      response_delivery_mode_(options.response_delivery_mode) {
  if (options_.stack_count <= 0) {
    throw std::invalid_argument("stack_count must be positive");
  }
  validate_spec(spec_);
  // 在任何维度参与 controller 数量前检查乘法溢出；不能依靠后续
  // Controller 构造间接暴露错误。
  (void)spec_.total_addressable_transactions();
  const std::uint64_t controller_count = checked_multiply_u64(
      static_cast<std::uint64_t>(options_.stack_count),
      static_cast<std::uint64_t>(std::max(1, spec_.org.channels)),
      "controller count");
  if (controller_count >
      static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
    throw std::overflow_error("controller count exceeds size_t range");
  }
  if (options_.stack_interleave_bytes == 0) {
    throw std::invalid_argument("stack_interleave_bytes must be positive");
  }
  if (options_.stack_mapping == StackMappingKind::Interleaved &&
      (options_.stack_interleave_bytes <
           static_cast<std::uint64_t>(spec_.org.line_size) ||
       options_.stack_interleave_bytes %
               static_cast<std::uint64_t>(spec_.org.line_size) !=
           0)) {
    throw std::invalid_argument(
        "interleaved stack mapping requires stack_interleave_bytes to be a "
        "positive multiple of line_size");
  }
  if (options_.stack_ingress_buffer_size == 0 ||
      options_.stack_dispatch_width == 0) {
    throw std::invalid_argument(
        "stack ingress buffer size and dispatch width must be positive");
  }
  int channel_count = std::max(1, spec_.org.channels);
  DramSpec channel_spec = make_channel_spec(spec_);
  memory_images_ = options_.stack_memory_images;
  if (memory_images_.empty() && options_.stack_count == 1 &&
      options_.controller.memory_image) {
    memory_images_.push_back(options_.controller.memory_image);
  }
  if (memory_images_.empty()) {
    memory_images_.reserve(static_cast<std::size_t>(options_.stack_count));
    for (int stack = 0; stack < options_.stack_count; stack++) {
      StorageModelOptions storage;
      storage.stack_id = stack;
      memory_images_.push_back(
          std::make_shared<MemoryImage>(spec_, 0, storage));
    }
  }
  if (memory_images_.size() != static_cast<std::size_t>(options_.stack_count)) {
    throw std::invalid_argument(
        "stack_memory_images size must equal stack_count");
  }
  for (const auto &image : memory_images_) {
    if (!image)
      throw std::invalid_argument("stack_memory_images contains null image");
  }
  controllers_.reserve(static_cast<std::size_t>(controller_count));
  for (int stack = 0; stack < options_.stack_count; stack++) {
    for (int channel = 0; channel < channel_count; channel++) {
      // Controller 只看到一颗 stack 的一个本地 channel；跨 stack/channel 路由
      // 完全由本类负责，因此不同 stack 不共享 bank/timing/refresh 状态。
      ControllerOptions controller_options = options_.controller;
      controller_options.memory_image =
          memory_images_[static_cast<std::size_t>(stack)];
      controller_options.global_channel_id = channel;
      // MemorySystem 需要先接收 Controller completion，才能按响应保留模式丢弃、
      // 暴露 transaction，或聚合 HostResponse。
      controller_options.retain_responses = true;
      controllers_.emplace_back(channel_spec, controller_options);
    }
  }
  active_controller_seen_.assign(controllers_.size(), false);
  active_stack_seen_.assign(static_cast<std::size_t>(options_.stack_count),
                            false);
  stack_ingress_queues_.resize(static_cast<std::size_t>(options_.stack_count));
  next_round_robin_channel_.assign(
      static_cast<std::size_t>(options_.stack_count), 0);
  per_stack_ingress_stalls_.assign(
      static_cast<std::size_t>(options_.stack_count), 0);
  per_stack_ingress_peak_.assign(static_cast<std::size_t>(options_.stack_count),
                                 0);
  per_stack_qos_dispatches_.assign(
      static_cast<std::size_t>(options_.stack_count), 0);
}

MemorySystem::MemorySystem(DramSpec spec, ControllerOptions controller_options)
    : MemorySystem(std::move(spec),
                   legacy_options(std::move(controller_options))) {}

DramSpec MemorySystem::make_channel_spec(const DramSpec &spec) {
  DramSpec channel_spec = spec;
  // 本地化 spec 可以避免 Controller 内部 flat_bank() 把全局 channel
  // 维度也算进去。 collect_issued_commands() 会把本地 channel=0
  // 的命令重新标成全局 channel。
  channel_spec.org.channels = 1;
  // Keep full-device density and refresh timings; this is a local scheduling
  // view, not a newly configured one-channel physical device.
  channel_spec.density_reference_channels = spec.density_reference_channels > 0
      ? spec.density_reference_channels : spec.org.channels;
  return channel_spec;
}

int MemorySystem::target_channel(const Request &req) {
  // channel mapper 是 memory-system 级策略：Request::decoded.channel
  // 是地址映射结果， round_robin/xor 可以覆盖它，用于压力测试多 controller
  // 并行或复现实验配置。
  int channels = std::max(1, spec_.org.channels);
  const bool decoded_channel_is_authoritative =
      req.type == RequestType::Maintenance ||
      options_.channel_mapper == ChannelMapperKind::Decoded;
  if (decoded_channel_is_authoritative &&
      (req.decoded.channel < 0 || req.decoded.channel >= channels)) {
    throw std::out_of_range(
        "request decoded channel is outside the DRAM organization");
  }
  if (channels <= 1) {
    return 0;
  }
  if (req.type == RequestType::Maintenance) {
    // 初始化、MR/WCK training、RAS/ECC 等控制请求已经携带目标 channel。
    // 它们不应该被 workload channel mapper 重写，否则 all-channel init sequence
    // 会在 round_robin/xor 实验中打到错误 controller。
    return req.decoded.channel;
  }

  switch (options_.channel_mapper) {
  case ChannelMapperKind::Decoded:
    return req.decoded.channel;
  case ChannelMapperKind::RoundRobin: {
    const std::size_t stack =
        static_cast<std::size_t>(std::max(0, req.target_stack));
    auto &cursor = next_round_robin_channel_[stack];
    int channel =
        static_cast<int>(cursor % static_cast<std::uint64_t>(channels));
    cursor++;
    return channel;
  }
  case ChannelMapperKind::Xor: {
    std::uint64_t line =
        req.address /
        static_cast<std::uint64_t>(std::max(1, spec_.transaction_bytes()));
    return static_cast<int>((line ^ (line >> 6) ^ (line >> 12)) %
                            static_cast<std::uint64_t>(channels));
  }
  }
  return 0;
}

int MemorySystem::target_stack(const Request &req) const {
  if (req.target_stack >= 0) {
    if (req.target_stack >= options_.stack_count) {
      throw std::out_of_range("request target_stack out of range");
    }
    return req.target_stack;
  }
  StackAddressMapper mapper(
      options_.stack_count, options_.stack_interleave_bytes,
      spec_.addressable_capacity_bytes(), options_.stack_mapping);
  return mapper
      .decode(req.has_system_address ? req.system_address : req.address)
      .stack;
}

std::size_t MemorySystem::controller_index(int stack, int channel) const {
  const int channels = std::max(1, spec_.org.channels);
  return static_cast<std::size_t>(stack * channels + channel);
}

Request MemorySystem::localize_request(Request req, int stack,
                                       int channel) const {
  const Address original =
      req.has_system_address ? req.system_address : req.address;
  if (req.type != RequestType::Maintenance) {
    StackAddressMapper stack_mapper(
        options_.stack_count, options_.stack_interleave_bytes,
        spec_.addressable_capacity_bytes(), options_.stack_mapping);
    StackAddress routed = req.has_explicit_stack
                              ? StackAddress{stack, req.address}
                              : stack_mapper.decode(original);
    if (routed.stack != stack)
      throw std::logic_error("stack routing changed during enqueue");
    req.system_address = req.has_explicit_stack
                             ? stack_mapper.encode(stack, req.address)
                             : original;
    req.has_system_address = true;
    req.address = routed.local_address;
    // 单-stack兼容路径尊重 frontend 已提供的 decoded 坐标；真正跨 stack
    // 路由后地址发生了压缩，必须按 stack-local 地址重新解码。
    if (options_.stack_count > 1) {
      req.decoded = AddressMapper(spec_).decode(req.address);
    }
  }
  req.target_stack = stack;
  req.target_channel = channel;
  // 每个 Controller 只看到自己的局部 channel，因此进入 controller 前把 channel
  // 归零。collect_issued_commands() 会在合并 trace 时再写回全局 channel。
  if (channel >= 0) {
    req.storage_decoded = req.decoded;
    req.storage_decoded.channel = channel;
    req.has_storage_decoded = true;
    req.decoded.channel = 0;
  }
  return req;
}

bool MemorySystem::enqueue(Request req) {
  if (req.qos_class < 0 || req.target_stack < -1 ||
      req.target_channel < -1) {
    throw std::invalid_argument(
        "request qos/stack/channel selectors must be non-negative or unset");
  }
  if (req.has_explicit_stack && req.target_stack < 0) {
    throw std::invalid_argument(
        "request marked with explicit stack is missing target_stack");
  }
  int stack = target_stack(req);
  auto &ingress = stack_ingress_queues_[static_cast<std::size_t>(stack)];
  if (ingress.size() >= options_.stack_ingress_buffer_size) {
    stack_ingress_stall_cycles_++;
    per_stack_ingress_stalls_[static_cast<std::size_t>(stack)]++;
    return false;
  }
  // 普通请求必须先转换为 stack-local 地址，channel mapper
  // 才能基于正确地址工作。
  Request routed = localize_request(req, stack, -1);
  int channel = target_channel(routed);
  routed = localize_request(req, stack, channel);
  routed.system_sequence = next_system_sequence_++;
  ingress.push_back(std::move(routed));
  stack_ingress_peak_ =
      std::max<std::uint64_t>(stack_ingress_peak_, ingress.size());
  per_stack_ingress_peak_[static_cast<std::size_t>(stack)] =
      std::max<std::uint64_t>(
          per_stack_ingress_peak_[static_cast<std::size_t>(stack)],
          ingress.size());
  active_stack_seen_[static_cast<std::size_t>(stack)] = true;
  return true;
}

bool MemorySystem::try_submit(Request request) {
  if (request.type == RequestType::Maintenance) {
    throw std::invalid_argument(
        "try_submit accepts frontend Read/Write requests only; maintenance is "
        "controller-internal");
  }
  if (request.transaction_count == 0 ||
      request.transaction_index >= request.transaction_count) {
    throw std::invalid_argument(
        "try_submit received invalid transaction_index/count");
  }
  auto submission = host_submissions_.find(request.host_request_id);
  if (submission == host_submissions_.end()) {
    if (request.transaction_index != 0) {
      throw std::invalid_argument(
          "the first transaction for a host_request_id must have index 0");
    }
  } else {
    const HostSubmission &host = submission->second;
    if (host.transaction_count != request.transaction_count ||
        host.type != request.type || host.accepted[request.transaction_index]) {
      throw std::invalid_argument("host_request_id reused or transaction "
                                  "submitted more than once while in flight");
    }
  }
  if (response_delivery_mode_ == ResponseDeliveryMode::Disabled) {
    response_delivery_mode_ = ResponseDeliveryMode::HostOnly;
  }
  if (!enqueue(request)) {
    if (!last_async_frontend_stall_cycle_.has_value() ||
        *last_async_frontend_stall_cycle_ != clk_) {
      frontend_stall_cycles_++;
      last_async_frontend_stall_cycle_ = clk_;
    }
    return false;
  }
  record_submitted_transaction(request);
  return true;
}

bool MemorySystem::try_submit_maintenance(Request request) {
  if (request.type != RequestType::Maintenance) {
    throw std::invalid_argument(
        "try_submit_maintenance accepts Maintenance requests only");
  }
  if (enqueue(std::move(request)))
    return true;
  if (!last_async_frontend_stall_cycle_.has_value() ||
      *last_async_frontend_stall_cycle_ != clk_) {
    frontend_stall_cycles_++;
    last_async_frontend_stall_cycle_ = clk_;
  }
  return false;
}

void MemorySystem::record_submitted_transaction(const Request &request) {
  auto [it, inserted] = host_submissions_.try_emplace(request.host_request_id);
  HostSubmission &host = it->second;
  if (inserted) {
    host.transaction_count = request.transaction_count;
    host.type = request.type;
    host.accepted.assign(request.transaction_count, false);
  }
  host.accepted[request.transaction_index] = true;
}

void MemorySystem::record_completed_transaction(
    const TransactionResponse &response) {
  auto it = host_submissions_.find(response.host_request_id);
  if (it == host_submissions_.end()) {
    return; // run() 批处理路径没有 try_submit() 注册表。
  }
  HostSubmission &host = it->second;
  host.completed++;
  // HostOnly/Both 下，tag 还要保护已经 valid、尚未被 frontend pop 的响应。
  // TransactionOnly 没有 HostResponse 握手点，因此最后一个子事务被系统接收
  // 后即可释放。
  if (host.completed >= host.transaction_count && !retains_host_responses()) {
    host_submissions_.erase(it);
  }
}

void MemorySystem::step() { tick(); }

void MemorySystem::finish(std::uint64_t remaining_frontend_requests) {
  remaining_frontend_requests_ = remaining_frontend_requests;
  finalize_run_stats();
  collect_issued_commands();
}

bool MemorySystem::idle() const { return done(); }

bool MemorySystem::responses_drained() const {
  return transaction_responses_.empty() && host_responses_.empty() &&
         host_assemblies_.empty() && host_submissions_.empty() &&
         std::none_of(controllers_.begin(), controllers_.end(),
                      [](const Controller &controller) {
                        return controller.has_response();
                      });
}

void MemorySystem::enable_response_interface(bool enabled) {
  const ResponseDeliveryMode mode =
      enabled && response_delivery_mode_ == ResponseDeliveryMode::Disabled
          ? ResponseDeliveryMode::HostOnly
      : enabled ? response_delivery_mode_
                : ResponseDeliveryMode::Disabled;
  set_response_delivery_mode(mode);
}

void MemorySystem::set_response_delivery_mode(ResponseDeliveryMode mode) {
  if (mode == response_delivery_mode_)
    return;
  if (clk_ != 0 && !quiescent()) {
    throw std::logic_error("response delivery mode may only change while "
                           "MemorySystem is quiescent");
  }
  response_delivery_mode_ = mode;
}

void MemorySystem::set_transaction_response_callback(
    TransactionResponseCallback callback) {
  if (callback) {
    if (response_delivery_mode_ == ResponseDeliveryMode::Disabled) {
      set_response_delivery_mode(ResponseDeliveryMode::TransactionOnly);
    } else if (response_delivery_mode_ == ResponseDeliveryMode::HostOnly) {
      set_response_delivery_mode(ResponseDeliveryMode::Both);
    }
  }
  transaction_response_callback_ = std::move(callback);
}

void MemorySystem::set_response_callback(HostResponseCallback callback) {
  if (callback) {
    if (response_delivery_mode_ == ResponseDeliveryMode::Disabled) {
      set_response_delivery_mode(ResponseDeliveryMode::HostOnly);
    } else if (response_delivery_mode_ ==
               ResponseDeliveryMode::TransactionOnly) {
      set_response_delivery_mode(ResponseDeliveryMode::Both);
    }
  }
  host_response_callback_ = std::move(callback);
}

const HostResponse &MemorySystem::front_response() const {
  if (host_responses_.empty()) {
    throw std::logic_error("MemorySystem host response queue is empty");
  }
  return host_responses_.front();
}

HostResponse MemorySystem::pop_response() {
  if (host_responses_.empty()) {
    throw std::logic_error("MemorySystem host response queue is empty");
  }
  HostResponse response = std::move(host_responses_.front());
  host_responses_.pop_front();
  // frontend tag 直到响应真正握手（pop）后才可复用，而不是在最后一个
  // controller completion 生成时就提前释放。
  auto submission = host_submissions_.find(response.host_request_id);
  if (submission != host_submissions_.end() &&
      submission->second.completed >= submission->second.transaction_count) {
    host_submissions_.erase(submission);
  }
  return response;
}

const TransactionResponse &MemorySystem::front_transaction_response() const {
  if (transaction_responses_.empty()) {
    throw std::logic_error("MemorySystem transaction response queue is empty");
  }
  return transaction_responses_.front();
}

TransactionResponse MemorySystem::pop_transaction_response() {
  if (transaction_responses_.empty()) {
    throw std::logic_error("MemorySystem transaction response queue is empty");
  }
  TransactionResponse response = std::move(transaction_responses_.front());
  transaction_responses_.pop_front();
  return response;
}

bool MemorySystem::transaction_response_queue_full() const {
  return options_.transaction_response_queue_capacity != 0 &&
         transaction_responses_.size() >=
             options_.transaction_response_queue_capacity;
}

bool MemorySystem::host_response_queue_full() const {
  return options_.host_response_queue_capacity != 0 &&
         host_responses_.size() >= options_.host_response_queue_capacity;
}

bool MemorySystem::retains_host_responses() const {
  return response_delivery_mode_ == ResponseDeliveryMode::HostOnly ||
         response_delivery_mode_ == ResponseDeliveryMode::Both;
}

bool MemorySystem::retains_transaction_responses() const {
  return response_delivery_mode_ == ResponseDeliveryMode::TransactionOnly ||
         response_delivery_mode_ == ResponseDeliveryMode::Both;
}

void MemorySystem::collect_responses() {
  if (response_delivery_mode_ == ResponseDeliveryMode::Disabled) {
    // 旧 run() 批处理路径只需要统计和 trace。及时丢弃运行时完成对象可避免
    // 大规模性能实验同时保留 transaction、host 和 payload 三份数据。
    for (auto &controller : controllers_) {
      while (controller.has_response()) {
        TransactionResponse response = controller.pop_response();
        record_completed_transaction(response);
      }
    }
    return;
  }
  for (auto &controller : controllers_) {
    while (controller.has_response()) {
      if ((retains_transaction_responses() &&
           transaction_response_queue_full()) ||
          (retains_host_responses() && host_response_queue_full())) {
        // 任一被选中的对外视图未 ready，就不 pop Controller。Host 容量因此
        // 真正传播到 Controller/pending，而不是落入无界内部 backlog。
        return;
      }
      TransactionResponse response = controller.pop_response();
      record_completed_transaction(response);
      if (retains_transaction_responses()) {
        transaction_responses_.push_back(response);
        if (transaction_response_callback_) {
          transaction_response_callback_(transaction_responses_.back());
        }
      }
      if (retains_host_responses()) {
        aggregate_response(response);
      }
    }
  }
}

void MemorySystem::aggregate_response(const TransactionResponse &response) {
  const std::uint32_t count =
      std::max<std::uint32_t>(1, response.transaction_count);
  if (response.status == ResponseStatus::InvalidTransaction ||
      response.transaction_index >= count) {
    HostResponse invalid;
    invalid.host_request_id = response.host_request_id;
    invalid.type = response.type;
    invalid.system_address = response.system_address;
    invalid.transaction_count = count;
    invalid.stack = response.stack;
    invalid.channel = response.channel;
    invalid.arrival_cycle = response.arrival_cycle;
    invalid.first_issued_cycle = response.issued_cycle;
    invalid.completion_cycle = response.completion_cycle;
    invalid.transactions.push_back(response);
    invalid.initialized = response.initialized;
    invalid.ecc_corrected = response.ecc_corrected;
    invalid.ecc_uncorrectable = response.ecc_uncorrectable;
    invalid.forwarded = response.forwarded;
    invalid.coalesced = response.coalesced;
    invalid.status = ResponseStatus::InvalidTransaction;
    publish_host_response(std::move(invalid));
    return;
  }

  auto [it, inserted] = host_assemblies_.try_emplace(response.host_request_id);
  HostAssembly &assembly = it->second;
  if (inserted) {
    assembly.transaction_count = count;
    assembly.transactions.resize(count);
  }
  if (assembly.transaction_count != count ||
      response.transaction_index >= assembly.transactions.size() ||
      assembly.transactions[response.transaction_index].has_value()) {
    HostResponse invalid;
    invalid.host_request_id = response.host_request_id;
    invalid.type = response.type;
    invalid.system_address = response.system_address;
    invalid.transaction_count = count;
    invalid.transactions.push_back(response);
    invalid.initialized = response.initialized;
    invalid.ecc_corrected = response.ecc_corrected;
    invalid.ecc_uncorrectable = response.ecc_uncorrectable;
    invalid.forwarded = response.forwarded;
    invalid.coalesced = response.coalesced;
    invalid.status = ResponseStatus::InvalidTransaction;
    publish_host_response(std::move(invalid));
    host_assemblies_.erase(it);
    return;
  }

  assembly.transactions[response.transaction_index] = response;
  assembly.received++;
  if (assembly.received == assembly.transaction_count) {
    publish_host_response(build_host_response(assembly));
    host_assemblies_.erase(it);
  }
}

HostResponse
MemorySystem::build_host_response(const HostAssembly &assembly) const {
  HostResponse host;
  host.transaction_count = assembly.transaction_count;
  host.transactions.reserve(assembly.transaction_count);
  host.initialized = true;
  host.forwarded = true;
  host.coalesced = true;
  host.arrival_cycle = std::numeric_limits<Cycle>::max();
  host.first_issued_cycle = std::numeric_limits<Cycle>::max();

  for (const auto &item : assembly.transactions) {
    if (!item.has_value()) {
      host.status = ResponseStatus::InvalidTransaction;
      continue;
    }
    const TransactionResponse &response = *item;
    if (host.transactions.empty()) {
      host.host_request_id = response.host_request_id;
      host.type = response.type;
      host.system_address = response.system_address;
      host.stack = response.stack;
      host.channel = response.channel;
    } else {
      host.system_address =
          std::min(host.system_address, response.system_address);
      if (host.stack != response.stack)
        host.stack = -1;
      if (host.channel != response.channel)
        host.channel = -1;
      if (host.type != response.type) {
        host.status = ResponseStatus::InvalidTransaction;
      }
    }
    host.arrival_cycle = std::min(host.arrival_cycle, response.arrival_cycle);
    host.first_issued_cycle =
        std::min(host.first_issued_cycle, response.issued_cycle);
    host.completion_cycle =
        std::max(host.completion_cycle, response.completion_cycle);
    host.initialized = host.initialized && response.initialized;
    host.ecc_corrected = host.ecc_corrected || response.ecc_corrected;
    host.ecc_uncorrectable =
        host.ecc_uncorrectable || response.ecc_uncorrectable;
    host.forwarded = host.forwarded && response.forwarded;
    host.coalesced = host.coalesced && response.coalesced;
    host.status = merge_response_status(host.status, response.status);
    if (response.type == RequestType::Read) {
      host.data.insert(host.data.end(), response.data.begin(),
                       response.data.end());
      host.initialized_mask.insert(host.initialized_mask.end(),
                                   response.initialized_mask.begin(),
                                   response.initialized_mask.end());
    }
    host.transactions.push_back(response);
  }
  if (host.arrival_cycle == std::numeric_limits<Cycle>::max()) {
    host.arrival_cycle = 0;
  }
  if (host.first_issued_cycle == std::numeric_limits<Cycle>::max()) {
    host.first_issued_cycle = 0;
  }
  return host;
}

void MemorySystem::publish_host_response(HostResponse response) {
  if (host_response_queue_full()) {
    throw std::logic_error(
        "host response queue became full during one completion");
  }
  host_responses_.push_back(std::move(response));
  if (host_response_callback_) {
    host_response_callback_(host_responses_.back());
  }
}

void MemorySystem::dispatch_stack_ingress() {
  for (int stack = 0; stack < options_.stack_count; stack++) {
    auto &queue = stack_ingress_queues_[static_cast<std::size_t>(stack)];
    for (std::size_t slot = 0;
         slot < options_.stack_dispatch_width && !queue.empty(); slot++) {
      auto selected = queue.begin();
      if (options_.stack_qos_policy == StackQosPolicy::StrictPriority) {
        selected =
            std::max_element(queue.begin(), queue.end(),
                             [](const Request &lhs, const Request &rhs) {
                               if (lhs.qos_class != rhs.qos_class)
                                 return lhs.qos_class < rhs.qos_class;
                               return lhs.system_sequence > rhs.system_sequence;
                             });
      }
      const std::size_t index =
          controller_index(stack, selected->target_channel);
      if (!controllers_[index].enqueue(*selected)) {
        // 最高优先级请求的目标 channel 已反压。其他 stack 仍可并行分发；
        // 本 stack 保留请求次序/优先级，下一拍重试。
        break;
      }
      if (selected->qos_class > 0) {
        qos_priority_dispatches_++;
        per_stack_qos_dispatches_[static_cast<std::size_t>(stack)]++;
      }
      active_controller_seen_[index] = true;
      queue.erase(selected);
    }
  }
}

void MemorySystem::run(std::vector<Request> requests, Cycle max_cycles) {
  // MemorySystem 的 run() 与 Controller::run() 类似，但外层每个 system cycle 会
  // tick 所有 controller。这样两个不同 channel 的命令可以出现在同一个 cycle。
  std::stable_sort(requests.begin(), requests.end(),
                   [](const Request &a, const Request &b) {
                     return a.inject_cycle < b.inject_cycle;
                   });

  std::size_t next = 0;
  while ((next < requests.size() || !done()) && clk_ < max_cycles) {
    bool stalled = false;
    while (next < requests.size() && requests[next].inject_cycle <= clk_) {
      if (!enqueue(requests[next])) {
        // 如果目标 channel buffer 满，本次 frontend
        // 注入停在该请求，后续请求也不越过它。 这保留了输入 trace
        // 的时间/顺序语义。
        stalled = true;
        break;
      }
      next++;
    }
    if (stalled) {
      frontend_stall_cycles_++;
    }
    tick();
  }

  remaining_frontend_requests_ = requests.size() - next;
  finalize_run_stats();
  collect_issued_commands();
}

void MemorySystem::run(RequestSource &source, Cycle max_cycles) {
  run_source(source, max_cycles, RunOptions{});
}

void MemorySystem::run(RequestSource &source, Cycle max_cycles,
                       Cycle progress_interval,
                       RunProgressConsumer progress_consumer) {
  RunOptions options;
  options.progress_interval = progress_interval;
  options.progress = std::move(progress_consumer);
  run_source(source, max_cycles, options);
}

void MemorySystem::run(RequestSource &source, Cycle max_cycles,
                       HostResponseConsumer consumer) {
  RunOptions options;
  if (consumer) {
    set_response_delivery_mode(ResponseDeliveryMode::HostOnly);
    options.drain_responses = true;
    options.host_response = std::move(consumer);
  }
  run_source(source, max_cycles, options);
}

void MemorySystem::run(RequestSource &source, Cycle max_cycles,
                       const RunOptions &options) {
  run_source(source, max_cycles, options);
}

void MemorySystem::run_source(RequestSource &source, Cycle max_cycles,
                              const RunOptions &options) {
  const bool drain = options.drain_responses || options.host_response ||
                     options.transaction_response;
  if (drain && response_delivery_mode_ == ResponseDeliveryMode::Disabled) {
    throw std::invalid_argument("response consumers require an enabled response delivery mode");
  }
  if ((options.host_response && !retains_host_responses()) ||
      (options.transaction_response && !retains_transaction_responses())) {
    throw std::invalid_argument("response consumer does not match response delivery mode");
  }
  auto consume_responses = [&] {
    if (!drain) return;
    while (has_response()) {
      HostResponse response = pop_response();
      if (options.host_response) options.host_response(response);
    }
    while (has_transaction_response()) {
      TransactionResponse response = pop_transaction_response();
      if (options.transaction_response) options.transaction_response(response);
    }
  };
  Request pending_request;
  bool has_pending_request = false;
  bool source_done = false;
  Cycle last_inject_cycle = 0;
  bool saw_request = false;
  auto remaining_frontend = [&] {
    std::uint64_t count = has_pending_request ? 1 : 0;
    if (!source_done) count += source.remaining_hint().value_or(1);
    return count;
  };
  while ((!source_done || has_pending_request || !done() ||
          (drain && !responses_drained())) && clk_ < max_cycles) {
    while (true) {
      if (!has_pending_request && !source_done) {
        if (!source.next(pending_request)) {
          source_done = true;
          break;
        }
        if (saw_request && pending_request.inject_cycle < last_inject_cycle)
          throw std::runtime_error("streaming request source is not ordered by inject_cycle");
        saw_request = true;
        last_inject_cycle = pending_request.inject_cycle;
        has_pending_request = true;
      }
      if (!has_pending_request || pending_request.inject_cycle > clk_) break;
      const bool accepted = drain
          ? (pending_request.type == RequestType::Maintenance
                 ? try_submit_maintenance(pending_request) : try_submit(pending_request))
          : enqueue(pending_request);
      if (!accepted) {
        // Async admission already records at most one stall per system tick.
        if (!drain) frontend_stall_cycles_++;
        break;
      }
      has_pending_request = false;
    }
    step();
    if (options.progress && options.progress_interval != 0 &&
        clk_ % options.progress_interval == 0) {
      RunProgress progress;
      progress.cycle = clk_;
      for (const Controller &controller : controllers_) {
        progress.completed_reads += controller.stats().completed_reads;
        progress.completed_writes += controller.stats().completed_writes;
      }
      progress.remaining_frontend = remaining_frontend();
      options.progress(progress);
    }
    consume_responses();
  }
  // Drain already-completed responses even at a zero/expired cycle limit.
  // Freeing a bounded system queue can expose completions still retained by
  // Controllers. Transfer those without ticking or executing unfinished work.
  consume_responses();
  if (drain) {
    while (true) {
      collect_responses();
      if (!has_response() && !has_transaction_response()) break;
      consume_responses();
    }
  }
  finish(remaining_frontend());
}

bool MemorySystem::done() const {
  bool ingress_empty =
      std::all_of(stack_ingress_queues_.begin(), stack_ingress_queues_.end(),
                  [](const auto &queue) { return queue.empty(); });
  return ingress_empty && std::all_of(controllers_.begin(), controllers_.end(),
                                      [](const Controller &controller) {
                                        return controller.done();
                                      });
}

void MemorySystem::tick() {
  // 每颗 stack 先从独立 ingress 向 channel buffer 分发，再并行 tick 全部
  // controller。各 stack/controller 状态独立，循环顺序不会引入额外依赖。
  dispatch_stack_ingress();
  for (auto &controller : controllers_) {
    controller.tick();
  }
  collect_responses();
  clk_++;
}

void MemorySystem::finalize_run_stats() {
  // system_cycles 描述外层 MemorySystem 走了多少拍；aggregate_controller_cycles
  // 描述所有 channel controller 的周期总量。多 channel 下二者差一个
  // controller_count。
  stats_ = Stats{};
  stats_.cycles = clk_;
  stats_.system_cycles = clk_;
  stats_.controller_count = controllers_.size();
  stats_.active_controllers = std::count(active_controller_seen_.begin(),
                                         active_controller_seen_.end(), true);
  stats_.stack_count = static_cast<std::uint64_t>(options_.stack_count);
  stats_.active_stacks =
      std::count(active_stack_seen_.begin(), active_stack_seen_.end(), true);
  stats_.stack_ingress_stall_cycles = stack_ingress_stall_cycles_;
  stats_.stack_ingress_peak = stack_ingress_peak_;
  stats_.qos_priority_dispatches = qos_priority_dispatches_;

  const int channels_per_stack = std::max(1, spec_.org.channels);
  per_stack_stats_.assign(static_cast<std::size_t>(options_.stack_count),
                          Stats{});

  for (std::size_t index = 0; index < controllers_.size(); index++) {
    auto &controller = controllers_[index];
    controller.finalize_run_stats();
    // aggregate_controller_cycles 是所有 controller 的 cycle
    // 总和。它用于队列平均 “per controller” 口径；system_cycles
    // 则用于真实外部时间和带宽计算。
    stats_.aggregate_controller_cycles += controller.stats().cycles;
    merge_stats(stats_, controller.stats());
    Stats &stack_stats =
        per_stack_stats_[index / static_cast<std::size_t>(channels_per_stack)];
    stack_stats.aggregate_controller_cycles += controller.stats().cycles;
    merge_stats(stack_stats, controller.stats());
  }

  stats_.remaining_requests += remaining_frontend_requests_;
  stats_.injection_stall_cycles += frontend_stall_cycles_;
  stats_.hit_cycle_limit =
      stats_.hit_cycle_limit || remaining_frontend_requests_ > 0;
  stats_.interface_transfer_rate_gbps = spec_.interface_transfer_rate_gbps();
  stats_.peak_bandwidth_GBps =
      spec_.peak_bandwidth_GBps() * options_.stack_count;
  if (stats_.cycles > 0 && spec_.cycles_per_second() > 0.0) {
    // 多 controller 模式下 bandwidth 使用 system_cycles 计算，因为 channel
    // 是并行工作的。 如果用 aggregate_controller_cycles，会把并行 channel
    // 误当成串行执行，带宽被低估。
    double bytes = static_cast<double>(stats_.read_bytes + stats_.write_bytes);
    double interface_bytes = static_cast<double>(
        stats_.interface_read_bytes + stats_.interface_write_bytes +
        rounded_bytes_from_bits(stats_.interface_command_bits));
    double seconds =
        static_cast<double>(stats_.cycles) / spec_.cycles_per_second();
    stats_.achieved_bandwidth_GBps =
        seconds <= 0.0 ? 0.0 : bytes / seconds / 1.0e9;
    stats_.achieved_interface_bandwidth_GBps =
        seconds <= 0.0 ? 0.0 : interface_bytes / seconds / 1.0e9;
  }
  stats_.bandwidth_utilization =
      stats_.peak_bandwidth_GBps <= 0.0
          ? 0.0
          : 100.0 * stats_.achieved_bandwidth_GBps / stats_.peak_bandwidth_GBps;
  const auto interface_bytes =
      stats_.interface_read_bytes + stats_.interface_write_bytes +
      rounded_bytes_from_bits(stats_.interface_command_bits);
  const auto payload_bytes = stats_.read_bytes + stats_.write_bytes;
  stats_.payload_efficiency = interface_bytes == 0
                                  ? 100.0
                                  : 100.0 * static_cast<double>(payload_bytes) /
                                        static_cast<double>(interface_bytes);
  std::vector<PhysicalStorageStats> storage_stats;
  storage_stats.reserve(memory_images_.size());
  for (const auto &image : memory_images_) {
    image->flush_dirty_row_buffers(clk_);
    // 正常完成路径必须传播持久化错误。MemoryImage/backend 析构中的 catch
    // 只用于异常展开期间的 noexcept 兜底，不能作为成功退出的唯一 flush。
    image->flush_backend();
    storage_stats.push_back(image->storage_stats());
  }
  apply_physical_storage_stats(stats_,
                               merge_physical_storage_stats(storage_stats));
  stats_.total_addressable_lines =
      checked_multiply_u64(spec_.total_addressable_lines(),
                           static_cast<std::uint64_t>(options_.stack_count),
                           "multi-stack addressable lines");
  stats_.storage_density_pct =
      stats_.total_addressable_lines == 0
          ? 0.0
          : 100.0 * static_cast<double>(stats_.unique_written_lines) /
                static_cast<double>(stats_.total_addressable_lines);

  for (int stack = 0; stack < options_.stack_count; stack++) {
    Stats &per = per_stack_stats_[static_cast<std::size_t>(stack)];
    per.cycles = clk_;
    per.system_cycles = clk_;
    per.controller_count = static_cast<std::uint64_t>(channels_per_stack);
    const auto first =
        active_controller_seen_.begin() + stack * channels_per_stack;
    per.active_controllers =
        std::count(first, first + channels_per_stack, true);
    per.stack_count = 1;
    per.active_stacks =
        active_stack_seen_[static_cast<std::size_t>(stack)] ? 1 : 0;
    per.stack_ingress_stall_cycles =
        per_stack_ingress_stalls_[static_cast<std::size_t>(stack)];
    per.stack_ingress_peak =
        per_stack_ingress_peak_[static_cast<std::size_t>(stack)];
    per.qos_priority_dispatches =
        per_stack_qos_dispatches_[static_cast<std::size_t>(stack)];
    per.interface_transfer_rate_gbps = spec_.interface_transfer_rate_gbps();
    per.peak_bandwidth_GBps = spec_.peak_bandwidth_GBps();
    if (clk_ > 0 && spec_.cycles_per_second() > 0.0) {
      const double seconds =
          static_cast<double>(clk_) / spec_.cycles_per_second();
      per.achieved_bandwidth_GBps =
          static_cast<double>(per.read_bytes + per.write_bytes) / seconds /
          1.0e9;
      const std::uint64_t interface_bytes =
          per.interface_read_bytes + per.interface_write_bytes +
          rounded_bytes_from_bits(per.interface_command_bits);
      per.achieved_interface_bandwidth_GBps =
          static_cast<double>(interface_bytes) / seconds / 1.0e9;
      per.payload_efficiency =
          interface_bytes == 0
              ? 100.0
              : 100.0 * static_cast<double>(per.read_bytes + per.write_bytes) /
                    static_cast<double>(interface_bytes);
    }
    per.bandwidth_utilization =
        per.peak_bandwidth_GBps <= 0.0
            ? 0.0
            : 100.0 * per.achieved_bandwidth_GBps / per.peak_bandwidth_GBps;
    apply_physical_storage_stats(
        per, storage_stats[static_cast<std::size_t>(stack)]);
    per.total_addressable_lines = spec_.total_addressable_lines();
    per.storage_density_pct =
        per.total_addressable_lines == 0
            ? 0.0
            : 100.0 * static_cast<double>(per.unique_written_lines) /
                  static_cast<double>(per.total_addressable_lines);
  }
}

void MemorySystem::collect_issued_commands() {
  issued_.clear();
  const std::size_t channels =
      static_cast<std::size_t>(std::max(1, spec_.org.channels));
  for (std::size_t index = 0; index < controllers_.size(); index++) {
    const int stack = static_cast<int>(index / channels);
    const int channel = static_cast<int>(index % channels);
    for (auto command : controllers_[index].issued_commands()) {
      // Controller 内部 trace 使用本地 channel=0；合并输出时恢复全局 channel，
      // 这样 command validator 和 CSV 都能看到真实 stack/channel 分布。
      command.decoded.channel = channel;
      command.stack_id = stack;
      if (command_meta(command.command).data && command.system_address == 0 &&
          options_.stack_count > 1) {
        StackAddressMapper mapper(
            options_.stack_count, options_.stack_interleave_bytes,
            spec_.addressable_capacity_bytes(), options_.stack_mapping);
        command.system_address = mapper.encode(stack, command.address);
      }
      issued_.push_back(command);
    }
  }
  std::stable_sort(issued_.begin(), issued_.end(),
                   [](const IssuedCommand &a, const IssuedCommand &b) {
                     if (a.cycle != b.cycle)
                       return a.cycle < b.cycle;
                     if (a.stack_id != b.stack_id)
                       return a.stack_id < b.stack_id;
                     if (a.decoded.channel != b.decoded.channel)
                       return a.decoded.channel < b.decoded.channel;
                     return a.request_id < b.request_id;
                   });
}

} // namespace hbm_sim
