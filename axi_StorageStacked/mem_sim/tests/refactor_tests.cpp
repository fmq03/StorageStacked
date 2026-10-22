#include "hbm_sim/config/model.hpp"
#include "hbm_sim/config/parse.hpp"
#include "hbm_sim/config/fields.hpp"
#include "hbm_sim/controller/request_queues.hpp"
#include "hbm_sim/core/system.hpp"
#include "hbm_sim/frontend/traffic.hpp"
#include <iostream>
#include <stdexcept>
#include <set>

using namespace hbm_sim;

namespace {
void require(bool ok, const char* message) {
  if (!ok) throw std::runtime_error(message);
}
template<class F> void rejects(F fn) {
  try { fn(); } catch (const std::exception&) { return; }
  throw std::runtime_error("invalid input accepted");
}
struct Source : RequestSource {
  std::vector<Request> requests;
  std::size_t index = 0;
  explicit Source(std::vector<Request> input) : requests(std::move(input)) {}
  bool next(Request& request) override {
    if (index == requests.size()) return false;
    request = requests[index++];
    return true;
  }
  std::optional<std::uint64_t> remaining_hint() const override {
    return requests.size() - index;
  }
};

void scalar_parsing() {
  using namespace hbm_sim::config;
  require(parse_int("-12") == -12 && parse_u64("18446744073709551615") == UINT64_MAX,
          "integer parsing changed");
  require(parse_double("1.25e2") == 125 && parse_bool("YeS") && !parse_bool("OFF"),
          "scalar aliases changed");
  require(lower_value("Open-Page") == "open_page", "name normalization changed");
  for (const char* bad : {"", " 1", "1 ", "1x", "1\n"}) {
    rejects([&] { parse_int(bad); });
    rejects([&] { parse_u64(bad); });
    rejects([&] { parse_double(bad); });
  }
  for (const char* bad : {"nan", "inf", "-inf", "1e999"})
    rejects([&] { parse_double(bad); });
  rejects([] { parse_u64("-1"); });
  rejects([] { parse_int("2147483648"); });
  rejects([] { parse_bool("truthy"); });
}

void field_registry() {
  auto spec = config::build_model("hbm4");
  std::set<std::string_view> keys;
  for (const auto& field : config::model_fields()) {
    const auto original = field.get(spec);
    const auto text = config::model_field_text(spec, field.keys.front());
    for (auto key : field.keys) {
      require(keys.insert(key).second, "duplicate scalar alias");
      require(config::is_spec_override_key(std::string(key)), "scalar missing whitelist");
      require(config::assign_model_field(spec, std::string(key), text), "scalar assignment failed");
      require(field.get(spec) == original, "scalar roundtrip changed value");
    }
  }
  spec.timing.tCK_ps = 500;
  for (const auto& field : config::timing_fields()) {
    for (auto key : field.keys) {
      require(keys.insert(key).second, "duplicate timing alias");
      require(config::is_timing_override_key(std::string(key)), "timing missing whitelist");
      require(config::canonical_timing_name_for_key(std::string(key)) == field.name,
              "timing source target differs from assignment");
      std::string value = key.ends_with("_us") ? "0.0035" :
                          key.ends_with("_ns") ? "3.5" : "7";
      require(config::assign_timing_field(spec, std::string(key), value), "timing assignment failed");
      require(spec.timing.*(field.member) == 7, "timing unit conversion changed");
    }
  }
  config::assign_timing_field(spec, "twtp_ns", "3.1");
  require(spec.timing.nWR == 7, "tWTP alias or ceiling incorrect");
  config::assign_timing_field(spec, "trefidb_ns", "6");
  require(spec.timing.nREFIpb == 12, "REFdb interval alias incorrect");
  config::assign_timing_field(spec, "taad_ns", "4");
  require(spec.timing.nAADMax == 8, "tAAD must select deadline");
  require(!config::is_spec_override_key("ncl_typo"), "unknown key accepted");
  rejects([&] { config::assign_timing_field(spec, "ncl", "1.5"); });
  rejects([] { config::build_model("hbm4", {{"ncl", "7"}, {"timing_source", "bad"}}); });
  rejects([] { config::build_model("hbm4", {{"nwr", "7"}, {"twtp_ns", "3.5"}}); });
  for (int schema : {2, 3}) {
    auto first = config::build_model("hbm4", {{"timing_source", "vendor"}, {"ncl", "60"}}, schema);
    auto last = config::build_model("hbm4", {{"ncl", "60"}, {"timing_source", "vendor"}}, schema);
    require(first.timing.nCL == 60 && last.timing.nCL == 60, "timing override lost");
    require(first.timing_table.source_count(TimingValueSource::Vendor) ==
            last.timing_table.source_count(TimingValueSource::Vendor),
            "timing source depends on input order");
  }
}

void queue_lifecycle() {
  auto spec = config::build_model("hbm4");
  RequestQueues queues(1, 1, 1, 1);
  Request request;
  request.id = 42;
  queues[RequestQueueKind::Read].push_back(request);
  require(!queues.has_space(RequestQueueKind::Read), "read capacity not enforced");
  require(queues.promote(RequestQueueKind::Read, 0, spec), "promotion rejected");
  require(queues.size() == 1 && queues.active_counts()[0] == 1 &&
          queues[RequestQueueKind::Active].front().id == 42, "promotion lost ownership or identity");
  queues[RequestQueueKind::Write].push_back(request);
  require(!queues.promote(RequestQueueKind::Write, 0, spec), "active capacity ignored");
  require(!queues.promote(RequestQueueKind::Active, 0, spec), "active re-promotion accepted");
  queues.erase(RequestQueueKind::Active, 0, spec);
  require(queues.active_counts()[0] == 0 && queues.size() == 1, "active retirement leaked ownership");
  require(queues.promote(RequestQueueKind::Write, 0, spec), "freed active capacity unavailable");
  queues.erase(RequestQueueKind::Active, 0, spec);
  queues.erase(RequestQueueKind::Read, 99, spec);
  require(queues.size() == 0, "empty queue erase changed state");
  rejects([] { RequestQueues invalid(0, 1, 1, 1); });
}

void streaming_driver() {
  for (const char* standard : {"hbm3", "hbm4", "lpddr5", "lpddr6"}) {
    const auto spec = config::build_model(standard, {{"channels", "1"},
        {"supports_refresh", "false"}, {"supports_rfm", "false"}});
    TrafficOptions traffic;
    traffic.requests = 12;
    traffic.pattern = "stream";
    traffic.read_ratio = 50;
    auto requests = generate_traffic(spec, traffic);
    Request maintenance;
    maintenance.type = RequestType::Maintenance;
    maintenance.next = Command::MRW;
    requests.insert(requests.begin(), maintenance);
    for (auto mode : {ResponseDeliveryMode::HostOnly, ResponseDeliveryMode::TransactionOnly,
                      ResponseDeliveryMode::Both}) {
      for (auto phy : {MemPhyMode::Direct, MemPhyMode::Behavioral}) {
        MemorySystemOptions options;
        options.response_delivery_mode = mode;
        options.host_response_queue_capacity = 1;
        options.transaction_response_queue_capacity = 1;
        options.stack_ingress_buffer_size = 1;
        options.controller.read_buffer_size = 2;
        options.controller.write_buffer_size = 2;
        options.controller.phy.mode = phy;
        options.controller.retain_command_trace = true;
        MemorySystem manual(spec, options), driven(spec, options);
        std::vector<std::uint64_t> manual_hosts, driven_hosts, manual_transactions, driven_transactions;
        std::vector<RunProgress> progress;
        auto consume = [&](MemorySystem& memory, auto& hosts, auto& transactions) {
          while (memory.has_response()) hosts.push_back(memory.pop_response().host_request_id);
          while (memory.has_transaction_response())
            transactions.push_back(memory.pop_transaction_response().host_request_id);
        };
        std::size_t next = 0;
        while ((next < requests.size() || !manual.quiescent()) && manual.clock() < 10000) {
          while (next < requests.size() && requests[next].inject_cycle <= manual.clock()) {
            const auto& req = requests[next];
            const bool accepted = req.type == RequestType::Maintenance
                ? manual.try_submit_maintenance(req) : manual.try_submit(req);
            if (!accepted) break;
            ++next;
          }
          manual.step();
          consume(manual, manual_hosts, manual_transactions);
        }
        manual.finish(requests.size() - next);
        Source source(requests);
        MemorySystem::RunOptions run;
        run.drain_responses = true;
        run.progress_interval = 3;
        run.progress = [&](const RunProgress& p) { progress.push_back(p); };
        if (mode != ResponseDeliveryMode::TransactionOnly)
          run.host_response = [&](const HostResponse& r) { driven_hosts.push_back(r.host_request_id); };
        if (mode != ResponseDeliveryMode::HostOnly)
          run.transaction_response = [&](const TransactionResponse& r) {
            driven_transactions.push_back(r.host_request_id);
          };
        driven.run(source, 10000, run);
        require(manual.quiescent() && driven.quiescent(), "driver did not drain");
        require(manual_hosts == driven_hosts && manual_transactions == driven_transactions,
                "driver response order changed");
        require(driven.clock() == manual.clock(), "driver added simulation cycles");
        require(driven.stats().injection_stall_cycles == manual.stats().injection_stall_cycles &&
                driven.stats().injection_stall_cycles > 0, "driver backpressure counting changed");
        require(driven.stats().completed_reads == manual.stats().completed_reads &&
                driven.stats().completed_writes == manual.stats().completed_writes,
                "driver completion changed");
        const auto& a = manual.issued_commands();
        const auto& b = driven.issued_commands();
        require(a.size() == b.size(), "driver command count changed");
        for (std::size_t i = 0; i < a.size(); ++i)
          require(a[i].cycle == b[i].cycle && a[i].command == b[i].command &&
                  a[i].request_id == b[i].request_id, "driver command order changed");
        require(!progress.empty() && progress.front().cycle == 3, "progress callback missing");
        for (std::size_t i = 1; i < progress.size(); ++i)
          require(progress[i].cycle == progress[i - 1].cycle + 3, "progress cadence changed");

        // Early limits preserve unsubmitted work; no extra tick is allowed.
        MemorySystem truncated(spec, options);
        Source deferred(requests);
        MemorySystem::RunOptions drain;
        drain.drain_responses = true;
        truncated.run(deferred, 0, drain);
        require(truncated.clock() == 0 && truncated.stats().remaining_requests == requests.size(),
                "zero cycle limit lost source work");
        truncated.run(deferred, 2, drain);
        require(truncated.clock() == 2 && truncated.stats().hit_cycle_limit,
                "early limit was not reported");
      }
    }
    MemorySystem empty(spec);
    Source no_requests({});
    empty.run(no_requests, 10);
    require(empty.idle() && empty.stats().remaining_requests == 0, "empty source failed");
    MemorySystem unordered(spec);
    Source bad(requests);
    bad.requests[0].inject_cycle = 1;
    bad.requests[1].inject_cycle = 0;
    rejects([&] { unordered.run(bad, 10); });
  }
}

void driver_consumption_contract() {
  const auto spec = config::build_model("hbm4", {{"channels", "1"},
      {"supports_refresh", "false"}, {"supports_rfm", "false"}});
  TrafficOptions traffic;
  traffic.requests = 8;
  auto requests = generate_traffic(spec, traffic);
  MemorySystemOptions options;
  options.response_delivery_mode = ResponseDeliveryMode::Both;
  options.host_response_queue_capacity = 1;
  options.transaction_response_queue_capacity = 1;
  MemorySystem memory(spec, options);
  Source source(requests);
  MemorySystem::RunOptions run;
  std::size_t hosts = 0;
  run.host_response = [&](const HostResponse&) { ++hosts; };
  // Callback presence enables drain; a selected view without a callback is
  // still consumed, so a one-entry transaction queue cannot stall the host.
  memory.run(source, 10000, run);
  require(hosts > 0 && memory.quiescent() && !memory.stats().hit_cycle_limit,
          "unobserved selected view was not drained");

  options.response_delivery_mode = ResponseDeliveryMode::HostOnly;
  MemorySystem mismatch(spec, options);
  Source empty({});
  run.transaction_response = [](const TransactionResponse&) {};
  rejects([&] { mismatch.run(empty, 1, run); });
  require(mismatch.clock() == 0, "invalid callback advanced simulation");
  options.response_delivery_mode = ResponseDeliveryMode::Disabled;
  MemorySystem disabled(spec, options);
  run = {};
  run.drain_responses = true;
  rejects([&] { disabled.run(empty, 1, run); });

  options.response_delivery_mode = ResponseDeliveryMode::Both;
  MemorySystem expired(spec, options);
  for (const auto& request : requests)
    require(expired.try_submit(request), "test request admission failed");
  for (int cycle = 0; cycle < 1000 && !expired.idle(); ++cycle) expired.step();
  require(expired.idle() && !expired.responses_drained(), "test did not retain completions");
  const Cycle stopped = expired.clock();
  expired.run(empty, stopped, run);
  require(expired.clock() == stopped && expired.quiescent(),
          "expired cycle limit failed to drain completed responses");
}
}

int main() {
  try {
    scalar_parsing();
    field_registry();
    queue_lifecycle();
    streaming_driver();
    driver_consumption_contract();
    std::cout << "refactor contract tests passed\n";
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
