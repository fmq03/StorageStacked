#include "../examples/hif_master.hpp"
#include "hbm_sim/config/model.hpp"
#include "hif2mc/memsim_backend.hpp"
#include <functional>
#include <iostream>
#include <map>
#include <string>

using namespace hif2mc;
using hif2mc::example::Master;
using hif2mc::example::pattern;
namespace {
void check(bool yes, const std::string &name) {
  if (!yes)
    throw std::runtime_error(name);
}
template <class F> void rejects(F f, const std::string &name) {
  bool failed = false;
  try {
    f();
  } catch (const std::exception &) {
    failed = true;
  }
  check(failed, name);
}
// Independent byte-addressed timing backend. Events have deliberately separate
// issue/completion times, and the first child may be accepted before the next
// is rejected. This is not a copy of Adapter's state machine.
class TimedMemory final : public Backend {
public:
  struct Job {
    BackendRequest r;
    std::uint64_t issue, complete;
    bool issued = false, done = false;
  };
  struct Host {
    std::uint32_t count = 0, done = 0;
    bool write = false;
    Bytes data, init;
    Status status = Status::Ok;
  };
  std::uint64_t time = 0, reject_until = 0;
  std::uint64_t latency = 50;
  bool alternating_accept = false, suppress_issue = false, reorder = false;
  double period = 250;
  std::vector<BackendRequest> accepted;
  std::vector<Job> jobs;
  std::map<std::uint64_t, Host> hosts;
  std::map<std::uint64_t, Status> faults;
  std::deque<BackendEvent> events;
  Bytes bytes = Bytes(8192), initialized = Bytes(8192);
  bool try_submit(const BackendRequest &r) override {
    if (time < reject_until || (alternating_accept && time % 2))
      return false;
    for (const auto &old : accepted)
      check(old.id != r.id, "backend child must not be submitted twice");
    accepted.push_back(r);
    auto &h = hosts[r.host_id];
    h.count = r.count;
    h.write = r.write;
    if (h.data.empty()) {
      h.data.resize(r.count * 32);
      h.init.resize(r.count * 32);
    }
    const auto delay = latency + (reorder && r.address < 64 ? 100 : 0);
    jobs.push_back({r, time + 2, time + delay, false, false});
    return true;
  }
  void tick() override {
    ++time;
    for (auto &j : jobs) {
      if (!j.issued && time >= j.issue) {
        j.issued = true;
        if (!suppress_issue)
          events.push_back({BackendEvent::Kind::Issued,
                            j.r.host_id,
                            j.r.index,
                            j.r.write,
                            Status::Ok,
                            {},
                            {}});
      }
      if (j.done || time < j.complete)
        continue;
      j.done = true;
      auto &h = hosts.at(j.r.host_id);
      for (std::size_t n = 0; n < j.r.bytes; ++n) {
        const auto a = static_cast<std::size_t>(j.r.address) + n;
        if (j.r.write) {
          if (j.r.mask[n]) {
            bytes[a] = j.r.data[n];
            initialized[a] = 0xff;
          }
        } else {
          h.data[32 * j.r.index + n] = bytes[a];
          h.init[32 * j.r.index + n] = initialized[a];
          if (!initialized[a] && h.status == Status::Ok)
            h.status = Status::Uninitialized;
        }
      }
      if (auto fault = faults.find(j.r.address); fault != faults.end())
        h.status = fault->second;
      ++h.done;
    }
    for (auto it = hosts.begin(); it != hosts.end();) {
      if (it->second.done != it->second.count) {
        ++it;
        continue;
      }
      auto &h = it->second;
      events.push_back({BackendEvent::Kind::Completed, it->first, 0, h.write,
                        h.status, h.data, h.init});
      it = hosts.erase(it);
    }
  }
  std::optional<BackendEvent> pop_event() override {
    if (events.empty())
      return {};
    auto e = std::move(events.front());
    events.pop_front();
    return e;
  }
  std::size_t transaction_bytes() const override { return 32; }
  double tick_period_ps() const override { return period; }
  std::uint64_t capacity_bytes() const override { return bytes.size(); }
  bool idle() const override { return hosts.empty() && events.empty(); }
  void finish() override { check(idle(), "fake backend not drained"); }
};
Config small() {
  auto c = Config::reference_64b();
  c.lpr_credits = 2;
  c.hpr_credits = 1;
  c.write_credits = 2;
  c.max_inflight = 6;
  return c;
}
void test_missing_and_unsupported() {
  rejects([] { Adapter a(Config{}, std::make_unique<TimedMemory>()); },
          "missing contract accepted");
  for (int mode = 0; mode < 7; ++mode) {
    auto c = small();
    c.enable_rmw = false;
    Adapter a(c, std::make_unique<TimedMemory>());
    Inputs in;
    in.hif_cmd_valid = true;
    in.hif_cmd_type = 1;
    if (mode == 0)
      in.hif_cmd_type = 3;
    if (mode == 1)
      in.hif_cmd_type = 2;
    if (mode == 2)
      in.hif_cmd_length = 1;
    if (mode == 3)
      in.hif_cmd_autopre = true;
    if (mode == 4)
      in.hif_cmd_pri = 2;
    if (mode == 5)
      in.hif_go2critical_wr = true;
    if (mode == 6)
      in.hif_cmd_token = std::uint64_t(1) << 53;
    rejects([&] { a.tick(in); }, "unsupported command accepted");
    check(a.evaluate({}).hif_cmd_stall, "failed adapter must stop");
  }
  auto c = small();
  c.pointer_delay_cycles = 1;
  rejects([&] { Adapter a(c, std::make_unique<TimedMemory>()); },
          "pointer delay below WPS bound accepted");
}
void test_write_read_rmw_timing() {
  auto c = small();
  auto b = std::make_unique<TimedMemory>();
  auto *mem = b.get();
  Adapter a(c, std::move(b));
  Master m(a, c);
  auto old = pattern(64, 11), update = pattern(64, 81);
  Bytes mask(64);
  for (std::size_t i = 0; i < 64; ++i)
    mask[i] = i % 4 == 1;
  const auto token = (std::uint64_t(1) << 52) | 7;
  m.write(0, 22, old, Bytes(64, 1));
  m.read(0, token, true);
  bool early_write = false;
  for (unsigned n = 0; n < 1000; ++n) {
    if (m.commands.empty() && m.sending.empty() && a.idle())
      break;
    m.step();
    if (m.samples.back().out.hif_wr_credit && a.stats().completed_writes == 0)
      early_write = true;
  }
  check(a.idle(), "roundtrip drain");
  check(m.reads[token] == old, "read passed older write awaiting data");
  check(early_write, "issue credit must precede write completion");
  check(m.read_ends[token] == 1, "exactly one end per read");
  std::uint64_t notified = 0, first_data = 0;
  bool found_data = false;
  for (const auto &s : m.samples) {
    if (s.out.hif_wdata_ptr_valid) {
      check(s.out.hif_wdata_ptr == 22, "pointer identity");
      notified = s.cycle;
    }
    if (!found_data && s.in.hif_wdata_valid && !s.out.hif_wdata_stall) {
      first_data = s.cycle;
      found_data = true;
    }
  }
  check(notified - m.accepted_at[22] >= 2, "pointer minimum delay");
  check(first_data >= notified + 1, "write data starts after pointer");
  std::uint64_t high_credit = 0, high_data = 0;
  for (const auto &s : m.samples) {
    if (s.out.hif_hpr_credit)
      high_credit = s.cycle;
    if (s.out.hif_rdata_valid && s.out.hif_rdata_token == token && !high_data)
      high_data = s.cycle;
  }
  check(high_credit < high_data,
        "HPR resource release incorrectly waits for read data");
  const auto rmw_start = m.samples.size();
  m.write(0, 21, update, mask, true);
  m.read(0, token + 1);
  m.run();
  auto expected = old;
  for (std::size_t i = 0; i < 64; ++i)
    if (mask[i])
      expected[i] = update[i];
  check(m.reads[token + 1] == expected, "RMW merge or overlap protection");
  check(a.stats().accepted_rmws == 1 && a.stats().completed_rmws == 1,
        "RMW accounting");
  check(a.stats().returned_credits == std::array<std::uint64_t, 3>{2, 1, 2},
        "RMW consumes/returns LPR and WR once");
  check(m.reads.size() == 2, "internal RMW read leaked onto HIF");
  check(mem->accepted.size() == 10,
        "RMW must really issue reads and writes (not a masked write alias)");
  std::uint64_t rmw_lpr = 0, rmw_wr = 0;
  for (std::size_t n = rmw_start; n < m.samples.size(); ++n) {
    const auto &s = m.samples[n];
    if (s.out.hif_lpr_credit && !rmw_lpr)
      rmw_lpr = s.cycle;
    if (s.out.hif_wr_credit)
      rmw_wr = s.cycle;
  }
  check(rmw_lpr && rmw_wr > rmw_lpr,
        "RMW read/write resources must release independently");
  a.finish();
}
void test_stalls_and_retries() {
  auto c = small();
  c.max_inflight = 2;
  c.write_data_fifo_beats = 1;
  c.write_assembly_interval = 4;
  auto b = std::make_unique<TimedMemory>();
  b->alternating_accept = true;
  b->reject_until = 5;
  auto *mem = b.get();
  Adapter a(c, std::move(b));
  Master m(a, c);
  for (unsigned n = 0; n < 5; ++n) {
    m.write(64 * n, 100 + n, pattern(64, n), Bytes(64, 1));
    m.read(64 * n, 200 + n);
  }
  m.run();
  check(a.stats().command_stalls > 0,
        "context pressure did not cause command stall");
  check(a.stats().write_data_stalls > 0,
        "assembly FIFO did not cause write stall");
  check(a.stats().submission_retries > 0, "backend refusal was not retried");
  check(mem->accepted.size() == 20,
        "lost/duplicate children after partial submission");
  check(std::any_of(m.samples.begin(), m.samples.end(),
                    [](const auto &s) {
                      return s.in.hif_wdata_valid && s.in.hif_wdata_end &&
                             s.out.hif_wdata_stall;
                    }),
        "last write beat under stall not covered");
  for (unsigned n = 0; n < 5; ++n)
    check(m.reads[200 + n] == pattern(64, n), "stalled data changed");
}
void test_input_stability_and_end() {
  {
    auto c = small();
    c.max_inflight = 2;
    auto b = std::make_unique<TimedMemory>();
    b->latency = 500;
    Adapter a(c, std::move(b));
    Inputs in;
    in.hif_cmd_valid = true;
    in.hif_cmd_type = 1;
    a.tick(in);
    in.hif_cmd_token = 1;
    a.tick(in);
    in.hif_cmd_token = 2;
    check(a.evaluate(in).hif_cmd_stall, "expected command stall");
    a.tick(in);
    ++in.hif_cmd_addr;
    rejects([&] { a.tick(in); }, "changed stalled command accepted");
  }
  {
    Adapter a(small(), std::make_unique<TimedMemory>());
    Inputs in;
    in.hif_cmd_valid = true;
    a.tick(in);
    in = {};
    for (unsigned n = 0; n < 10 && !a.evaluate(in).hif_wdata_ptr_valid; ++n)
      a.tick(in);
    check(a.evaluate(in).hif_wdata_ptr_valid, "missing pointer");
    a.tick(in);
    in.hif_wdata_valid = true;
    in.hif_wdata_end = true;
    check(!a.evaluate(in).hif_wdata_stall,
          "first data must be ready after one cycle");
    rejects([&] { a.tick(in); }, "early write end accepted");
  }
  {
    Adapter a(small(), std::make_unique<TimedMemory>());
    Inputs in;
    in.hif_wdata_valid = true;
    a.tick(in);
    in.hif_wdata_mask = 1;
    rejects([&] { a.tick(in); }, "changed stalled data accepted");
  }
}
void test_out_of_order_and_credit_fallback() {
  auto c = small();
  auto b = std::make_unique<TimedMemory>();
  b->reorder = true;
  b->suppress_issue = true;
  Adapter a(c, std::move(b));
  Master m(a, c);
  m.read(0, 1);
  m.read(64, 2);
  m.run();
  std::vector<std::uint64_t> order;
  for (const auto &s : m.samples)
    if (s.out.hif_rdata_valid && s.out.hif_rdata_end)
      order.push_back(s.out.hif_rdata_token);
  check(order == std::vector<std::uint64_t>{2, 1},
        "responses should follow backend completion order");
  check(a.stats().credit_fallbacks == 2,
        "missing issue events need explicit completion fallback accounting");
  check(m.credits == std::array<std::size_t, 3>{2, 1, 2},
        "credits not restored");
}
void test_errors_and_reset() {
  auto c = small();
  auto b = std::make_unique<TimedMemory>();
  auto *mem = b.get();
  Adapter a(c, std::move(b));
  Master m(a, c);
  m.write(8192, 9, Bytes(64, 1), Bytes(64, 1));
  m.read(8192, 19);
  m.run();
  check(mem->accepted.empty(), "invalid addresses reached backend");
  bool re = false, we = false;
  for (const auto &s : m.samples) {
    re |= s.out.hif_rdata_valid && s.out.hif_rdata_addr_err;
    we |= s.out.hif_wdata_ptr_valid && s.out.hif_wdata_ptr_addr_err;
  }
  check(re && we && a.stats().address_errors == 2,
        "HIF address error channels");
  m.write(0, 10, pattern(64, 7), Bytes(64, 1));
  m.run();
  Inputs reset;
  reset.core_ddrc_rstn = false;
  check(a.evaluate(reset).hif_cmd_stall, "reset must stall");
  a.tick(reset);
  m.read(0, 20);
  m.run();
  check(m.reads[20] == pattern(64, 7), "idle reset erased memory");
  mem->faults[0] = Status::EccUncorrectable;
  m.read(0, 21);
  m.run();
  bool ecc = false;
  for (const auto &s : m.samples)
    if (s.out.hif_rdata_token == 21)
      ecc |= s.out.hif_rdata_uncorr_ecc_err;
  check(ecc, "ECC error mapping");
  for (const auto &s : m.samples)
    check(!s.out.hif_rdata_crc_err && !s.out.hif_wrecc_credit,
          "invented CRC/inline ECC signal");
  {
    Adapter hot(c, std::make_unique<TimedMemory>());
    Inputs i;
    i.hif_cmd_valid = true;
    i.hif_cmd_type = 1;
    hot.tick(i);
    rejects([&] { hot.tick(reset); }, "hot reset accepted");
  }
}
void test_clock_and_evaluate() {
  auto c = small();
  auto b = std::make_unique<TimedMemory>();
  auto *mem = b.get();
  b->period = 312.5;
  Adapter a(c, std::move(b));
  for (unsigned n = 0; n < 100; ++n)
    (void)a.evaluate({});
  check(mem->time == 0 && a.cycle() == 0, "evaluate advanced time");
  for (unsigned n = 0; n < 5; ++n)
    a.tick({});
  check(mem->time == 32, "fractional clock conversion drift");
}
void test_credit_reuse_and_completion_policy() {
  {
    auto c = small();
    c.lpr_credits = 1;
    Adapter a(c, std::make_unique<TimedMemory>());
    Master m(a, c);
    m.read(0, 1);
    m.read(64, 2);
    m.read(128, 3);
    m.run();
    check(std::any_of(m.samples.begin(), m.samples.end(),
                      [](const auto &s) {
                        return s.out.hif_lpr_credit && s.in.hif_cmd_valid &&
                               !s.out.hif_cmd_stall;
                      }),
          "same-edge return and acceptance not supported");
    check(a.stats().returned_credits[0] == 3, "same-edge credit conservation");
  }
  {
    auto c = small();
    c.credit_policy = CreditPolicy::BackendCompleted;
    Adapter a(c, std::make_unique<TimedMemory>());
    Master m(a, c);
    m.write(0, 1, pattern(64, 1), Bytes(64, 1));
    bool credit_seen = false;
    for (unsigned n = 0; n < 200; ++n) {
      if (m.commands.empty() && m.sending.empty() && a.idle())
        break;
      m.step();
      if (m.samples.back().out.hif_wr_credit) {
        credit_seen = true;
        check(a.stats().completed_writes == 1,
              "completion policy returned write credit too early");
      }
    }
    check(credit_seen && a.idle(), "completion credit policy did not drain");
  }
}
void test_rmw_uninitialized_and_unrepresentable_error() {
  {
    Adapter a(small(), std::make_unique<TimedMemory>());
    Master m(a, small());
    m.write(0, 1, Bytes(64, 5), Bytes(64, 0), true);
    rejects([&] { m.run(); },
            "RMW silently initialized preserved unknown data");
  }
  {
    auto b = std::make_unique<TimedMemory>();
    b->faults[0] = Status::DataMismatch;
    Adapter a(small(), std::move(b));
    Master m(a, small());
    m.read(0, 1);
    rejects([&] { m.run(); }, "data mismatch masqueraded as CRC");
  }
}
void test_real_memsim(bool behavioral, int stacks) {
  auto c = Config::reference_64b();
  auto spec = hbm_sim::config::build_model("hbm4", {{"channels", "1"}});
  hbm_sim::MemorySystemOptions options;
  options.stack_count = stacks;
  options.stack_ingress_buffer_size = 1;
  options.stack_dispatch_width = 1;
  options.controller.read_buffer_size = 1;
  options.controller.write_buffer_size = 1;
  options.controller.phy.mode = behavioral ? hbm_sim::MemPhyMode::Behavioral
                                           : hbm_sim::MemPhyMode::Direct;
  options.controller.phy.reset_cycles = 4;
  options.controller.phy.initialization_cycles = 4;
  options.controller.phy.training_cycles = 8;
  options.host_response_queue_capacity = 2;
  auto b = std::make_unique<MemSimBackend>(spec, options);
  auto *mem = b.get();
  Adapter a(c, std::move(b));
  Master m(a, c);
  const auto token = (std::uint64_t(1) << 52) | 55;
  auto original = pattern(64, 3), update = pattern(64, 99);
  Bytes mask(64);
  for (std::size_t n = 0; n < 64; ++n)
    mask[n] = n % 2;
  m.write(0x1000, 22, original, Bytes(64, 1));
  m.read(0x1000, token, true);
  m.run();
  check(m.reads[token] == original,
        "real mem_sim byte order or read-after-write");
  m.write(0x1000, 23, update, mask);
  m.read(0x1000, token + 1);
  m.run();
  auto expected = original;
  for (std::size_t n = 0; n < 64; ++n)
    if (mask[n])
      expected[n] = update[n];
  check(m.reads[token + 1] == expected, "real mem_sim mask update");
  Bytes rmw_mask(64);
  for (std::size_t n = 0; n < 64; ++n)
    rmw_mask[n] = n % 3 == 0;
  m.write(0x1000, 24, original, rmw_mask, true);
  m.read(0x1000, token + 2);
  m.run();
  for (std::size_t n = 0; n < 64; ++n)
    if (rmw_mask[n])
      expected[n] = original[n];
  check(m.reads[token + 2] == expected, "real mem_sim RMW");
  if (stacks == 2) {
    m.write(0x1100, 25, update, Bytes(64, 1));
    m.read(0x1100, token + 3);
    m.run();
    check(m.reads[token + 3] == update, "multi-stack routing");
  }
  for (unsigned n = 0; n < 20; ++n)
    m.read(0x2000 + 64 * n, 500 + n);
  m.run();
  a.finish();
  check(a.stats().submission_retries > 0,
        "real backend backpressure not exercised");
  check(a.stats().credit_fallbacks == 0,
        "issued events missing for non-forwarded requests");
  const auto &stats = mem->memory_system().stats();
  if (behavioral)
    check(stats.phy_read_completions > 0 && stats.phy_write_completions > 0,
          "PHY not exercised");
  else
    check(stats.phy_read_completions == 0,
          "Direct unexpectedly used behavioral PHY");
  check(stats.completed_reads >= 46 && stats.completed_writes >= 6,
        "backend completion counts");
}
} // namespace
int main(int argc, char **argv) {
  try {
    const std::string group = argc > 1 ? argv[1] : "all";
    unsigned passed = 0;
    auto run = [&](const char *name, auto f) {
      f();
      ++passed;
      std::cout << "[PASS] " << name << '\n';
    };
    if (group == "contract" || group == "all") {
      run("explicit contracts and unsupported inputs",
          test_missing_and_unsupported);
      run("pointer timing, overlap and real RMW stages",
          test_write_read_rmw_timing);
      run("natural stalls and partial-submit retries", test_stalls_and_retries);
      run("stalled input stability and end", test_input_stability_and_end);
      run("out-of-order read and explicit fallback",
          test_out_of_order_and_credit_fallback);
      run("address/ECC errors and reset", test_errors_and_reset);
      run("clock accumulator and pure evaluation", test_clock_and_evaluate);
      run("same-edge credit reuse and explicit completion policy",
          test_credit_reuse_and_completion_policy);
      run("unrepresentable errors fail explicitly",
          test_rmw_uninitialized_and_unrepresentable_error);
    }
    if (group == "memsim" || group == "all") {
      run("mem_sim direct", [] { test_real_memsim(false, 1); });
      run("mem_sim behavioral PHY", [] { test_real_memsim(true, 1); });
      run("mem_sim multi-stack behavioral PHY",
          [] { test_real_memsim(true, 2); });
    }
    check(passed > 0, "unknown test group");
    std::cout << passed << " scenarios passed\n";
  } catch (const std::exception &e) {
    std::cerr << "[FAIL] " << e.what() << '\n';
    return 1;
  }
}
