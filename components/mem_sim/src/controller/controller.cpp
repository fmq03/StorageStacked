// Controller 协调层：连接 request buffer、scheduler、row policy、maintenance
// manager、 TimingEngine 和
// CommandExecutor。它负责“本周期发哪条命令”，但命令副作用由 CommandExecutor
// 执行，跨 scope timing 由 TimingEngine 维护。
#include "hbm_sim/controller/controller.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <limits>
#include <stdexcept>
#include <utility>

#include "hbm_sim/controller/executor.hpp"
#include "hbm_sim/controller/scheduler.hpp"
#include "hbm_sim/dram/interface.hpp"
#include "hbm_sim/dram/semantics.hpp"
#include "hbm_sim/dram/state.hpp"

namespace hbm_sim {
namespace {

std::uint64_t rounded_bytes_from_bits(std::uint64_t bits) {
  return (bits + 7) / 8;
}

int low_power_exit_latency(const DramSpec &spec) {
  if (spec.low_power_mode == LowPowerMode::SelfRefresh) {
    return spec.self_refresh_exit_cycles > 0 ? spec.self_refresh_exit_cycles
                                             : spec.low_power_exit_cycles;
  }
  return spec.low_power_exit_cycles;
}

std::size_t dfi_beat_bytes(const DramSpec &spec) {
  if (spec.dfi_data_lane_bytes > 0) {
    return static_cast<std::size_t>(spec.dfi_data_lane_bytes);
  }
  std::size_t payload =
      static_cast<std::size_t>(std::max(1, spec.transaction_bytes()));
  std::size_t burst = static_cast<std::size_t>(std::max(1, spec.timing.nBL));
  return std::max<std::size_t>(1, (payload + burst - 1) / burst);
}

std::uint64_t dfi_beats_for_payload(const DramSpec &spec,
                                    std::size_t payload_bytes) {
  std::size_t beat = dfi_beat_bytes(spec);
  return static_cast<std::uint64_t>((payload_bytes + beat - 1) / beat);
}

std::size_t request_data_size(const DramSpec &spec, const Request &req) {
  if (req.transfer_bytes > 0) {
    return req.transfer_bytes;
  }
  if (req.has_expected_payload && !req.expected_payload.empty()) {
    return req.expected_payload.size();
  }
  if (req.has_payload && !req.payload.empty()) {
    return req.payload.size();
  }
  return static_cast<std::size_t>(std::max(1, spec.transaction_bytes()));
}

Address range_end(Address start, std::size_t size) {
  const Address max = std::numeric_limits<Address>::max();
  if (size > static_cast<std::size_t>(max - start)) {
    return max;
  }
  return start + static_cast<Address>(size);
}

bool ranges_overlap(Address a_start, std::size_t a_size, Address b_start,
                    std::size_t b_size) {
  if (a_size == 0 || b_size == 0) {
    return false;
  }
  return a_start < range_end(b_start, b_size) &&
         b_start < range_end(a_start, a_size);
}

void validate_request_decoded(const DramSpec &spec,
                              const DecodedAddress &decoded) {
  const Organization &o = spec.org;
  const auto check = [](int value, int count, const char *name) {
    if (value < 0 || value >= count) {
      throw std::out_of_range(std::string("request decoded ") + name +
                              " is outside the DRAM organization");
    }
  };
  check(decoded.channel, o.channels, "channel");
  check(decoded.pseudo_channel, o.pseudo_channels, "pseudo_channel");
  check(decoded.sid, o.sids, "sid");
  check(decoded.rank, o.ranks, "rank");
  check(decoded.bank_group, o.bank_groups, "bank_group");
  check(decoded.bank, o.banks_per_group, "bank");
  check(decoded.row, o.rows, "row");
  check(decoded.column, o.columns, "column");
}

} // namespace

Controller::Controller(DramSpec spec, ControllerOptions options)
    : spec_(std::move(spec)), options_(options), banks_(spec_.total_banks()),
      request_queues_(spec_.total_banks(), options.priority_buffer_size,
                      options.read_buffer_size, options.write_buffer_size),
      row_policy_(static_cast<std::size_t>(spec_.total_banks()),
                  std::max(1, options_.row_policy_cap)),
      timing_engine_(spec_) {
  validate_spec(spec_);
  if (options_.read_buffer_size == 0 || options_.write_buffer_size == 0 ||
      options_.priority_buffer_size == 0) {
    throw std::invalid_argument("Controller buffer sizes must be positive");
  }
  if (!std::isfinite(options_.write_low_watermark) ||
      !std::isfinite(options_.write_high_watermark) ||
      options_.write_low_watermark < 0.0 ||
      options_.write_high_watermark > 1.0 ||
      options_.write_low_watermark >= options_.write_high_watermark) {
    throw std::invalid_argument(
        "Controller write watermarks must satisfy 0 <= low < high <= 1");
  }
  if (options_.row_policy_cap <= 0) {
    throw std::invalid_argument("Controller row_policy_cap must be positive");
  }
  if (options_.global_channel_id < 0) {
    throw std::invalid_argument("Controller global_channel_id must be non-negative");
  }
  memory_image_ = options_.memory_image;
  if (!memory_image_) {
    memory_image_ = std::make_shared<MemoryImage>(spec_);
  }
  mem_phy_ = std::make_unique<MemPhy>(spec_, options_.phy, memory_image_);
  data_validator_ = options_.data_validator;
  row_policy_.reset(static_cast<std::size_t>(spec_.total_banks()),
                    options_.row_policy_cap);
  refresh_manager_.reset(spec_, clk_);
  rfm_manager_.reset(spec_);
  timing_engine_.reset(spec_);
}

bool Controller::enqueue(Request req) {
  validate_request_decoded(spec_, req.decoded);
  if (req.qos_class < 0 || req.target_stack < -1 || req.target_channel < -1) {
    throw std::invalid_argument(
        "request qos/stack/channel selectors must be non-negative or unset");
  }
  if (req.type != RequestType::Maintenance) {
    const std::uint64_t capacity = memory_image_->capacity_bytes();
    const std::size_t size = request_data_size(spec_, req);
    if (capacity != 0 &&
        (req.address >= capacity ||
        static_cast<std::uintmax_t>(size) >
            static_cast<std::uintmax_t>(capacity - req.address))) {
      throw std::out_of_range("request address range exceeds DRAM capacity");
    }
  }
  req.arrival = clk_;
  if (low_power_active_ && req.type != RequestType::Maintenance) {
    if (explicit_power_down_active_ || explicit_self_refresh_active_) {
      // PDE/SREFEN 已经真实进入 PHY，不能只清 controller 标志。由 controller
      // 注入相应退出命令，且状态只在 PDX/SREFEX 真正 issue 后才改变。
      const Command exit =
          explicit_self_refresh_active_ ? Command::SREFEX : Command::PDX;
      if (!explicit_low_power_exit_pending_.has_value()) {
        schedule_maintenance(exit, req.decoded);
        explicit_low_power_exit_pending_ = exit;
      }
    } else {
      // 自动空闲策略只是 controller 侧 residency 模型，没有向 PHY 发
      // PDE/SREFEN； 因而保留原有的抽象退出延迟即可。
      low_power_active_ = false;
      low_power_exit_until_ = std::max(
          low_power_exit_until_,
          clk_ + timing_delay(std::max(0, low_power_exit_latency(spec_))));
      stats_.low_power_exits++;
    }
  }
  if (req.type == RequestType::Read) {
    if (read_hits_buffered_write(req)) {
      req.controller_sequence = next_controller_sequence_++;
      // Ramulator2.1 ControllerBase 会把命中写缓冲的读请求直接 forward。
      // 这里按 byte range 判断命中，而不是只看完全相同地址，因此 partial/
      // offset write 后的同一 cache line 读也能被数据正确性检查覆盖。
      req.bypass_dram = true;
      req.issued_cycle = clk_;
      req.completion = clk_ + 1;
      stats_.reads++;
      stats_.injected_requests++;
      stats_.read_forwards++;
      bool initialized = true;
      ByteVector initialized_mask;
      const EccStatusCounters storage_before =
          memory_image_->ecc_status_counters();
      req.payload = read_forward_payload(req, &initialized, &initialized_mask);
      const EccStatusCounters storage_after = memory_image_->ecc_status_counters();
      req.response_ecc_corrected = storage_after.ecc_corrected_errors >
                                   storage_before.ecc_corrected_errors;
      req.response_ecc_uncorrectable = storage_after.ecc_uncorrectable_errors >
                                       storage_before.ecc_uncorrectable_errors;
      req.has_payload = true;
      // bypass 读在下一拍才对响应端可见；把快照存在 Request 中，避免后续
      // 写缓冲变化改变已经接受的读结果。
      req.byte_mask = std::move(initialized_mask);
      req.has_byte_mask = true;
      pending_.push_back(std::move(req));
      return true;
    }
    if (!buffer_has_space(BufferKind::Read)) {
      // enqueue() 返回 false 表示 frontend 本 cycle
      // 注入失败。MemorySystem/Controller::run 会保持 next 指针不前进，并统计
      // injection_stall_cycles。
      return false;
    }
    req.controller_sequence = next_controller_sequence_++;
    stats_.reads++;
    stats_.injected_requests++;
    request_queues_[BufferKind::Read].push_back(req);
    return true;
  }

  if (req.type == RequestType::Write) {
    ensure_write_payload(req);
    if (buffered_write_addrs_.count(req.address) > 0) {
      // buffered_writes_ 只能保存同地址写的最终合并值。若中间已有尚未完成的
      // 重叠读，立即合并会让该读看到后到写，因此让 frontend 稍后重试。
      if (has_unresolved_overlapping_read(req, 0)) {
        return false;
      }
      req.controller_sequence = next_controller_sequence_++;
      req.bypass_dram = true;
      req.issued_cycle = clk_;
      req.completion = clk_ + 1;
      // 同地址写请求合并到已有 write/active entry，不再占用队列和总线。
      auto existing = buffered_writes_.find(req.address);
      if (existing == buffered_writes_.end() || !req.has_byte_mask) {
        BufferedWriteState state;
        state.request = req;
        state.byte_sequence.assign(req.payload.size(), req.controller_sequence);
        buffered_writes_[req.address] = std::move(state);
      } else {
        Request &merged = existing->second.request;
        ensure_write_payload(merged);
        req.byte_mask = normalize_mask(req.byte_mask, req.payload.size());
        if (merged.payload.size() < req.payload.size()) {
          merged.payload.resize(req.payload.size(), 0);
        }
        ByteVector merged_mask =
            merged.has_byte_mask
                ? normalize_mask(merged.byte_mask, merged.payload.size())
                : ByteVector(merged.payload.size(), 0xff);
        merged_mask.resize(merged.payload.size(), 0);
        existing->second.byte_sequence.resize(merged.payload.size(), 0);
        for (std::size_t i = 0; i < req.payload.size(); i++) {
          if (req.byte_mask[i] != 0) {
            merged.payload[i] = req.payload[i];
            merged_mask[i] = 0xff;
            existing->second.byte_sequence[i] = req.controller_sequence;
          }
        }
        merged.has_payload = true;
        merged.has_byte_mask =
            !std::all_of(merged_mask.begin(), merged_mask.end(),
                         [](std::uint8_t byte) { return byte != 0; });
        merged.byte_mask =
            merged.has_byte_mask ? std::move(merged_mask) : ByteVector{};
      }
      stats_.writes++;
      stats_.injected_requests++;
      stats_.write_coalesces++;
      // 合并写没有独立 DRAM 命令，但每个被接受的 frontend 请求仍必须得到
      // 一条响应。pending_ 只延迟响应，不会重复提交合并后的数据。
      pending_.push_back(std::move(req));
      return true;
    }
    if (!buffer_has_space(BufferKind::Write)) {
      return false;
    }
    req.controller_sequence = next_controller_sequence_++;
    stats_.writes++;
    stats_.injected_requests++;
    buffered_write_addrs_.insert(req.address);
    BufferedWriteState state;
    state.request = req;
    ByteVector mask = req.has_byte_mask
                          ? normalize_mask(req.byte_mask, req.payload.size())
                          : ByteVector(req.payload.size(), 0xff);
    state.byte_sequence.resize(req.payload.size(), 0);
    for (std::size_t i = 0; i < mask.size(); i++) {
      if (mask[i] != 0)
        state.byte_sequence[i] = req.controller_sequence;
    }
    buffered_writes_[req.address] = std::move(state);
    request_queues_[BufferKind::Write].push_back(req);
    return true;
  }

  if (!buffer_has_space(BufferKind::Priority)) {
    // Maintenance 请求也走队列容量限制。这样 refresh/RFM 高压场景会真实体现
    // 维护请求占用 priority buffer 后对普通请求的反压。
    return false;
  }
  req.controller_sequence = next_controller_sequence_++;
  stats_.maintenance_requests++;
  request_queues_[BufferKind::Priority].push_back(req);
  return true;
}

void Controller::run(std::vector<Request> requests, Cycle max_cycles) {
  // run() 是单 controller 调试路径。MemorySystem 多 controller 路径会自己排序并
  // 分发请求；这里保留同样的 inject_cycle 语义，便于对比单/多 controller 差异。
  std::stable_sort(requests.begin(), requests.end(),
                   [](const Request &a, const Request &b) {
                     return a.inject_cycle < b.inject_cycle;
                   });

  std::size_t next = 0;
  while ((next < requests.size() || !done()) && clk_ < max_cycles) {
    bool stalled = false;
    while (next < requests.size() && requests[next].inject_cycle <= clk_) {
      if (!enqueue(requests[next])) {
        stalled = true;
        break;
      }
      next++;
    }
    if (stalled) {
      stats_.injection_stall_cycles++;
    }
    tick();
  }

  finalize_run_stats();
  stats_.remaining_requests += requests.size() - next;
  stats_.hit_cycle_limit = stats_.hit_cycle_limit || next < requests.size();
}

void Controller::run(RequestSource &source, Cycle max_cycles) {
  run(source, max_cycles, 0, {});
}

void Controller::run(RequestSource &source, Cycle max_cycles,
                     Cycle progress_interval,
                     RunProgressConsumer progress_consumer) {
  Request pending_request;
  bool has_pending_request = false;
  bool source_done = false;
  Cycle last_inject_cycle = 0;
  bool saw_request = false;

  while ((!source_done || has_pending_request || !done()) &&
         clk_ < max_cycles) {
    bool stalled = false;
    while (true) {
      if (!has_pending_request && !source_done) {
        if (!source.next(pending_request)) {
          source_done = true;
          break;
        }
        if (saw_request && pending_request.inject_cycle < last_inject_cycle) {
          throw std::runtime_error(
              "streaming request source is not ordered by inject_cycle");
        }
        saw_request = true;
        last_inject_cycle = pending_request.inject_cycle;
        has_pending_request = true;
      }
      if (!has_pending_request || pending_request.inject_cycle > clk_) {
        break;
      }
      if (!enqueue(pending_request)) {
        stalled = true;
        break;
      }
      has_pending_request = false;
    }
    if (stalled) {
      stats_.injection_stall_cycles++;
    }
    tick();
    if (progress_consumer && progress_interval != 0 &&
        clk_ % progress_interval == 0) {
      std::uint64_t remaining = has_pending_request ? 1 : 0;
      if (!source_done) {
        remaining += source.remaining_hint().value_or(1);
      }
      progress_consumer({clk_, stats_.completed_reads,
                         stats_.completed_writes, remaining});
    }
  }

  finalize_run_stats();
  std::uint64_t remaining = has_pending_request ? 1 : 0;
  if (!source_done) {
    remaining += source.remaining_hint().value_or(1);
  }
  stats_.remaining_requests += remaining;
  stats_.hit_cycle_limit = stats_.hit_cycle_limit || remaining > 0;
}

void Controller::run_until_done(Cycle max_cycles) {
  while (!done() && clk_ < max_cycles) {
    tick();
  }
  finalize_run_stats();
}

void Controller::finalize_run_stats() {
  flush_storage();
  // 单 controller 模式中 system_cycles 和 aggregate_controller_cycles 相同。
  // 多 controller 模式会在 MemorySystem::finalize_run_stats() 中重新聚合。
  stats_.cycles = clk_;
  stats_.system_cycles = clk_;
  stats_.aggregate_controller_cycles = clk_;
  stats_.remaining_requests = queued_requests() + pending_maintenance_.size();
  stats_.remaining_pending = pending_.size();
  stats_.hit_cycle_limit = !done();
  stats_.interface_transfer_rate_gbps = spec_.interface_transfer_rate_gbps();
  stats_.peak_bandwidth_GBps = spec_.peak_bandwidth_GBps();
  if (stats_.cycles > 0 && spec_.cycles_per_second() > 0.0) {
    // achieved_bandwidth_GBps 只统计 payload；achieved_interface_bandwidth_GBps
    // 统计 payload + metadata/ECC overhead，是协议开销的记账等效需求带宽。
    // 当前保护 bit 不延长数据总线占用，不能把它解释为 pin-accurate 实测值。
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
  stats_.dfi_beat_bytes = dfi_beat_bytes(spec_);
  if (mem_phy_) {
    const MemPhyStats &phy = mem_phy_->stats();
    stats_.phy_commands = phy.commands;
    stats_.phy_read_requests = phy.read_requests;
    stats_.phy_write_requests = phy.write_requests;
    stats_.phy_read_completions = phy.read_completions;
    stats_.phy_write_completions = phy.write_completions;
    stats_.phy_command_backpressure = phy.command_backpressure;
    stats_.phy_data_backpressure = phy.data_backpressure;
    stats_.phy_reset_cycles = phy.reset_cycles;
    stats_.phy_initialization_cycles = phy.initialization_cycles;
    stats_.phy_training_cycles = phy.training_cycles;
    stats_.phy_ca_edges = phy.ca_edges;
    stats_.phy_hbm_row_commands = phy.hbm_row_commands;
    stats_.phy_hbm_column_commands = phy.hbm_column_commands;
    stats_.phy_lpddr_wck_events = phy.lpddr_wck_events;
    stats_.phy_lpddr_split_act_events = phy.lpddr_split_activate_events;
    stats_.phy_max_command_fifo = phy.max_command_fifo;
    stats_.phy_max_read_fifo = phy.max_read_fifo;
    stats_.phy_max_write_fifo = phy.max_write_fifo;
    stats_.phy_total_read_service_cycles = phy.total_read_service_cycles;
    stats_.phy_total_write_service_cycles = phy.total_write_service_cycles;
  }
  if (memory_image_) {
    memory_image_->advance_thermal(clk_);
    apply_physical_storage_stats(stats_, memory_image_->storage_stats());
  }
  stats_.total_addressable_lines = spec_.total_addressable_lines();
  stats_.storage_density_pct =
      stats_.total_addressable_lines == 0
          ? 0.0
          : 100.0 * static_cast<double>(stats_.unique_written_lines) /
                static_cast<double>(stats_.total_addressable_lines);
}

void Controller::flush_storage() {
  if (!memory_image_)
    return;
  memory_image_->flush_dirty_row_buffers(clk_);
  memory_image_->flush_backend();
}

bool Controller::done() const {
  return request_queues_[BufferKind::Active].empty() && request_queues_[BufferKind::Priority].empty() &&
         request_queues_[BufferKind::Read].empty() && request_queues_[BufferKind::Write].empty() &&
         pending_maintenance_.empty() && pending_.empty() &&
         (!mem_phy_ || mem_phy_->idle());
}

const TransactionResponse &Controller::front_response() const {
  if (responses_.empty()) {
    throw std::logic_error("Controller response queue is empty");
  }
  return responses_.front();
}

TransactionResponse Controller::pop_response() {
  if (responses_.empty()) {
    throw std::logic_error("Controller response queue is empty");
  }
  TransactionResponse response = std::move(responses_.front());
  responses_.pop_front();
  return response;
}

bool Controller::response_queue_full() const {
  return options_.response_queue_capacity != 0 &&
         responses_.size() >= options_.response_queue_capacity;
}

TransactionResponse Controller::make_response(const Request &req,
                                              ByteVector data,
                                              ByteVector initialized_mask,
                                              bool initialized, bool forwarded,
                                              bool coalesced,
                                              ResponseStatus status) const {
  TransactionResponse response;
  response.request_id = req.id;
  response.host_request_id = req.host_request_id;
  response.transaction_index = req.transaction_index;
  response.transaction_count =
      std::max<std::uint32_t>(1, req.transaction_count);
  response.type = req.type;
  response.system_address =
      req.has_system_address ? req.system_address : req.address;
  response.local_address = req.address;
  response.stack = req.target_stack;
  response.channel =
      req.target_channel >= 0 ? req.target_channel : options_.global_channel_id;
  response.arrival_cycle = req.arrival;
  response.issued_cycle = req.issued_cycle;
  response.completion_cycle = req.completion;
  response.data = std::move(data);
  response.initialized_mask = std::move(initialized_mask);
  response.initialized = initialized;
  response.forwarded = forwarded;
  response.coalesced = coalesced;
  response.status = status;
  if (req.transaction_count == 0 ||
      req.transaction_index >= response.transaction_count) {
    response.status = ResponseStatus::InvalidTransaction;
  }
  return response;
}

void Controller::tick() {
  // tick() 是单个 channel controller 的主循环。顺序故意接近 Ramulator 风格：
  // 1. 推进时间并统计队列长度
  // 2. 结算已完成请求
  // 3. 生成 refresh/RFM/row-policy 等维护请求
  // 4. 在可用命令总线上选择并发射命令
  clk_++;
  if (mem_phy_) {
    mem_phy_->tick(clk_);
  }
  if (spec_.hbm_edge_pairing) {
    if (is_rising_edge()) {
      stats_.rising_edge_ticks++;
    } else {
      stats_.falling_edge_ticks++;
    }
  }

  stats_.read_queue_len_sum += request_queues_[BufferKind::Read].size();
  stats_.write_queue_len_sum += request_queues_[BufferKind::Write].size();
  stats_.priority_queue_len_sum += request_queues_[BufferKind::Priority].size();
  stats_.active_queue_len_sum += request_queues_[BufferKind::Active].size();
  if (write_mode_) {
    stats_.write_mode_cycles++;
  }
  if (low_power_active_) {
    stats_.low_power_cycles++;
  }

  complete_pending();
  if (low_power_exit_until_ > clk_) {
    stats_.low_power_exit_blocked_cycles++;
    return;
  }
  timing_engine_.prune_recent_acts(spec_, clk_);
  schedule_refresh();
  service_pending_maintenance();
  apply_row_policy_pre_schedule();

  if (spec_.dual_command_bus) {
    // HBM-like 模式有独立 column/row command path。rising edge
    // 可以先尝试列命令， 再尝试行命令；falling edge 受 HBM edge pairing
    // 限制，只允许特定 PRE。
    Candidate col;
    if (!spec_.hbm_edge_pairing || is_rising_edge()) {
      col = choose(BusClass::Column);
      if (col.valid) {
        try_upgrade_row_policy_command(col);
        issue(col);
      }
    }

    Candidate row = choose(BusClass::Row);
    if (row.valid) {
      try_upgrade_row_policy_command(row);
      issue(row);
    }

    if (col.valid && row.valid) {
      stats_.dual_issue_cycles++;
    }
  } else {
    // LPDDR-like 模式使用统一命令总线，一个 cycle 只发一条命令。
    Candidate cand = choose(BusClass::Unified);
    if (cand.valid) {
      try_upgrade_row_policy_command(cand);
      issue(cand);
    }
  }

  if (spec_.low_power_mode != LowPowerMode::Off) {
    if (done()) {
      if (low_power_idle_since_ == 0) {
        low_power_idle_since_ = clk_;
      }
      if (!low_power_active_ &&
          clk_ - low_power_idle_since_ >=
              timing_delay(std::max(0, spec_.low_power_entry_cycles))) {
        low_power_active_ = true;
        stats_.low_power_entries++;
      }
    } else {
      low_power_idle_since_ = 0;
    }
  }
}

void Controller::complete_pending() {
  for (auto it = pending_.begin(); it != pending_.end();) {
    if (it->completion > clk_) {
      ++it;
      continue;
    }
    // 输出端未 ready 时保持完成项和响应数据，不从 PHY 取走 completion。
    if (options_.retain_responses && response_queue_full()) {
      ++it;
      continue;
    }

    std::optional<MemPhyCompletion> phy_completion;
    if (!it->bypass_dram && mem_phy_ && mem_phy_->behavioral()) {
      phy_completion =
          mem_phy_->take_completion(it->id, it->controller_sequence);
      if (!phy_completion.has_value()) {
        ++it;
        continue;
      }
      // Behavioral PHY 的 completion 是最终事实口径；不要只依赖 submit_data()
      // 返回的预测周期，为以后增加可变 PHY 服务时间保留接口。
      it->issued_cycle = phy_completion->issued_cycle;
      it->completion = phy_completion->completion_cycle;
    }

    if (it->type == RequestType::Read) {
      // 读请求在 RD/RDA 发出后不会立即完成，而是等 nCL+nBL 的简化读延迟。
      // bypass_dram 的读也在这里进入统一响应路径，但不计真实 DRAM/interface
      // 搬运字节。
      stats_.completed_reads++;
      stats_.total_read_latency += it->completion - it->arrival;
      if (!it->bypass_dram) {
        const std::size_t payload_bytes = request_data_size(spec_, *it);
        const int interface_bytes =
            request_interface_bytes(spec_, payload_bytes);
        stats_.read_bytes += payload_bytes;
        stats_.interface_read_bytes += interface_bytes;
        stats_.interface_overhead_bits +=
            std::max<std::int64_t>(
                0, static_cast<std::int64_t>(interface_bytes) -
                       static_cast<std::int64_t>(payload_bytes)) *
            8;
      }
      TransactionResponse response =
          complete_read(*it, phy_completion ? &*phy_completion : nullptr);
      if (options_.retain_responses) {
        responses_.push_back(std::move(response));
      }
    } else if (it->type == RequestType::Write) {
      // completion 已在发射路径确定：Direct PHY 使用发出后 1 cycle 的轻量
      // 语义，Behavioral PHY 则等待 MemPhy 报告包含写数据时序的完成事件。
      stats_.completed_writes++;
      if (!it->bypass_dram) {
        const std::size_t payload_bytes = request_data_size(spec_, *it);
        const int interface_bytes =
            request_interface_bytes(spec_, payload_bytes);
        stats_.write_bytes += payload_bytes;
        stats_.interface_write_bytes += interface_bytes;
        stats_.interface_overhead_bits +=
            std::max<std::int64_t>(
                0, static_cast<std::int64_t>(interface_bytes) -
                       static_cast<std::int64_t>(payload_bytes)) *
            8;
      }
      TransactionResponse response =
          complete_write(*it, phy_completion ? &*phy_completion : nullptr);
      if (options_.retain_responses) {
        responses_.push_back(std::move(response));
      }
    }
    it = pending_.erase(it);
  }
}

TransactionResponse
Controller::complete_read(Request &req,
                          const MemPhyCompletion *phy_completion) {
  const std::size_t size = request_data_size(spec_, req);
  bool initialized = true;
  DecodedAddress storage = storage_decoded_for(req);
  ByteVector actual;
  ByteVector initialized_mask;
  bool ecc_corrected = req.response_ecc_corrected;
  bool ecc_uncorrectable = req.response_ecc_uncorrectable;
  if (req.bypass_dram) {
    actual = req.payload;
    initialized_mask = req.byte_mask;
    initialized = std::all_of(initialized_mask.begin(), initialized_mask.end(),
                              [](std::uint8_t byte) { return byte != 0; });
  } else if (phy_completion != nullptr) {
    actual = phy_completion->data;
    initialized = phy_completion->initialized;
    initialized_mask = phy_completion->initialized_mask;
    ecc_corrected = phy_completion->ecc_corrected;
    ecc_uncorrectable = phy_completion->ecc_uncorrectable;
  } else {
    const EccStatusCounters storage_before = memory_image_->ecc_status_counters();
    actual = memory_image_->read(req.address, size, &initialized, &storage);
    initialized_mask = memory_image_->read_initialized_mask(
        req.address, actual.size(), &storage);
    const EccStatusCounters storage_after = memory_image_->ecc_status_counters();
    ecc_corrected = storage_after.ecc_corrected_errors >
                    storage_before.ecc_corrected_errors;
    ecc_uncorrectable = storage_after.ecc_uncorrectable_errors >
                        storage_before.ecc_uncorrectable_errors;
  }
  const Command data_cmd = req.issued_data_command == Command::NOP
                               ? Command::RD
                               : req.issued_data_command;
  if (req.bypass_dram) {
    stats_.dfi_forwarded_read_beats +=
        dfi_beats_for_payload(spec_, actual.size());
  } else {
    annotate_issued_data(req, data_cmd, actual, nullptr, &initialized_mask,
                         initialized);
    stats_.dfi_read_beats += dfi_beats_for_payload(spec_, actual.size());
    stats_.dfi_data_bytes += actual.size();
  }
  ResponseStatus status =
      check_read_data(req, actual, initialized, req.bypass_dram);
  if (ecc_corrected) {
    status = merge_response_status(status, ResponseStatus::EccCorrected);
  }
  if (ecc_uncorrectable) {
    status = merge_response_status(status, ResponseStatus::EccUncorrectable);
  }
  // MemoryImage 的自动预充统一落在数据完成点。Controller 的 BankState 会在
  // RDA 发射时立即关闭（调度语义），但物理数据/row-buffer 必须先完成读取，
  // Direct 和 Behavioral PHY 才会得到相同的存储统计与数据顺序。
  if (!req.bypass_dram && data_cmd == Command::RDA) {
    memory_image_->precharge_bank(storage, clk_);
  }
  TransactionResponse response =
      make_response(req, std::move(actual), std::move(initialized_mask),
                    initialized, req.bypass_dram, false, status);
  response.ecc_corrected = ecc_corrected;
  response.ecc_uncorrectable = ecc_uncorrectable;
  return response;
}

TransactionResponse
Controller::complete_write(Request &req,
                           const MemPhyCompletion *phy_completion) {
  if (req.bypass_dram) {
    return make_response(req, {}, {}, true, false, true, ResponseStatus::Ok);
  }
  if (phy_completion != nullptr) {
    ensure_write_payload(req);
    const ByteVector *mask = req.has_byte_mask ? &req.byte_mask : nullptr;
    Command data_cmd = req.issued_data_command == Command::NOP
                           ? Command::WR
                           : req.issued_data_command;
    annotate_issued_data(req, data_cmd, req.payload, mask, nullptr, true);
    req.data_committed = true;
    stats_.dfi_write_beats += dfi_beats_for_payload(spec_, req.payload.size());
    stats_.dfi_data_bytes += req.payload.size();
    if (req.has_byte_mask)
      stats_.dfi_masked_write_beats +=
          dfi_beats_for_payload(spec_, req.payload.size());
    stats_.data_write_commits++;
    if (req.has_byte_mask)
      stats_.data_masked_write_commits++;
    if (data_cmd == Command::WRA) {
      DecodedAddress storage = storage_decoded_for(req);
      memory_image_->precharge_bank(storage, clk_);
    }
    return make_response(req);
  }
  if (!req.data_committed) {
    commit_write_data(req);
  }
  const Command data_cmd = req.issued_data_command == Command::NOP
                               ? Command::WR
                               : req.issued_data_command;
  if (data_cmd == Command::WRA) {
    DecodedAddress storage = storage_decoded_for(req);
    memory_image_->precharge_bank(storage, clk_);
  }
  return make_response(req);
}

void Controller::commit_write_data(Request &req) {
  ensure_write_payload(req);
  const ByteVector *mask = req.has_byte_mask ? &req.byte_mask : nullptr;
  DecodedAddress storage = storage_decoded_for(req);
  memory_image_->write(req.address, req.payload, mask, &storage, req.id, clk_);
  Command data_cmd = req.issued_data_command == Command::NOP
                         ? Command::WR
                         : req.issued_data_command;
  annotate_issued_data(req, data_cmd, req.payload, mask, nullptr, true);
  req.data_committed = true;
  stats_.dfi_write_beats += dfi_beats_for_payload(spec_, req.payload.size());
  stats_.dfi_data_bytes += req.payload.size();
  if (req.has_byte_mask) {
    stats_.dfi_masked_write_beats +=
        dfi_beats_for_payload(spec_, req.payload.size());
  }
  stats_.data_write_commits++;
  if (req.has_byte_mask) {
    stats_.data_masked_write_commits++;
  }
}

ResponseStatus Controller::check_read_data(const Request &req,
                                           const ByteVector &actual,
                                           bool initialized, bool forwarded) {
  if (!initialized) {
    stats_.data_uninitialized_reads++;
  }
  if (!req.has_expected_payload) {
    return initialized ? ResponseStatus::Ok : ResponseStatus::UninitializedData;
  }
  stats_.data_checked_reads++;
  if (forwarded) {
    stats_.data_forward_checks++;
  }
  if (data_validator_) {
    DecodedAddress storage = storage_decoded_for(req);
    DataCheckResult result = data_validator_->check_read(
        clk_, req.id, req.address,
        memory_image_->physical_address(req.address, &storage),
        req.expected_payload, actual, initialized, forwarded,
        memory_image_->metadata(req.address, &storage));
    if (!result.matched) {
      stats_.data_mismatches++;
      return ResponseStatus::DataMismatch;
    }
    return initialized ? ResponseStatus::Ok : ResponseStatus::UninitializedData;
  }
  if (actual != req.expected_payload) {
    stats_.data_mismatches++;
    return ResponseStatus::DataMismatch;
  }
  return initialized ? ResponseStatus::Ok : ResponseStatus::UninitializedData;
}

void Controller::ensure_write_payload(Request &req) {
  if (!req.has_payload || req.payload.empty()) {
    req.payload = make_request_payload(req.address, req.id,
                                       request_data_size(spec_, req));
    req.has_payload = true;
  }
  if (req.has_byte_mask) {
    req.byte_mask = normalize_mask(req.byte_mask, req.payload.size());
  }
}

bool Controller::read_hits_buffered_write(const Request &req) const {
  const std::size_t read_size = request_data_size(spec_, req);
  for (const auto &[write_addr, state] : buffered_writes_) {
    const Request &write_req = state.request;
    const std::size_t write_size = request_data_size(spec_, write_req);
    if (!ranges_overlap(req.address, read_size, write_addr, write_size)) {
      continue;
    }
    if (!write_req.has_byte_mask) {
      return true;
    }
    ByteVector mask = normalize_mask(write_req.byte_mask, write_size);
    const Address begin = std::max(req.address, write_addr);
    const Address end = std::min(range_end(req.address, read_size),
                                 range_end(write_addr, write_size));
    for (Address pos = begin; pos < end; pos++) {
      std::size_t src = static_cast<std::size_t>(pos - write_addr);
      if (src < mask.size() && mask[src] != 0) {
        return true;
      }
    }
  }
  return false;
}

bool Controller::has_unresolved_overlapping_read(
    const Request &write, std::uint64_t older_than_sequence) const {
  const std::size_t write_size = request_data_size(spec_, write);
  auto queue_has_overlap = [&](const std::deque<Request> &queue) {
    return std::any_of(
        queue.begin(), queue.end(), [&](const Request &candidate) {
          if (candidate.type != RequestType::Read) {
            return false;
          }
          if (older_than_sequence != 0 &&
              candidate.controller_sequence >= older_than_sequence) {
            return false;
          }
          return ranges_overlap(write.address, write_size, candidate.address,
                                request_data_size(spec_, candidate));
        });
  };
  return queue_has_overlap(request_queues_[BufferKind::Read]) || queue_has_overlap(request_queues_[BufferKind::Active]) ||
         queue_has_overlap(pending_);
}

bool Controller::has_unresolved_overlapping_write(const Request &read) const {
  const std::size_t read_size = request_data_size(spec_, read);
  return std::any_of(
      pending_.begin(), pending_.end(), [&](const Request &pending) {
        return pending.type == RequestType::Write &&
               ranges_overlap(read.address, read_size, pending.address,
                              request_data_size(spec_, pending));
      });
}

ByteVector
Controller::read_forward_payload(const Request &req, bool *initialized,
                                 ByteVector *initialized_mask) const {
  const std::size_t size = request_data_size(spec_, req);
  DecodedAddress storage = storage_decoded_for(req);
  ByteVector actual = memory_image_->read(req.address, size, nullptr, &storage);
  ByteVector result_mask = memory_image_->read_initialized_mask(
      req.address, actual.size(), &storage);

  std::vector<const BufferedWriteState *> writes;
  writes.reserve(buffered_writes_.size());
  for (const auto &[write_addr, state] : buffered_writes_) {
    const Request &write_req = state.request;
    if (ranges_overlap(req.address, size, write_addr,
                       request_data_size(spec_, write_req))) {
      writes.push_back(&state);
    }
  }

  // 每个目标 byte 独立选择最新的 buffered write。一次 masked merge 只会
  // 更新 mask 覆盖的 byte_sequence，因而不会错误改变同一请求中旧 byte
  // 相对于其他重叠写的顺序。
  std::vector<std::uint64_t> newest_sequence(actual.size(), 0);
  for (const BufferedWriteState *state : writes) {
    const Request &write_req = state->request;
    const std::size_t write_size = request_data_size(spec_, write_req);
    if (!write_req.has_payload || write_req.payload.empty()) {
      continue;
    }
    ByteVector mask =
        write_req.has_byte_mask
            ? normalize_mask(write_req.byte_mask, write_req.payload.size())
            : ByteVector(write_req.payload.size(), 0xff);
    const Address begin = std::max(req.address, write_req.address);
    const Address end = std::min(range_end(req.address, size),
                                 range_end(write_req.address, write_size));
    for (Address pos = begin; pos < end; pos++) {
      std::size_t src = static_cast<std::size_t>(pos - write_req.address);
      std::size_t dst = static_cast<std::size_t>(pos - req.address);
      const std::uint64_t sequence =
          src < state->byte_sequence.size() ? state->byte_sequence[src] : 0;
      if (src < write_req.payload.size() && src < mask.size() &&
          dst < actual.size() && mask[src] != 0 &&
          sequence >= newest_sequence[dst]) {
        actual[dst] = write_req.payload[src];
        newest_sequence[dst] = sequence;
        if (dst < result_mask.size()) {
          result_mask[dst] = 0xff;
        }
      }
    }
  }

  if (initialized != nullptr) {
    *initialized = std::all_of(result_mask.begin(), result_mask.end(),
                               [](std::uint8_t byte) { return byte != 0; });
  }
  if (initialized_mask != nullptr) {
    *initialized_mask = std::move(result_mask);
  }
  return actual;
}

DecodedAddress Controller::storage_decoded_for(const Request &req) const {
  DecodedAddress decoded =
      req.has_storage_decoded ? req.storage_decoded : req.decoded;
  if (!req.has_storage_decoded) {
    decoded.channel = options_.global_channel_id;
  }
  return decoded;
}

void Controller::apply_storage_command_event(const Request &req,
                                             Command issued) {
  if (!memory_image_) {
    return;
  }

  DecodedAddress storage = storage_decoded_for(req);
  std::size_t payload_bytes =
      is_data_command(issued) ? request_data_size(spec_, req) : 0;
  memory_image_->record_command_event(issued, storage, clk_, payload_bytes);
  switch (issued) {
  case Command::ACT:
  case Command::ACT2:
    memory_image_->activate_row(storage, clk_);
    break;
  case Command::PRE:
  case Command::PREPB:
    memory_image_->precharge_bank(storage, clk_);
    break;
  case Command::PREAB:
  case Command::REFAB:
  case Command::RFMAB:
  case Command::SREFEN:
    memory_image_->precharge_all(storage, clk_);
    break;
  case Command::REFPB:
  case Command::REFDB:
  case Command::RFMPB:
    memory_image_->precharge_bank(storage, clk_);
    break;
  default:
    break;
  }
}

Controller::Candidate Controller::choose(BusClass bus) {
  Candidate cand;
  PhyBackpressureObservation phy_block;

  // LPDDR split-activate 中 ACT1 后的 ACT2 不能被普通请求长期饿死；
  // deadline 到达后先尝试服务 owning request，再看维护和读写队列。
  cand = pick_urgent_act2(bus, &phy_block);

  if (!cand.valid && (bus == BusClass::Column || bus == BusClass::Unified)) {
    // active_buffer 保存已经打开/正在打开的请求。优先检查它能最大化 row-hit
    // 和已经投入的 ACT/CAS 工作，接近 Ramulator active buffer 的意义。
    cand = pick_best_ready_from(request_queues_[BufferKind::Active], BufferKind::Active, bus, false,
                                &phy_block);
  }
  if (!cand.valid) {
    // refresh/RFM/row-policy precharge 都进入 priority path。priority
    // 只看队首， 防止维护命令之间互相重排导致 refresh rotation 难以验证。
    cand = pick_priority_if(bus, &phy_block);
  }
  if (!cand.valid && request_queues_[BufferKind::Priority].empty()) {
    // 只有 priority buffer 为空时才调度普通读写，避免维护请求被持续推迟。
    cand = pick_rw_if(bus, &phy_block);
  }

  // 这是“一个 bus 在一个 tick 最终没有可发命令”的唯一计数点。候选必须先通过
  // bus、依赖和 JEDEC timing gate，才会在 phy_admission_ok() 中留下记录。
  if (!cand.valid && mem_phy_) {
    if (phy_block.command)
      mem_phy_->note_command_backpressure();
    if (phy_block.data)
      mem_phy_->note_data_backpressure();
  }

  return cand;
}

Controller::Candidate
Controller::pick_urgent_act2(BusClass bus,
                             PhyBackpressureObservation *phy_block) {
  if (!spec_.split_activate ||
      (bus != BusClass::Unified && bus != BusClass::Row)) {
    return {};
  }
  Candidate best;
  Cycle best_deadline = 0;
  for (std::size_t i = 0; i < request_queues_[BufferKind::Active].size(); i++) {
    Request &req = request_queues_[BufferKind::Active][i];
    auto cmd = next_command(req);
    if (!cmd.has_value() || *cmd != Command::ACT2) {
      continue;
    }
    const BankState &bank = banks_[req.decoded.flat_bank(spec_)];
    if (clk_ > bank.act2_deadline) {
      throw std::runtime_error(
          "LPDDR ACT2 missed nAADMax deadline for request " +
          std::to_string(req.id));
    }
    if (clk_ < bank.act2_deadline ||
        !candidate_eligible(req, *cmd, bus, BufferKind::Active, false) ||
        !timing_ok(req, *cmd) || !phy_admission_ok(*cmd, phy_block)) {
      continue;
    }
    // 若多个 ACT2 同时到达 deadline，先服务 deadline 更早的；deadline 相同则用
    // arrival 打破平局。这样 split-activate 的 owning request 不会被后来的 row
    // hit 或维护请求长期饿死。
    if (!best.valid || bank.act2_deadline < best_deadline ||
        (bank.act2_deadline == best_deadline &&
         req.arrival < request_queues_[BufferKind::Active][best.index].arrival)) {
      best = Candidate{i, BufferKind::Active, *cmd, bus, true};
      best_deadline = bank.act2_deadline;
    }
  }
  if (best.valid) {
    stats_.act2_deadline_forced++;
  }
  return best;
}

Controller::Candidate
Controller::pick_best_ready_from(std::deque<Request> &buffer, BufferKind kind,
                                 BusClass bus, bool avoid_active_close,
                                 PhyBackpressureObservation *phy_block) {
  // Controller 先为 buffer 中每个请求派生“下一条应该发的命令”，再把
  // eligible/ready 信息交给 Scheduler。这样 Scheduler 只表达排序策略，
  // 不需要知道 ACT/PRE/CAS/RD 的协议细节。
  std::vector<SchedulerCandidateView> candidates;
  candidates.reserve(buffer.size());

  for (std::size_t i = 0; i < buffer.size(); i++) {
    auto &req = buffer[i];
    auto cmd = next_command(req);
    if (!cmd.has_value()) {
      continue;
    }
    bool eligible =
        candidate_eligible(req, *cmd, bus, kind, avoid_active_close);
    bool ready = eligible && timing_ok(req, *cmd);
    if (ready) {
      eligible = phy_admission_ok(*cmd, phy_block);
    }
    candidates.push_back(SchedulerCandidateView{
        i,
        req.arrival,
        *cmd,
        eligible,
        ready,
    });
  }

  auto selected = select_scheduled_request(options_.scheduler, candidates);
  if (!selected.has_value()) {
    return {};
  }

  return Candidate{
      selected->request_index, kind, selected->command, bus, true,
  };
}

bool Controller::candidate_eligible(const Request &req, Command cmd,
                                    BusClass bus, BufferKind kind,
                                    bool avoid_active_close) const {
  if (mem_phy_ && mem_phy_->behavioral() && req.type == RequestType::Read &&
      is_data_command(cmd) && has_unresolved_overlapping_write(req)) {
    return false;
  }
  if (req.type == RequestType::Write &&
      has_unresolved_overlapping_read(req, req.controller_sequence)) {
    // Do not acquire an active bank for a write waiting on an older read.
    // Otherwise maintenance waits for this active write, while that older read
    // is blocked by maintenance priority: a circular wait before data issue.
    return false;
  }
  if (!bus_matches(req, cmd, bus)) {
    return false;
  }
  if (avoid_active_close && would_close_active(req, cmd, kind)) {
    return false;
  }
  return true;
}

bool Controller::phy_admission_ok(Command cmd,
                                  PhyBackpressureObservation *phy_block) const {
  if (!mem_phy_)
    return true;
  if (!mem_phy_->can_accept_command(cmd)) {
    if (phy_block)
      phy_block->command = true;
    return false;
  }
  if (is_data_command(cmd) && !mem_phy_->can_accept_data(cmd)) {
    if (phy_block)
      phy_block->data = true;
    return false;
  }
  return true;
}

Controller::Candidate
Controller::pick_priority_if(BusClass bus,
                             PhyBackpressureObservation *phy_block) {
  while (!request_queues_[BufferKind::Priority].empty()) {
    const Request& head = request_queues_[BufferKind::Priority].front();
    const BankState& bank = banks_[head.decoded.flat_bank(spec_)];
    if (head.next != Command::PREPB || bank.activating || bank.open_row >= 0)
      break;
    // Another PRE/refresh/auto-precharge already satisfied this maintenance
    // request. Retire it without emitting an illegal PRE to a closed bank.
    row_policy_.on_issue(options_.row_policy, head.decoded.flat_bank(spec_), Command::PREPB);
    request_queues_[BufferKind::Priority].pop_front();
  }
  if (request_queues_[BufferKind::Priority].empty()) {
    return {};
  }
  Request &req = request_queues_[BufferKind::Priority].front();
  auto cmd = next_command(req);
  if (!cmd.has_value()) {
    return {};
  }
  if (!candidate_eligible(req, *cmd, bus, BufferKind::Priority, true) ||
      !timing_ok(req, *cmd) || !phy_admission_ok(*cmd, phy_block)) {
    return {};
  }
  return Candidate{0, BufferKind::Priority, *cmd, bus, true};
}

Controller::Candidate
Controller::pick_rw_if(BusClass bus, PhyBackpressureObservation *phy_block) {
  set_write_mode();
  if (write_mode_) {
    auto write = pick_best_ready_from(request_queues_[BufferKind::Write], BufferKind::Write, bus, true,
                                      phy_block);
    if (write.valid) return write;
    // A write-drain watermark cannot prevent the older reads needed by queued
    // writes from progressing. Do not relax bank/timing/PHY eligibility.
    const bool read_dependency = std::any_of(request_queues_[BufferKind::Write].begin(), request_queues_[BufferKind::Write].end(),
        [&](const Request& req) {
          return has_unresolved_overlapping_read(req, req.controller_sequence);
        });
    if (read_dependency)
      return pick_best_ready_from(request_queues_[BufferKind::Read], BufferKind::Read, bus, true, phy_block);
    return {};
  }
  return pick_best_ready_from(request_queues_[BufferKind::Read], BufferKind::Read, bus, true,
                              phy_block);
}

void Controller::set_write_mode() {
  // 写缓冲切换沿用 Ramulator 常见高/低水位策略：超过 high 或没有读请求时进入
  // write mode，低于 low
  // 且有读请求时退出。这样避免读写频繁切换带来的总线翻转开销。
  double high = options_.write_high_watermark *
                static_cast<double>(options_.write_buffer_size);
  double low = options_.write_low_watermark *
               static_cast<double>(options_.write_buffer_size);
  if (!write_mode_) {
    if (static_cast<double>(request_queues_[BufferKind::Write].size()) > high ||
        request_queues_[BufferKind::Read].empty()) {
      write_mode_ = true;
    }
  } else {
    if (static_cast<double>(request_queues_[BufferKind::Write].size()) < low &&
        !request_queues_[BufferKind::Read].empty()) {
      write_mode_ = false;
    }
  }
}

std::deque<Request> &Controller::request_buffer(BufferKind kind) {
  return request_queues_[kind];
}
const std::deque<Request> &Controller::request_buffer(BufferKind kind) const {
  return request_queues_[kind];
}
std::size_t Controller::queued_requests() const { return request_queues_.size(); }
bool Controller::buffer_has_space(BufferKind kind) const { return request_queues_.has_space(kind); }

std::optional<Command> Controller::next_command(Request &req) {
  BankState &bank = banks_[req.decoded.flat_bank(spec_)];

  if (is_maintenance_request(req)) {
    // 维护请求的 req.next 是终端命令，例如
    // REFpb/RFMab。若当前状态不允许直接发， Controller 会先插入
    // PREpb/PREab，使维护路径仍复用统一状态机和 timing gate。
    if ((req.next == Command::REFAB || req.next == Command::RFMAB) &&
        any_bank_busy_in_rank(req.decoded)) {
      return Command::PREAB;
    }
    if ((req.next == Command::REFPB || req.next == Command::REFDB ||
         req.next == Command::RFMPB) &&
        (bank.activating || bank.open_row >= 0)) {
      return Command::PREPB;
    }
    if (req.next == Command::REFDB && !dual_bank_target_idle(req.decoded) &&
        any_bank_busy_in_rank(req.decoded)) {
      return Command::PREAB;
    }
    return req.next;
  }

  if (bank.open_row == req.decoded.row) {
    if (spec_.lpddr_family && !wck_ready_for_data(req)) {
      // LPDDR 读写数据命令必须落在 WCK ready window 内。若窗口尚未建立，
      // 需要先发 CAS_RD/CAS_WR；但 CAS 过早会让窗口在 tRCD 前过期，因此这里
      // 会等到本次 CAS 的 ready 点能覆盖后续 RD/WR bank gate。
      const TimingScopeState &wck =
          timing_engine_.wck_state(spec_, req.decoded);
      if (clk_ < wck.wck_ready_at && clk_ < wck.wck_active_until) {
        // CAS 已经发出且 WCK 正在同步；等待 ready_at，避免重复 CAS
        // 把 ready 窗口不断向后推，导致数据命令永远发不出来。
        return std::nullopt;
      }
      const Timing &t = spec_.timing;
      Command data_command =
          req.type == RequestType::Read ? Command::RD : Command::WR;
      Cycle data_gate = std::max(
          req.type == RequestType::Read ? bank.next_rd : bank.next_wr,
          timing_engine_.constraint_ready_at(spec_, req.decoded, data_command));
      int cas_to_data = std::max(t.nCAS, t.nWCK2CK);
      Cycle prospective_ready = clk_ + timing_delay(std::max(1, cas_to_data));
      Cycle prospective_end =
          clk_ + timing_delay(std::max(t.nWCKPST, t.nWCK2CK + 1));
      if (std::max(data_gate, prospective_ready) >= prospective_end) {
        // WCK/CAS 过早启动会在 nRCD 或总线换向 gate 结束前过期，导致重复
        // CAS。等到本次窗口确实覆盖数据命令的最早合法 tick 再发。
        return std::nullopt;
      }
      return req.type == RequestType::Read ? Command::CASRD : Command::CASWR;
    }
    return req.type == RequestType::Read
               ? read_command_for(options_.row_policy)
               : write_command_for(options_.row_policy);
  }

  if (bank.open_row >= 0) {
    return Command::PREPB;
  }

  if (bank.activating) {
    if (req.issued_first_activate) {
      return Command::ACT2;
    }
    return std::nullopt;
  }

  if (spec_.split_activate) {
    return req.issued_first_activate ? Command::ACT2 : Command::ACT1;
  }

  return Command::ACT;
}

bool Controller::timing_ok(const Request &req, Command cmd) const {
  // timing_ok 分三层：
  // - state_ok：语义状态合法性，closed/opened/activating/maintenance
  // - constraint_timing_ok：表驱动作用域约束
  // - switch 内 bank-local/scope-local gate：nRCD/nRP/WCK/tFAW 等当前实现细节
  const BankState &bank = banks_[req.decoded.flat_bank(spec_)];
  const TimingScopeState &row = timing_engine_.row_state(spec_, req.decoded);
  const TimingScopeState &col = timing_engine_.column_state(spec_, req.decoded);
  const TimingScopeState &bg =
      timing_engine_.bank_group_state(spec_, req.decoded);
  const TimingScopeState &wck = timing_engine_.wck_state(spec_, req.decoded);

  if (!state_ok(req, cmd)) {
    return false;
  }
  if (!constraint_timing_ok(req, cmd)) {
    return false;
  }

  switch (cmd) {
  case Command::ACT:
    return !bank.activating && clk_ >= bank.next_act && clk_ >= row.next_row &&
           clk_ >= row.next_act && clk_ >= bg.next_act &&
           timing_engine_.faw_ready(spec_, req.decoded);
  case Command::ACT1:
    // JESD209-6 要求已经发出的 ACT1 必须先由 ACT2 完成，才能在同一 rank
    // 发出另一条 ACT1。普通 RD/WR/PRE/REF（不同 bank）仍可在 tAAD 窗口插空。
    return !bank.activating && !any_bank_activating_in_rank(req.decoded) &&
           clk_ >= bank.next_act && clk_ >= row.next_row &&
           clk_ >= row.next_act && clk_ >= bg.next_act &&
           timing_engine_.faw_ready(spec_, req.decoded);
  case Command::ACT2:
    return bank.activating && req.issued_first_activate &&
           clk_ >= bank.next_act2 && clk_ >= row.next_row;
  case Command::PRE:
  case Command::PREPB:
    return !bank.activating && bank.open_row >= 0 && clk_ >= bank.next_pre &&
           clk_ >= row.next_row;
  case Command::CASRD:
  case Command::CASWR:
    return spec_.lpddr_family && bank.open_row == req.decoded.row &&
           clk_ >= col.next_col && clk_ >= wck.next_cas;
  case Command::RD:
  case Command::RDA:
    return bank.open_row == req.decoded.row && clk_ >= bank.next_rd &&
           clk_ >= bank.next_any_col && clk_ >= col.next_col &&
           clk_ >= bg.next_col &&
           (!spec_.lpddr_family || wck_ready_for_data(req));
  case Command::WR:
  case Command::WRA:
    return bank.open_row == req.decoded.row && clk_ >= bank.next_wr &&
           clk_ >= bank.next_any_col && clk_ >= col.next_col &&
           clk_ >= bg.next_col &&
           (!spec_.lpddr_family || wck_ready_for_data(req));
  case Command::REFPB:
    return !bank.activating && bank.open_row < 0 && clk_ >= bank.next_act &&
           clk_ >= row.next_row && timing_engine_.faw_ready(spec_, req.decoded);
  case Command::REFDB:
    return dual_bank_target_idle(req.decoded) && clk_ >= bank.next_act &&
           clk_ >= banks_[lpddr_refdb_partner(spec_, req.decoded).flat_bank(spec_)].next_act &&
           clk_ >= row.next_row && timing_engine_.faw_ready(spec_, req.decoded);
  case Command::RFMPB:
    return !bank.activating && bank.open_row < 0 && clk_ >= bank.next_act &&
           clk_ >= row.next_row;
  case Command::PREAB:
    return clk_ >= row.next_row;
  case Command::REFAB:
  case Command::RFMAB:
    return !any_bank_busy_in_rank(req.decoded) && clk_ >= row.next_row;
  case Command::MRW:
  case Command::MRR:
  case Command::WCKTRAIN:
  case Command::DVFS:
  case Command::PDE:
  case Command::SREFEN:
  case Command::ECCSCRUB:
    return !any_bank_busy_in_channel(req.decoded) && clk_ >= row.next_row;
  case Command::WCKSYNC:
    return spec_.lpddr_family && clk_ >= col.next_col && clk_ >= wck.next_cas;
  case Command::PDX:
  case Command::SREFEX:
  case Command::RASERR:
    return clk_ >= row.next_row;
  case Command::NOP:
    return false;
  }

  return false;
}

bool Controller::constraint_timing_ok(const Request &req, Command cmd) const {
  return timing_engine_.constraint_ready(spec_, req.decoded, cmd, clk_);
}

bool Controller::bus_matches(const Request &req, Command cmd,
                             BusClass bus) const {
  if (bus == BusClass::Unified) {
    return true;
  }

  if (spec_.hbm_edge_pairing && !is_rising_edge()) {
    // HBM edge pairing 的简化规则：falling edge 不发列命令，只允许满足 pairing
    // 条件的 PRE。这样能表达 HBM4 半周期发射约束，而不把每条 JEDEC edge
    // 表硬编码进调度器。
    if (bus == BusClass::Column) {
      return false;
    }
    if (cmd != Command::PREPB && cmd != Command::PREAB) {
      return false;
    }
    return !spec_.hbm_strict_edge_pairing ||
           can_issue_falling_edge_pre(req, cmd);
  }

  if (is_row_command(cmd)) {
    return bus == BusClass::Row;
  }
  if (is_column_command(cmd)) {
    return bus == BusClass::Column;
  }
  return false;
}

bool Controller::would_close_active(const Request &req, Command cmd,
                                    BufferKind source) const {
  if (source == BufferKind::Active) {
    return false;
  }
  if (cmd != Command::PREPB && cmd != Command::REFPB && cmd != Command::REFDB &&
      cmd != Command::RFMPB && cmd != Command::PREAB && cmd != Command::REFAB &&
      cmd != Command::RFMAB) {
    return false;
  }
  if (is_all_bank_row_command(cmd)) {
    // priority buffer 中的 PREab/REFab/RFMab 不能关闭仍有 active request 的
    // bank， 否则 active_buffer 里的普通请求会在 row 被维护命令关闭后继续发
    // RD/WR。
    const auto [begin, end] = rank_bank_range(req.decoded);
    return std::any_of(request_queues_.active_counts().begin() + begin,
                       request_queues_.active_counts().begin() + end,
                       [](int count) { return count > 0; });
  }
  return request_queues_.active_counts()[req.decoded.flat_bank(spec_)] > 0;
}

bool Controller::is_rising_edge() const { return (clk_ % 2) == 1; }

bool Controller::can_issue_falling_edge_pre(const Request &req,
                                            Command cmd) const {
  if (clk_ != rising_edge_.next_pairing_falling_edge) {
    return true;
  }
  if (rising_edge_.pseudo_channel != req.decoded.pseudo_channel) {
    return true;
  }
  if (is_all_bank_row_command(rising_edge_.command)) {
    return false;
  }
  return cmd == Command::PREPB &&
         bank_key(req.decoded) != rising_edge_.bank_key;
}

void Controller::record_rising_row_command(const Request &req, Command cmd) {
  // rising edge row 命令记录的是下一次 falling edge 是否允许搭配 PRE。
  // ACT 的 pairing window 更长，所以 next_pairing_falling_edge 用 +3；其他 row
  // 命令用 +1。
  rising_edge_.command = cmd;
  rising_edge_.pseudo_channel = req.decoded.pseudo_channel;
  rising_edge_.bank_key =
      is_all_bank_row_command(cmd) ? -1 : bank_key(req.decoded);
  rising_edge_.next_pairing_falling_edge = clk_ + (cmd == Command::ACT ? 3 : 1);
}

int Controller::bank_key(const DecodedAddress &decoded) const {
  return (decoded.sid * spec_.org.bank_groups + decoded.bank_group) *
             spec_.org.banks_per_group +
         decoded.bank;
}

void Controller::issue(Candidate cand) {
  assert(cand.valid);
  auto &buffer = request_buffer(cand.buffer);
  if (cand.index >= buffer.size()) {
    return;
  }

  Command issued = cand.command;
  Request &req = buffer[cand.index];

  // 命令发射先记录 trace，再执行状态转换。这样如果后续 executor 逻辑出错，
  // 测试仍能看到最后一条进入 DRAM 的命令，便于定位。
  append_issued(req, issued, cand.bus);
  if (mem_phy_) {
    mem_phy_->accept_command(req, issued, cand.bus, clk_);
  }
  const int command_overhead_bits =
      command_interface_overhead_bits(spec_, issued);
  if (command_overhead_bits > 0) {
    // LPDDR6 CA parity 这类命令/地址保护不属于读写 payload。当前把它计入
    // command_bits 和总 interface_overhead_bits，用于等效需求带宽与效率统计；
    // 尚未据此增加 CA 总线拍数。
    stats_.interface_command_bits +=
        static_cast<std::uint64_t>(command_overhead_bits);
    stats_.interface_overhead_bits +=
        static_cast<std::uint64_t>(command_overhead_bits);
  }
  if (cand.bus == BusClass::Row) {
    stats_.row_bus_issues++;
  } else if (cand.bus == BusClass::Column) {
    stats_.column_bus_issues++;
  } else {
    stats_.unified_bus_issues++;
  }

  classify_row_status(req);
  apply_storage_command_event(req, issued);
  // Executor 是唯一会修改 bank/timing/RFM 计数和命令计数的地方；
  // Controller 拿到 executor 返回的维护请求后再放入自己的 priority path。
  CommandExecutor executor(spec_, banks_, timing_engine_, rfm_manager_, stats_);
  CommandExecutionResult result = executor.issue(req, issued, clk_);
  if (result.rfm_command.has_value()) {
    schedule_maintenance(result.rfm_command->command,
                         result.rfm_command->decoded);
  }
  if (issued == Command::DVFS &&
      spec_.lpddr_requires_wck_retrain_after_dvfs()) {
    // LPDDR6 的 DVFS transition 通常会使后续 WCK 相位/训练状态失效。这里用
    // channel 级“训练债务”表达：DVFS 后必须显式 WCK_TRAIN，CAS/RD/WR 才能继续。
    wck_retrain_required_ = true;
  } else if (issued == Command::WCKTRAIN) {
    wck_retrain_required_ = false;
  }
  if (issued == Command::PDE) {
    if (!explicit_power_down_active_) {
      stats_.low_power_entries++;
    }
    explicit_power_down_active_ = true;
    low_power_active_ = true;
  } else if (issued == Command::PDX) {
    explicit_power_down_active_ = false;
    if (explicit_low_power_exit_pending_ == Command::PDX) {
      explicit_low_power_exit_pending_.reset();
    }
    low_power_active_ = explicit_self_refresh_active_;
    stats_.low_power_exits++;
  } else if (issued == Command::SREFEN) {
    if (!explicit_self_refresh_active_) {
      stats_.low_power_entries++;
    }
    explicit_self_refresh_active_ = true;
    low_power_active_ = true;
  } else if (issued == Command::SREFEX) {
    explicit_self_refresh_active_ = false;
    if (explicit_low_power_exit_pending_ == Command::SREFEX) {
      explicit_low_power_exit_pending_.reset();
    }
    low_power_active_ = explicit_power_down_active_;
    stats_.low_power_exits++;
  }
  on_row_policy_issue(req, issued);
  if (spec_.hbm_edge_pairing && cand.bus == BusClass::Row && is_rising_edge()) {
    record_rising_row_command(req, issued);
  }
  retire_or_advance(cand, issued);
}

void Controller::retire_or_advance(Candidate cand, Command issued) {
  auto &buffer = request_buffer(cand.buffer);
  if (cand.index >= buffer.size()) {
    return;
  }
  Request &req = buffer[cand.index];

  if (is_terminal_maintenance(req, issued)) {
    // 维护请求以 req.next 作为终端命令。PREpb/PREab 只是前置清理步骤，
    // 不能让请求离队；真正 REF/RFM 发出后才统计 maintenance_served。
    stats_.maintenance_served++;
    erase_request(cand.buffer, cand.index);
    return;
  }

  if (is_opening_command(issued) && !is_maintenance_request(req) &&
      cand.buffer != BufferKind::Active) {
    // 普通请求发出 ACT/ACT1 后进入 active_buffer，后续 ACT2/CAS/RD/WR
    // 继续由 active path 服务，避免另一个请求抢走正在打开的 bank。
    promote_to_active(cand.buffer, cand.index);
    return;
  }

  if (!is_data_command(issued)) {
    // 非终端命令只改变 DRAM 状态，不完成上层请求。例如 ACT/ACT1/ACT2/CAS 之后
    // 请求仍留在 active_buffer 等下一条命令。
    return;
  }

  Request done_req = req;
  done_req.issued_data_command = issued;
  done_req.issued_cycle = clk_;
  if (done_req.type == RequestType::Write) {
    auto pending_write = buffered_writes_.find(done_req.address);
    if (pending_write != buffered_writes_.end()) {
      const Request &merged = pending_write->second.request;
      done_req.payload = merged.payload;
      done_req.expected_payload = merged.expected_payload;
      done_req.byte_mask = merged.byte_mask;
      done_req.has_payload = merged.has_payload;
      done_req.has_expected_payload = merged.has_expected_payload;
      done_req.has_byte_mask = merged.has_byte_mask;
      buffered_writes_.erase(pending_write);
    }
    buffered_write_addrs_.erase(done_req.address);
    if (!mem_phy_ || !mem_phy_->behavioral()) {
      commit_write_data(done_req);
    }
  }
  if (mem_phy_ && mem_phy_->behavioral()) {
    done_req.completion = mem_phy_->submit_data(done_req, issued, clk_);
  } else {
    done_req.completion = (issued == Command::RD || issued == Command::RDA)
                              ? clk_ + timing_delay(spec_.timing.read_latency())
                              : clk_ + 1;
  }
  if (issued == Command::RDA) {
    done_req.completion =
        std::max(done_req.completion,
                 clk_ + timing_delay(std::max(0, spec_.timing.nRTP)));
  } else if (issued == Command::WRA) {
    // WRA 的内部 PRE 不能早于写数据、burst 和 write recovery 完成。把
    // MemoryImage 的关闭点绑定到这个 gate，可避免 Direct 的一拍完成路径比
    // Behavioral PHY 更早关闭物理 row buffer。
    done_req.completion =
        std::max(done_req.completion,
                 clk_ + timing_delay(spec_.timing.nCWL + spec_.timing.nBL +
                                     spec_.timing.nWR));
  }
  // pending_ 是“已发数据命令但上层尚未看到完成”的队列。它让读延迟统计和
  // DRAM 命令发射解耦，便于后续扩展 callback 或 out-of-order completion。
  pending_.push_back(done_req);
  erase_request(cand.buffer, cand.index);
}

void Controller::erase_request(BufferKind kind, std::size_t index) {
  request_queues_.erase(kind, index, spec_);
}
void Controller::promote_to_active(BufferKind kind, std::size_t index) {
  request_queues_.promote(kind, index, spec_);
}

void Controller::classify_row_status(Request &req) {
  if (req.row_status_counted || is_maintenance_request(req)) {
    return;
  }

  const BankState &bank = banks_[req.decoded.flat_bank(spec_)];
  // row status 只在请求第一次真正参与调度时统计一次。之后即使它被 promote 到
  // active_buffer 或经历 CAS/RD 多条命令，也不能重复计 row hit/miss/conflict。
  if (bank.open_row == req.decoded.row) {
    stats_.row_hits++;
  } else if (bank.open_row >= 0) {
    stats_.row_conflicts++;
  } else {
    stats_.row_misses++;
  }
  req.row_status_counted = true;
}

void Controller::annotate_issued_data(const Request &req, Command cmd,
                                      const ByteVector &payload,
                                      const ByteVector *mask,
                                      const ByteVector *initialized_mask,
                                      bool initialized) {
  if (!options_.retain_command_trace) {
    return;
  }
  for (auto it = issued_.rbegin(); it != issued_.rend(); ++it) {
    if (it->request_id != req.id || it->command != cmd ||
        !command_meta(it->command).data) {
      continue;
    }
    it->address = req.address;
    it->payload = payload;
    it->has_payload = true;
    if (mem_phy_ && mem_phy_->behavioral()) {
      it->data_cycle = clk_;
    }
    if (req.type == RequestType::Read && req.has_expected_payload) {
      it->expected_payload = req.expected_payload;
      it->has_expected_payload = true;
    }
    it->payload_initialized = initialized;
    if (mask != nullptr) {
      it->byte_mask = normalize_mask(*mask, payload.size());
      it->has_byte_mask = true;
    } else {
      it->byte_mask.clear();
      it->has_byte_mask = false;
    }
    if (initialized_mask != nullptr && !initialized_mask->empty()) {
      it->initialized_mask = normalize_mask(*initialized_mask, payload.size());
    } else {
      it->initialized_mask.assign(payload.size(), initialized ? 0xff : 0x00);
    }
    return;
  }
}

void Controller::append_issued(const Request &req, Command cmd, BusClass bus) {
  if (options_.retain_command_trace) {
    IssuedCommand issued;
    issued.cycle = clk_;
    issued.request_id = req.id;
    issued.command = cmd;
    issued.bus = bus;
    issued.decoded = req.decoded;
    issued.address = req.address;
    issued.stack_id = std::max(0, req.target_stack);
    issued.system_address =
        req.has_system_address ? req.system_address : req.address;
    issued_.push_back(std::move(issued));
  }
}

bool Controller::is_row_command(Command cmd) const {
  return command_meta(cmd).row_command;
}

bool Controller::is_column_command(Command cmd) const {
  return command_meta(cmd).column_command;
}

bool Controller::is_data_command(Command cmd) const {
  return command_meta(cmd).data;
}

bool Controller::is_opening_command(Command cmd) const {
  return command_meta(cmd).opening;
}

bool Controller::is_maintenance_request(const Request &req) const {
  return req.type == RequestType::Maintenance;
}

bool Controller::is_terminal_maintenance(const Request &req,
                                         Command cmd) const {
  return is_maintenance_request(req) && cmd == req.next;
}

bool Controller::is_all_bank_row_command(Command cmd) const {
  return command_meta(cmd).all_bank;
}

bool Controller::any_bank_busy_in_channel(const DecodedAddress &decoded) const {
  int banks_per_channel = std::max(1, spec_.banks_per_channel());
  int channel =
      std::clamp(decoded.channel, 0, std::max(1, spec_.org.channels) - 1);
  int begin = channel * banks_per_channel;
  int end =
      std::min(static_cast<int>(banks_.size()), begin + banks_per_channel);
  return std::any_of(banks_.begin() + begin, banks_.begin() + end,
                     [](const BankState &bank) {
                       return bank.activating || bank.open_row >= 0;
                     });
}

std::pair<int, int>
Controller::rank_bank_range(const DecodedAddress &decoded) const {
  const int banks_per_rank =
      std::max(1, spec_.org.bank_groups * spec_.org.banks_per_group);
  const int channel =
      std::clamp(decoded.channel, 0, std::max(1, spec_.org.channels) - 1);
  const int pc = std::clamp(decoded.pseudo_channel, 0,
                            std::max(1, spec_.org.pseudo_channels) - 1);
  const int sid = std::clamp(decoded.sid, 0, std::max(1, spec_.org.sids) - 1);
  const int rank =
      std::clamp(decoded.rank, 0, std::max(1, spec_.org.ranks) - 1);
  const int rank_index =
      ((channel * std::max(1, spec_.org.pseudo_channels) + pc) *
           std::max(1, spec_.org.sids) +
       sid) *
          std::max(1, spec_.org.ranks) +
      rank;
  const int begin = rank_index * banks_per_rank;
  const int end =
      std::min(static_cast<int>(banks_.size()), begin + banks_per_rank);
  return {begin, end};
}

bool Controller::any_bank_busy_in_rank(const DecodedAddress &decoded) const {
  const auto [begin, end] = rank_bank_range(decoded);
  return std::any_of(banks_.begin() + begin, banks_.begin() + end,
                     [](const BankState &bank) {
                       return bank.activating || bank.open_row >= 0;
                     });
}

bool Controller::any_bank_activating_in_rank(
    const DecodedAddress &decoded) const {
  const auto [begin, end] = rank_bank_range(decoded);
  return std::any_of(banks_.begin() + begin, banks_.begin() + end,
                     [](const BankState &bank) { return bank.activating; });
}

Cycle Controller::timing_delay(int cycles) const {
  if (cycles <= 0) {
    return 0;
  }
  return static_cast<Cycle>(cycles) *
         static_cast<Cycle>(std::max(1, spec_.tick_multiplier));
}

void Controller::schedule_refresh() {
  const bool ordinary_work = !request_queues_[BufferKind::Active].empty() || !request_queues_[BufferKind::Read].empty() ||
                             !request_queues_[BufferKind::Write].empty();
  const bool allow_pull_in = !ordinary_work && request_queues_[BufferKind::Priority].empty() &&
                             pending_maintenance_.empty() && pending_.empty();
  auto result =
      refresh_manager_.tick(spec_, clk_, ordinary_work, allow_pull_in);
  if (result.postponed) {
    stats_.refresh_postpones++;
  }
  if (result.pulled_in) {
    stats_.refresh_pullins++;
  }
  stats_.refresh_credit_peak = std::max<std::uint64_t>(
      stats_.refresh_credit_peak,
      static_cast<std::uint64_t>(std::max(0, result.credit)));
  // RefreshManager 只产生“应该做 refresh”的抽象维护命令；如果目标 bank 未关闭，
  // next_command() 会先把它展开成 PREpb/PREab，再回到终端 REF 命令。
  for (const auto &command : result.commands) {
    schedule_maintenance(command.command, command.decoded);
  }
  if (result.started_batch) {
    stats_.refresh_batches++;
    if (!result.commands.empty() &&
        result.commands.front().command == Command::REFAB) {
      stats_.refresh_all_bank_batches++;
    } else {
      stats_.refresh_per_bank_batches++;
    }
  }
}

void Controller::service_pending_maintenance() {
  // pending_maintenance_ 是无限小队列，用来暂存 manager 生成但 priority buffer
  // 暂时放不下的维护请求。每个 tick 尽可能搬运，保持维护压力可见。
  while (!pending_maintenance_.empty() &&
         buffer_has_space(BufferKind::Priority)) {
    request_queues_[BufferKind::Priority].push_back(pending_maintenance_.front());
    pending_maintenance_.pop_front();
  }
}

void Controller::schedule_maintenance(Command cmd,
                                      const DecodedAddress &decoded) {
  // 维护请求没有真实地址，decoded 直接携带目标 bank/scope。id 使用独立的
  // next_maintenance_id_，避免和 frontend request id 混淆。
  Request req;
  req.id = next_maintenance_id_++;
  req.type = RequestType::Maintenance;
  req.next = cmd;
  req.address = 0;
  req.decoded = decoded;
  req.inject_cycle = clk_;
  req.arrival = clk_;
  pending_maintenance_.push_back(req);
  stats_.maintenance_requests++;
}

void Controller::apply_row_policy_pre_schedule() {
  if (options_.row_policy != RowPolicyKind::ClosedCap) {
    return;
  }
  for (int flat = 0; flat < static_cast<int>(row_policy_.bank_count());
       flat++) {
    if (!row_policy_.should_schedule_precharge(options_.row_policy, flat,
                                               request_queues_.active_counts()[flat])) {
      continue;
    }
    DecodedAddress decoded = decoded_from_flat_bank(flat);
    Request req;
    req.id = next_maintenance_id_++;
    req.type = RequestType::Maintenance;
    req.next = Command::PREPB;
    req.decoded = decoded;
    req.inject_cycle = clk_;
    req.arrival = clk_;
    if (!timing_ok(req, Command::PREPB) ||
        !buffer_has_space(BufferKind::Priority)) {
      // ClosedCAP 的显式 PRE 只有在当前 cycle 真的 ready 且 priority
      // 有空间时才注入， 防止创建大量等待 PRE
      // 的维护请求，反而遮蔽调度器本身的行为。
      continue;
    }
    request_queues_[BufferKind::Priority].push_back(req);
    row_policy_.mark_precharge_pending(flat);
    stats_.maintenance_requests++;
    stats_.row_policy_precharges++;
  }
}

void Controller::try_upgrade_row_policy_command(Candidate &cand) {
  if (!cand.valid || options_.row_policy != RowPolicyKind::ClosedCap) {
    return;
  }
  if (cand.command != Command::RD && cand.command != Command::WR) {
    return;
  }
  auto &buffer = request_buffer(cand.buffer);
  if (cand.index >= buffer.size() ||
      is_maintenance_request(buffer[cand.index])) {
    return;
  }
  Request &req = buffer[cand.index];
  int flat = req.decoded.flat_bank(spec_);
  if (!row_policy_.should_auto_precharge(options_.row_policy, flat)) {
    return;
  }
  Command upgraded = cand.command == Command::RD ? Command::RDA : Command::WRA;
  if (bus_matches(req, upgraded, cand.bus) && timing_ok(req, upgraded)) {
    cand.command = upgraded;
    stats_.row_policy_ap_upgrades++;
  }
}

void Controller::on_row_policy_issue(const Request &req, Command issued) {
  row_policy_.on_issue(options_.row_policy, req.decoded.flat_bank(spec_),
                       issued);
}

DecodedAddress Controller::decoded_from_flat_bank(int flat_bank) const {
  // 这是 flat_bank() 的反向操作，主要给 row policy 自动 PRE 使用。
  // row/column 不参与 flat bank，因此保持默认 0 即可。
  DecodedAddress decoded;
  const Organization &o = spec_.org;
  int x = std::max(0, flat_bank);
  int banks_per_group = std::max(1, o.banks_per_group);
  int bank_groups = std::max(1, o.bank_groups);
  int ranks = std::max(1, o.ranks);
  int sids = std::max(1, o.sids);
  int pseudo_channels = std::max(1, o.pseudo_channels);

  decoded.bank = x % banks_per_group;
  x /= banks_per_group;
  decoded.bank_group = x % bank_groups;
  x /= bank_groups;
  decoded.rank = x % ranks;
  x /= ranks;
  decoded.sid = x % sids;
  x /= sids;
  decoded.pseudo_channel = x % pseudo_channels;
  x /= pseudo_channels;
  decoded.channel = x;
  return decoded;
}

bool Controller::dual_bank_target_idle(const DecodedAddress &decoded) const {
  const BankState &bank = banks_[decoded.flat_bank(spec_)];
  if (bank.activating || bank.open_row >= 0 || clk_ < bank.next_act) {
    return false;
  }
  DecodedAddress partner = lpddr_refdb_partner(spec_, decoded);
  const BankState &partner_bank = banks_[partner.flat_bank(spec_)];
  return !partner_bank.activating && partner_bank.open_row < 0 &&
         clk_ >= partner_bank.next_act;
}

bool Controller::wck_ready_for_data(const Request &req) const {
  return timing_engine_.wck_ready_for_data(spec_, req.decoded, clk_);
}

bool Controller::state_ok(const Request &req, Command cmd) const {
  const BankState &bank = banks_[req.decoded.flat_bank(spec_)];
  CommandStateSnapshot snapshot;
  // CommandStateSnapshot 把在线 BankState 压缩成 command_state.cpp
  // 需要的协议语义。 这样状态机模块不依赖 Controller 的内部字段，也便于
  // Validator 复用同一套规则。
  if (bank.activating) {
    snapshot.bank_state = BankProtocolState::Activating;
  } else if (bank.open_row >= 0) {
    snapshot.bank_state = BankProtocolState::Opened;
  } else {
    snapshot.bank_state = BankProtocolState::Closed;
  }
  snapshot.row_hit = bank.open_row == req.decoded.row;
  snapshot.wck_ready = wck_ready_for_data(req);
  // PREab/REFab/RFMab 的 all-bank 作用域是目标 rank。若这里使用整个
  // channel，另一 pseudo-channel 的打开行会让目标 rank 无限重复 PREab。
  snapshot.any_bank_busy =
      (cmd == Command::PREAB || cmd == Command::REFAB || cmd == Command::RFMAB)
          ? any_bank_busy_in_rank(req.decoded)
          : any_bank_busy_in_channel(req.decoded);
  snapshot.split_activate = spec_.split_activate;
  snapshot.lpddr_family = spec_.lpddr_family;
  snapshot.maintenance = is_maintenance_request(req);
  snapshot.issued_first_activate = req.issued_first_activate;
  snapshot.low_power_active =
      explicit_power_down_active_ ||
      (low_power_active_ && spec_.low_power_mode != LowPowerMode::SelfRefresh);
  snapshot.self_refresh_active =
      explicit_self_refresh_active_ ||
      (low_power_active_ && spec_.low_power_mode == LowPowerMode::SelfRefresh);
  snapshot.training_active = false;
  snapshot.dvfs_transition_active = false;
  snapshot.wck_training_required = wck_retrain_required_;
  snapshot.ras_ecc_supported =
      spec_.supports_ecc || spec_.lpddr_link_ecc_enabled;
  snapshot.link_retry_supported =
      spec_.hbm_link_retry_enabled || spec_.lpddr_link_protection;
  return check_command_state(snapshot, cmd).legal;
}

} // namespace hbm_sim
