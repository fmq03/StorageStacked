#include "hif2mc/adapter.hpp"
#include <algorithm>
#include <cmath>
#include <deque>
#include <limits>
#include <list>
#include <optional>
#include <tuple>

namespace hif2mc {
void Config::validate() const {
  if (!address_unit_bytes || !command_bytes || command_bytes % 32 ||
      !hif_period_ps || !lpr_credits || !write_credits || !max_inflight ||
      credit_policy == CreditPolicy::Unspecified)
    throw std::invalid_argument(
        "HIF contract missing address/length/clock/capacity/credit policy");
  if (command_bytes > 4096 || address_unit_bytes > command_bytes ||
      command_bytes % address_unit_bytes ||
      (command_bytes & (command_bytes - 1)) ||
      (address_unit_bytes & (address_unit_bytes - 1)))
    throw std::invalid_argument(
        "v1 requires power-of-two address units/command bytes <=4096");
  if (!address_bits || address_bits > 64 || !token_bits || token_bits > 64 ||
      !pointer_bits || pointer_bits > 32 || pointer_delay_cycles < 2 ||
      !write_data_fifo_beats || !write_assembly_interval)
    throw std::invalid_argument(
        "invalid HIF widths, pointer latency or write FIFO");
  if (lpr_credits > max_inflight || hpr_credits > max_inflight ||
      write_credits > max_inflight)
    throw std::invalid_argument("each credit pool must fit max_inflight");
}
namespace {
bool fits(std::uint64_t v, unsigned bits) {
  return bits == 64 || (v >> bits) == 0;
}
auto command_fields(const Inputs &i) {
  return std::tuple{i.hif_cmd_type,    i.hif_cmd_addr,   i.hif_cmd_pri,
                    i.hif_cmd_token,   i.hif_cmd_length, i.hif_cmd_wdata_ptr,
                    i.hif_cmd_autopre, i.hif_cmd_latency};
}
bool overlap(std::uint64_t a, std::uint64_t b, std::size_t size) {
  return a <= b ? b - a < size : a - b < size;
}
} // namespace
struct Adapter::Impl {
  enum class Phase { WaitData, Read, WaitRmw, Write, ReadReady, Done };
  struct Stage {
    std::size_t submitted = 0;
    std::vector<bool> issued;
    bool complete = false, credit_released = false;
  };
  struct Parent {
    std::uint64_t id, address, accepted_cycle;
    Inputs command;
    Command type;
    Phase phase;
    bool address_error = false, pointer_scheduled = false,
         pointer_notified = false;
    bool ecc_error = false;
    std::uint64_t data_eligible_cycle = 0;
    std::size_t received_beats = 0, assembled_beats = 0, emit_beat = 0;
    Bytes data, mask, old_data, old_initialized, result;
    Stage read, write;
  };
  struct WriteBeat {
    std::uint64_t parent;
    std::size_t offset;
    DataBeat data;
    std::uint32_t mask;
  };
  Config config;
  std::unique_ptr<Backend> memory;
  Statistics statistics;
  std::list<Parent> parents;
  std::deque<std::uint64_t> awaiting_data, ready_reads;
  std::deque<WriteBeat> write_fifo;
  std::array<std::size_t, 3> available, pending_credit{};
  Outputs registered;
  std::optional<std::uint64_t> pointer_output;
  std::optional<Inputs> stalled_command, stalled_data;
  std::uint64_t now = 0, next_parent = 1, next_child = 1;
  long double time_remainder = 0;
  bool poisoned = false;

  Impl(Config c, std::unique_ptr<Backend> b)
      : config(c), memory(std::move(b)),
        available{c.lpr_credits, c.hpr_credits, c.write_credits} {
    config.validate();
    if (!memory)
      throw std::invalid_argument("null backend");
    const auto tx = memory->transaction_bytes();
    const auto period = memory->tick_period_ps();
    if (!tx || tx > c.command_bytes || c.command_bytes % tx ||
        !std::isfinite(period) || period <= 0 ||
        static_cast<long double>(c.hif_period_ps) / period > 1000000 ||
        memory->capacity_bytes() < c.command_bytes)
      throw std::invalid_argument(
          "backend transaction, capacity or clock incompatible with HIF");
  }
  Parent &get(std::uint64_t id) {
    auto p = std::find_if(parents.begin(), parents.end(),
                          [=](const Parent &x) { return x.id == id; });
    if (p == parents.end())
      throw std::logic_error("unknown HIF parent");
    return *p;
  }
  const Parent &get(std::uint64_t id) const {
    return const_cast<Impl *>(this)->get(id);
  }
  std::array<unsigned, 3> pulse(const Outputs &o) const {
    return {o.hif_lpr_credit, o.hif_hpr_credit, unsigned(o.hif_wr_credit)};
  }
  Outputs evaluate(const Inputs &i) const {
    if (poisoned || !i.core_ddrc_rstn)
      return {};
    auto out = registered;
    const auto returned = pulse(out);
    auto has = [&](std::size_t k) {
      return available[k] + (config.reuse_credit_same_edge ? returned[k] : 0) >
             0;
    };
    bool resource =
        i.hif_cmd_type == 1 ? has(i.hif_cmd_pri == 1 ? 1 : 0) : has(2);
    if (i.hif_cmd_type == 2)
      resource = resource && has(0);
    // Context storage is separate from credits: credits may return before data.
    out.hif_cmd_stall = parents.size() >= config.max_inflight || !resource;
    if (i.hif_cmd_valid) {
      for (const auto &p : parents) {
        if (i.hif_cmd_type == 1 && p.type == Command::Read &&
            p.command.hif_cmd_token == i.hif_cmd_token)
          out.hif_cmd_stall = true;
        if (i.hif_cmd_type != 1 && p.type != Command::Read &&
            p.command.hif_cmd_wdata_ptr == i.hif_cmd_wdata_ptr)
          out.hif_cmd_stall = true;
      }
    }
    out.hif_wdata_stall = awaiting_data.empty() ||
                          write_fifo.size() >= config.write_data_fifo_beats;
    if (!awaiting_data.empty())
      out.hif_wdata_stall =
          out.hif_wdata_stall ||
          get(awaiting_data.front()).data_eligible_cycle > now;
    return out;
  }
  void validate_command(const Inputs &i) const {
    if (i.hif_cmd_type > 2)
      throw ProtocolError("reserved HIF command type 11");
    if (i.hif_cmd_type == 2 && !config.enable_rmw)
      throw ProtocolError("RMW disabled by HIF profile");
    if (i.hif_cmd_type == 1 && i.hif_cmd_pri == 1 && !config.hpr_credits)
      throw ProtocolError("HPR disabled by HIF profile");
    if (!fits(i.hif_cmd_addr, config.address_bits) ||
        (i.hif_cmd_type == 1 && !fits(i.hif_cmd_token, config.token_bits)) ||
        (i.hif_cmd_type != 1 &&
         !fits(i.hif_cmd_wdata_ptr, config.pointer_bits)))
      throw ProtocolError("HIF field exceeds configured wire width");
    // CONTRACT: full aligned access only; no inferred partial-length encoding.
    if (i.hif_cmd_length || i.hif_cmd_latency || i.hif_cmd_autopre ||
        i.hif_cmd_pri > 1 || (i.hif_cmd_type != 1 && i.hif_cmd_pri != 0))
      throw ProtocolError(
          "unsupported length/autopre/VPR/latency/priority contract");
  }
  void validate_edge(const Inputs &i, const Outputs &out) const {
    if (i.hif_go2critical_lpr || i.hif_go2critical_hpr || i.hif_go2critical_wr)
      throw ProtocolError("go2critical scheduling is not implemented");
    if (config.check_stalled_inputs && stalled_command &&
        (!i.hif_cmd_valid ||
         command_fields(*stalled_command) != command_fields(i)))
      throw ProtocolError("command changed or valid dropped while stalled");
    if (config.check_stalled_inputs && stalled_data &&
        (!i.hif_wdata_valid || stalled_data->hif_wdata != i.hif_wdata ||
         stalled_data->hif_wdata_mask != i.hif_wdata_mask ||
         stalled_data->hif_wdata_end != i.hif_wdata_end))
      throw ProtocolError(
          "write data/mask/end changed or valid dropped while stalled");
    if (i.hif_cmd_valid)
      validate_command(i);
    if (i.hif_wdata_valid && !out.hif_wdata_stall) {
      const auto &p = get(awaiting_data.front());
      const bool last = p.received_beats + 1 == config.command_bytes / 32;
      if (i.hif_wdata_end != last)
        throw ProtocolError("write end disagrees with configured beat count");
    }
  }
  void accept_command(const Inputs &i) {
    if (next_parent >= (std::numeric_limits<std::uint64_t>::max() >> 1))
      throw std::overflow_error("HIF host ID exhausted");
    const auto type = static_cast<Command>(i.hif_cmd_type);
    Parent p{};
    p.id = next_parent++;
    p.command = i;
    p.type = type;
    p.accepted_cycle = now;
    p.phase = type == Command::Write ? Phase::WaitData : Phase::Read;
    p.address_error =
        i.hif_cmd_addr >
        std::numeric_limits<std::uint64_t>::max() / config.address_unit_bytes;
    if (!p.address_error) {
      p.address = i.hif_cmd_addr * config.address_unit_bytes;
      p.address_error =
          p.address % config.command_bytes ||
          p.address >= memory->capacity_bytes() ||
          config.command_bytes > memory->capacity_bytes() - p.address;
    }
    if (p.address_error)
      ++statistics.address_errors;
    p.data.resize(config.command_bytes);
    p.mask.resize(config.command_bytes);
    const auto count = config.command_bytes / memory->transaction_bytes();
    p.read.issued.resize(count);
    p.write.issued.resize(count);
    if (type == Command::Read) {
      --available[i.hif_cmd_pri == 1 ? 1 : 0];
      ++statistics.accepted_reads;
    } else {
      --available[2];
      if (type == Command::Rmw) {
        --available[0];
        ++statistics.accepted_rmws;
      } else
        ++statistics.accepted_writes;
    }
    parents.push_back(std::move(p));
  }
  void release(Parent &p, bool write, bool fallback = false) {
    auto &stage = write ? p.write : p.read;
    if (stage.credit_released)
      return;
    stage.credit_released = true;
    const std::size_t k =
        write                                                   ? 2
        : p.type == Command::Read && p.command.hif_cmd_pri == 1 ? 1
                                                                : 0;
    ++pending_credit[k];
    if (fallback)
      ++statistics.credit_fallbacks;
  }
  void maybe_release(Parent &p, bool write, bool completion) {
    auto &s = write ? p.write : p.read;
    if (config.credit_policy == CreditPolicy::BackendCompleted) {
      if (completion)
        release(p, write);
    } else {
      const bool issued = std::all_of(s.issued.begin(), s.issued.end(),
                                      [](bool v) { return v; });
      if (issued || completion)
        release(p, write, completion && !issued);
    }
  }
  void assemble_write() {
    if (write_fifo.empty() || now % config.write_assembly_interval)
      return;
    const auto b = write_fifo.front();
    write_fifo.pop_front();
    auto &p = get(b.parent);
    std::copy(b.data.begin(), b.data.end(),
              p.data.begin() + static_cast<std::ptrdiff_t>(b.offset));
    for (std::size_t j = 0; j < 32; ++j)
      p.mask[b.offset + j] = (b.mask >> j) & 1 ? 0xff : 0;
    ++p.assembled_beats;
  }
  bool data_ready(const Parent &p) const {
    return p.assembled_beats == config.command_bytes / 32;
  }
  void advance_parents() {
    for (auto &p : parents) {
      if (p.address_error) {
        if (p.type == Command::Read && p.phase == Phase::Read) {
          p.result.assign(config.command_bytes, 0);
          p.phase = Phase::ReadReady;
          release(p, false);
          ready_reads.push_back(p.id);
        } else if (p.type != Command::Read) {
          if (p.type == Command::Rmw)
            release(p, false);
          if (data_ready(p)) {
            release(p, true);
            p.phase = Phase::Done;
          }
        }
      } else if (p.phase == Phase::WaitData && data_ready(p))
        p.phase = Phase::Write;
      else if (p.phase == Phase::WaitRmw && data_ready(p)) {
        if (p.old_data.size() != config.command_bytes ||
            p.old_initialized.size() != config.command_bytes)
          throw std::logic_error("RMW old data/initialization size mismatch");
        for (std::size_t j = 0; j < config.command_bytes; ++j) {
          if (!p.mask[j] && !p.old_initialized[j])
            throw ProtocolError("RMW would preserve uninitialized bytes; no "
                                "HIF late-write-error channel");
          if (!p.mask[j])
            p.data[j] = p.old_data[j];
        }
        std::fill(p.mask.begin(), p.mask.end(), 0xff);
        p.phase = Phase::Write;
      }
    }
  }
  bool blocked_by_older(const Parent &p) const {
    for (const auto &older : parents) {
      if (older.id == p.id)
        break;
      if (older.address_error || older.phase == Phase::Done ||
          older.phase == Phase::ReadReady)
        continue;
      if (overlap(older.address, p.address, config.command_bytes))
        return true;
    }
    return false;
  }
  void submit_one() {
    for (auto &p : parents) {
      if (p.address_error ||
          (p.phase != Phase::Read && p.phase != Phase::Write) ||
          blocked_by_older(p))
        continue;
      const bool write = p.phase == Phase::Write;
      auto &s = write ? p.write : p.read;
      if (s.submitted == s.issued.size())
        continue;
      const auto bytes = memory->transaction_bytes();
      const auto offset = s.submitted * bytes;
      BackendRequest q;
      q.id = next_child;
      q.host_id = (p.id << 1) | unsigned(write);
      q.address = p.address + offset;
      q.bytes = bytes;
      q.write = write;
      q.index = static_cast<std::uint32_t>(s.submitted);
      q.count = static_cast<std::uint32_t>(s.issued.size());
      q.qos = p.type == Command::Read ? p.command.hif_cmd_pri : 0;
      if (write) {
        q.data.assign(p.data.begin() + static_cast<std::ptrdiff_t>(offset),
                      p.data.begin() +
                          static_cast<std::ptrdiff_t>(offset + bytes));
        q.mask.assign(p.mask.begin() + static_cast<std::ptrdiff_t>(offset),
                      p.mask.begin() +
                          static_cast<std::ptrdiff_t>(offset + bytes));
      }
      if (memory->try_submit(q)) {
        ++s.submitted;
        ++next_child;
        ++statistics.submitted_children;
      } else
        ++statistics.submission_retries;
      return; // bounded to one child per backend tick
    }
  }
  void consume_events() {
    while (auto event = memory->pop_event()) {
      const auto &e = *event;
      auto &p = get(e.host_id >> 1);
      const bool write = (e.host_id & 1) != 0;
      if (write != e.write)
        throw std::logic_error("backend event direction mismatch");
      auto &s = write ? p.write : p.read;
      if (e.kind == BackendEvent::Kind::Issued) {
        if (e.index >= s.submitted || s.issued[e.index])
          throw std::logic_error("duplicate/unsubmitted issue event");
        s.issued[e.index] = true;
        maybe_release(p, write, false);
        continue;
      }
      if (s.complete || s.submitted != s.issued.size())
        throw std::logic_error("early/duplicate host completion");
      s.complete = true;
      if (e.status == Status::Retry || e.status == Status::Timeout ||
          e.status == Status::DataMismatch ||
          (write && (e.status == Status::EccUncorrectable ||
                     e.status == Status::Invalid)))
        throw ProtocolError("backend error has no agreed HIF representation; "
                            "simulation stopped");
      if (p.type == Command::Rmw && !write &&
          (e.status == Status::EccUncorrectable || e.status == Status::Invalid))
        throw ProtocolError(
            "RMW internal read failed; no late-write-error channel");
      maybe_release(p, write, true);
      if (write)
        p.phase = Phase::Done;
      else {
        if (e.data.size() != config.command_bytes)
          throw std::logic_error("backend read length mismatch");
        if (e.status == Status::Uninitialized)
          ++statistics.uninitialized_reads;
        if (p.type == Command::Rmw) {
          p.old_data = e.data;
          p.old_initialized = e.initialized_mask;
          p.phase = Phase::WaitRmw;
        } else {
          p.result = e.data;
          p.ecc_error = e.status == Status::EccUncorrectable;
          p.address_error = e.status == Status::Invalid;
          p.phase = Phase::ReadReady;
          ready_reads.push_back(p.id);
        }
      }
    }
  }
  void retire_done() {
    for (auto it = parents.begin(); it != parents.end();) {
      if (it->phase != Phase::Done) {
        ++it;
        continue;
      }
      if (it->type == Command::Read)
        ++statistics.completed_reads;
      else if (it->type == Command::Rmw)
        ++statistics.completed_rmws;
      else
        ++statistics.completed_writes;
      it = parents.erase(it);
    }
  }
  void prepare_outputs() {
    registered = {};
    pointer_output.reset();
    std::array<unsigned, 3> credits{};
    for (std::size_t k = 0; k < 3; ++k) {
      if (pending_credit[k]) {
        --pending_credit[k];
        credits[k] = 1;
      }
    }
    registered.hif_lpr_credit = static_cast<std::uint8_t>(credits[0]);
    registered.hif_hpr_credit = static_cast<std::uint8_t>(credits[1]);
    registered.hif_wr_credit = credits[2] != 0;
    for (auto &p : parents) {
      if (p.type == Command::Read || p.pointer_scheduled ||
          now < p.accepted_cycle + config.pointer_delay_cycles)
        continue;
      registered.hif_wdata_ptr_valid = true;
      registered.hif_wdata_ptr = p.command.hif_cmd_wdata_ptr;
      registered.hif_wdata_ptr_addr_err = p.address_error;
      p.pointer_scheduled = true;
      pointer_output = p.id;
      break;
    }
    if (!ready_reads.empty()) {
      auto &p = get(ready_reads.front());
      registered.hif_rdata_valid = true;
      registered.hif_rdata_token = p.command.hif_cmd_token;
      registered.hif_rdata_end = p.emit_beat + 1 == config.command_bytes / 32;
      std::copy_n(p.result.begin() +
                      static_cast<std::ptrdiff_t>(p.emit_beat * 32),
                  32, registered.hif_rdata.begin());
      registered.hif_rdata_addr_err = p.address_error;
      registered.hif_rdata_uncorr_ecc_err = p.ecc_error;
    }
  }
  bool idle() const {
    return !poisoned && parents.empty() && write_fifo.empty() &&
           awaiting_data.empty() && ready_reads.empty() && !pointer_output &&
           !registered.hif_rdata_valid &&
           pending_credit == std::array<std::size_t, 3>{} &&
           pulse(registered) == std::array<unsigned, 3>{} && memory->idle();
  }
  void tick(const Inputs &i) {
    if (poisoned)
      throw ProtocolError(
          "adapter is stopped after a previous protocol/backend error");
    if (!i.core_ddrc_rstn) {
      if (!idle())
        throw ProtocolError(
            "hot reset with outstanding transactions is unsupported");
      registered = {};
      pointer_output.reset();
      stalled_command.reset();
      stalled_data.reset();
      available = {config.lpr_credits, config.hpr_credits,
                   config.write_credits};
      // Idle interface reset preserves backend data, clock and lifetime stats.
      ++now;
      return;
    }
    const auto out = evaluate(i);
    validate_edge(i, out);
    const auto credits = pulse(out);
    const std::array<std::size_t, 3> maximum{
        config.lpr_credits, config.hpr_credits, config.write_credits};
    for (std::size_t k = 0; k < 3; ++k) {
      available[k] += credits[k];
      statistics.returned_credits[k] += credits[k];
      if (available[k] > maximum[k])
        throw std::logic_error("HIF credit over-return");
    }
    if (out.hif_rdata_valid) {
      auto &p = get(ready_reads.front());
      ++p.emit_beat;
      if (out.hif_rdata_end) {
        p.phase = Phase::Done;
        ready_reads.pop_front();
      }
    }
    if (out.hif_wdata_ptr_valid) {
      auto &p = get(*pointer_output);
      p.pointer_notified = true;
      p.data_eligible_cycle = now + 1;
      awaiting_data.push_back(p.id);
    }
    assemble_write();
    if (i.hif_wdata_valid && !out.hif_wdata_stall) {
      auto &p = get(awaiting_data.front());
      write_fifo.push_back(
          {p.id, p.received_beats * 32, i.hif_wdata, i.hif_wdata_mask});
      ++p.received_beats;
      if (i.hif_wdata_end)
        awaiting_data.pop_front();
    }
    if (i.hif_cmd_valid && !out.hif_cmd_stall)
      accept_command(i);
    if (i.hif_cmd_valid && out.hif_cmd_stall)
      ++statistics.command_stalls;
    if (i.hif_wdata_valid && out.hif_wdata_stall)
      ++statistics.write_data_stalls;
    stalled_command = i.hif_cmd_valid && out.hif_cmd_stall
                          ? std::optional<Inputs>(i)
                          : std::nullopt;
    stalled_data = i.hif_wdata_valid && out.hif_wdata_stall
                       ? std::optional<Inputs>(i)
                       : std::nullopt;
    advance_parents();
    time_remainder += config.hif_period_ps;
    const long double quantum = memory->tick_period_ps();
    while (time_remainder + quantum * 1e-12L >= quantum) {
      submit_one();
      memory->tick();
      ++statistics.backend_ticks;
      consume_events();
      advance_parents();
      time_remainder -= quantum;
    }
    if (time_remainder < 0)
      time_remainder = 0;
    retire_done();
    ++now;
    prepare_outputs();
  }
};
Adapter::Adapter(Config c, std::unique_ptr<Backend> b)
    : impl_(std::make_unique<Impl>(c, std::move(b))) {}
Adapter::~Adapter() = default;
Outputs Adapter::evaluate(const Inputs &i) const { return impl_->evaluate(i); }
void Adapter::tick(const Inputs &i) {
  try {
    impl_->tick(i);
  } catch (...) {
    impl_->poisoned = true;
    throw;
  }
}
bool Adapter::idle() const { return impl_->idle(); }
void Adapter::finish() {
  if (!idle())
    throw std::logic_error("finish requires drained HIF and backend");
  impl_->memory->finish();
}
std::uint64_t Adapter::cycle() const { return impl_->now; }
const Statistics &Adapter::stats() const { return impl_->statistics; }
const Backend &Adapter::backend() const { return *impl_->memory; }
} // namespace hif2mc
