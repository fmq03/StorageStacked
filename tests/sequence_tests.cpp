#include "spec_fixture.hpp"
#include "hbm_sim/config/model.hpp"
// 轻量序列测试入口。项目保持零外部测试依赖，因此这里用 require()
// 直接断言关键命令序列、timing 间隔、维护路径和 validator 行为。
// 测试目标不是覆盖性能，而是守住协议状态机和 Ramulator 风格模块边界。
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "hbm_sim/controller/controller.hpp"
#include "hbm_sim/controller/executor.hpp"
#include "hbm_sim/controller/refresh.hpp"
#include "hbm_sim/controller/row_policy.hpp"
#include "hbm_sim/controller/timing.hpp"
#include "hbm_sim/core/addr_map.hpp"
#include "hbm_sim/core/stack_model.hpp"
#include "hbm_sim/core/system.hpp"
#include "hbm_sim/dram/interface.hpp"
#include "hbm_sim/dram/jedec.hpp"
#include "hbm_sim/dram/profiles.hpp"
#include "hbm_sim/dram/semantics.hpp"
#include "hbm_sim/dram/spec.hpp"
#include "hbm_sim/dram/state.hpp"
#include "hbm_sim/frontend/traffic.hpp"
#include "hbm_sim/validation/dfi.hpp"
#include "hbm_sim/validation/trace.hpp"
#include "hbm_sim/validation/validator.hpp"

namespace {

using hbm_sim::Command;
using hbm_sim::Controller;
using hbm_sim::Cycle;
using hbm_sim::DecodedAddress;
using hbm_sim::DramSpec;
using hbm_sim::IssuedCommand;
using hbm_sim::MemorySystem;
using hbm_sim::Request;
using hbm_sim::RequestType;

void require(bool condition, const std::string &message) {
  if (!condition) {
    // 不引入测试框架，保持项目无外部依赖。失败信息直接说明协议断言。
    std::cerr << "sequence test failed: " << message << '\n';
    std::exit(1);
  }
}

Request make_request(std::uint64_t id, RequestType type, int pseudo_channel,
                     int bank_group, int bank, int row) {
  // 这里手写 decoded 坐标，而不是通过 AddressMapper 反推地址，是为了让测试
  // 精确控制 pseudo-channel/bank-group/bank/row，从而稳定触发目标 timing。
  Request req;
  req.id = id;
  req.address = id * 64;
  req.type = type;
  req.decoded.channel = 0;
  req.decoded.pseudo_channel = pseudo_channel;
  req.decoded.rank = 0;
  req.decoded.bank_group = bank_group;
  req.decoded.bank = bank;
  req.decoded.row = row;
  req.decoded.column = 0;
  return req;
}

bool same_rank_scope(const IssuedCommand &a, const IssuedCommand &b) {
  // HBM4 的 activation_scope 当前是 Rank，因此这里检查同一 channel、
  // pseudo-channel 和 rank 内的 ACT 间隔。
  return a.decoded.channel == b.decoded.channel &&
         a.decoded.pseudo_channel == b.decoded.pseudo_channel &&
         a.decoded.rank == b.decoded.rank;
}

bool is_activate(Command cmd) {
  return cmd == Command::ACT || cmd == Command::ACT1;
}

bool is_data(Command cmd) {
  return cmd == Command::RD || cmd == Command::WR || cmd == Command::RDA ||
         cmd == Command::WRA;
}

bool is_column(Command cmd) {
  return cmd == Command::CASRD || cmd == Command::CASWR || cmd == Command::RD ||
         cmd == Command::WR || cmd == Command::RDA || cmd == Command::WRA;
}

bool is_falling_pre(Command cmd) {
  return cmd == Command::PREPB || cmd == Command::PREAB;
}

bool report_contains(const hbm_sim::CommandValidationReport &report,
                     const std::string &text) {
  for (const auto &error : report.errors) {
    if (error.find(text) != std::string::npos) {
      return true;
    }
  }
  return false;
}

const IssuedCommand *find_first(const std::vector<IssuedCommand> &trace,
                                Command cmd) {
  // LPDDR 序列测试只需要第一条目标命令，因为每个测试只构造一个请求。
  for (const auto &issued : trace) {
    if (issued.command == cmd) {
      return &issued;
    }
  }
  return nullptr;
}

void test_jedec_conversion_helpers() {
  require(hbm_sim::jedec::hbm_tccdl_nck(500.0) == 5,
          "HBM4 tCCDL Max(4nCK, 2.5ns/tCK) conversion is wrong");
  require(hbm_sim::jedec::hbm_trefipb_nck(500.0, 32) == 244,
          "HBM4 8-high tREFIpb conversion is wrong");
  require(hbm_sim::jedec::max_ns_or_nck(20.7, 2, 375.0) == 56,
          "LPDDR6 Max(ns, nCK) conversion is wrong");
  require(hbm_sim::jedec::ns_to_nck(488.0, 375.0) == 1302,
          "LPDDR6 REFdb interval conversion is wrong");
}

void test_unified_spec_factory() {
  const DramSpec draft = hbm_sim::make_spec_draft("hbm4");
  const DramSpec hbm4 = hbm_sim::make_spec("HBM");
  const DramSpec hbm3 = hbm_sim::make_spec("hbm3");
  const DramSpec lpddr6 = hbm_sim::make_spec("LpDdR6");
  const DramSpec lpddr5 = hbm_sim::make_spec("lpddr5");

  require(hbm4.name == "HBM4" && hbm4.supports_rfm &&
              hbm4.org.dram_transaction_bytes == 32,
          "unified factory did not select the HBM4 traits");
  require(hbm3.name == "HBM3" && !hbm3.supports_rfm,
          "unified factory did not select the HBM3 traits");
  require(lpddr6.name == "LPDDR6" && lpddr6.lpddr_family &&
              lpddr6.org.pseudo_channels == 2,
          "unified factory did not select the LPDDR6 traits");
  require(lpddr5.name == "LPDDR5" && lpddr5.lpddr_family &&
              lpddr5.org.pseudo_channels == 1,
          "unified factory did not select the LPDDR5 traits");
  require(!hbm4.timing_constraints.empty() &&
              !lpddr6.timing_table.entries.empty(),
          "unified factory did not finalize derived timing data");
  require(draft.standard == hbm_sim::DramStandard::Hbm4 &&
              draft.org.channels == 1 && draft.timing_constraints.empty() &&
              draft.timing_table.entries.empty(),
          "draft factory should apply traits without profile organization or "
          "derived timing data");

  DramSpec first = hbm_sim::make_spec("hbm4");
  first.org.rows = 1;
  require(hbm_sim::make_spec("hbm4").org.rows != 1,
          "factory callers unexpectedly share mutable catalog state");

  const std::vector<std::string> supported = hbm_sim::supported_specs();
  require(supported ==
              std::vector<std::string>({"hbm4", "hbm3", "lpddr6", "lpddr5"}),
          "supported standard list diverged from the traits catalog");

  bool rejected = false;
  try {
    static_cast<void>(hbm_sim::make_spec("ddr7"));
  } catch (const std::invalid_argument &) {
    rejected = true;
  }
  require(rejected, "unified factory accepted an unsupported standard");
}

void test_timing_table_validation() {
  DramSpec hbm4 = hbm_sim::make_spec("hbm4");
  DramSpec lpddr6 = hbm_sim::make_spec("lpddr6");

  require(!hbm4.timing_table.entries.empty(), "HBM4 timing table is empty");
  require(!lpddr6.timing_table.entries.empty(), "LPDDR6 timing table is empty");
  require(hbm_sim::validate_timing_table(hbm4, false).empty(),
          "HBM4 timing table is not complete for the current model");
  require(hbm_sim::validate_timing_table(lpddr6, false).empty(),
          "LPDDR6 timing table is not complete for the current model");
  require(!hbm_sim::validate_timing_table(hbm4, true).empty(),
          "HBM4 strict timing validation should require vendor-calibrated row "
          "values");
}

void test_library_spec_rejects_unimplemented_or_invalid_protocol_modes() {
  // 库调用方不会经过 CLI 的 validate_model_config()，所以协议模式的
  // 可执行性约束必须由 finalize_spec() 自身守住。
  DramSpec burst_sync = hbm_sim::make_spec("lpddr6");
  burst_sync.lpddr_wck_mode = hbm_sim::LpddrWckMode::BurstSync;
  bool rejected = false;
  try {
    hbm_sim::finalize_spec(burst_sync);
  } catch (const std::invalid_argument &) {
    rejected = true;
  }
  require(rejected, "library API accepted unimplemented burst-sync WCK mode");

  DramSpec bad_aad = hbm_sim::make_spec("lpddr6");
  bad_aad.timing.nAADMin = 9;
  bad_aad.timing.nAADMax = 8;
  rejected = false;
  try {
    hbm_sim::finalize_spec(bad_aad);
  } catch (const std::invalid_argument &) {
    rejected = true;
  }
  require(rejected, "library API accepted an inverted tAAD issue window");

  DramSpec bad_refdb = hbm_sim::make_spec("lpddr6");
  bad_refdb.org.bank_groups = 3;
  rejected = false;
  try {
    hbm_sim::finalize_spec(bad_refdb);
  } catch (const std::invalid_argument &) {
    rejected = true;
  }
  require(rejected, "library API accepted an unpairable REFdb topology");

  DramSpec bad_tick = hbm_sim::make_spec("hbm4");
  bad_tick.tick_multiplier = 0;
  rejected = false;
  try {
    hbm_sim::finalize_spec(bad_tick);
  } catch (const std::invalid_argument &) {
    rejected = true;
  }
  require(rejected, "library API silently clamped a zero tick multiplier");

  DramSpec bad_stack_height = hbm_sim::make_spec("hbm4");
  bad_stack_height.stack_height = 0;
  rejected = false;
  try {
    hbm_sim::MemoryImage invalid(bad_stack_height);
  } catch (const std::invalid_argument &) {
    rejected = true;
  }
  require(rejected,
          "MemoryImage silently replaced an invalid HBM stack height");

  DramSpec bad_density = hbm_sim::make_spec("hbm4");
  bad_density.density_gb = 0;
  rejected = false;
  try {
    (void)hbm_sim::AddressMapper(bad_density);
  } catch (const std::invalid_argument &) {
    rejected = true;
  }
  require(rejected, "AddressMapper accepted a non-positive density identity");

  DramSpec bad_overhead = hbm_sim::make_spec("hbm4");
  bad_overhead.metadata_bits_per_request = -1;
  rejected = false;
  try {
    hbm_sim::finalize_spec(bad_overhead);
  } catch (const std::invalid_argument &) {
    rejected = true;
  }
  require(rejected, "library API silently clamped negative interface overhead");

  hbm_sim::ControllerOptions bad_controller_options;
  bad_controller_options.read_buffer_size = 0;
  rejected = false;
  try {
    Controller invalid(hbm_sim::make_spec("hbm4"), bad_controller_options);
  } catch (const std::invalid_argument &) {
    rejected = true;
  }
  require(rejected, "library API silently clamped a zero controller buffer");

  rejected = false;
  try {
    hbm_sim::RowPolicyEngine invalid(0, 1);
  } catch (const std::invalid_argument &) {
    rejected = true;
  }
  require(rejected, "row-policy API accepted a zero bank count");

  rejected = false;
  try {
    hbm_sim::RowPolicyEngine invalid(1, 0);
  } catch (const std::invalid_argument &) {
    rejected = true;
  }
  require(rejected, "row-policy API silently clamped a zero ClosedCAP limit");

  hbm_sim::StorageModelOptions bad_storage_options;
  bad_storage_options.thermal_grid_cols_per_tile = 0;
  rejected = false;
  try {
    hbm_sim::MemoryImage invalid(hbm_sim::make_spec("hbm4"), 0,
                                 bad_storage_options);
  } catch (const std::invalid_argument &) {
    rejected = true;
  }
  require(rejected, "library API silently clamped an invalid thermal grid");

  hbm_sim::StorageModelOptions bad_idd_options;
  bad_idd_options.power_source = "dramsim3_idd";
  bad_idd_options.idd_devices_per_rank = 0.0;
  rejected = false;
  try {
    hbm_sim::MemoryImage invalid(hbm_sim::make_spec("hbm4"), 0,
                                 bad_idd_options);
  } catch (const std::invalid_argument &) {
    rejected = true;
  }
  require(rejected,
          "IDD power mode silently replaced zero devices_per_rank with one");

  DramSpec request_spec = hbm_sim::make_spec("hbm4");
  Controller request_controller(request_spec);
  Request invalid_request = make_request(999, RequestType::Read, 0, 0, 0, 0);
  invalid_request.decoded.bank = request_spec.org.banks_per_group;
  rejected = false;
  try {
    (void)request_controller.enqueue(invalid_request);
  } catch (const std::out_of_range &) {
    rejected = true;
  }
  require(rejected, "Controller accepted an out-of-range decoded bank");
}

void test_timing_engine_table_and_window_state() {
  DramSpec spec = hbm_sim::make_spec("hbm4");
  spec.tick_multiplier = 1;
  spec.timing.nFAW = 16;
  spec.timing_constraints.clear();
  spec.timing_constraints.push_back(hbm_sim::TimingConstraint{
      hbm_sim::TimingScope::Bank,
      {Command::ACT},
      {Command::RD},
      7,
      0,
      false,
      "unit test ACT->RD table constraint",
      "unit_nRCD",
  });

  DecodedAddress decoded;
  decoded.channel = 0;
  decoded.pseudo_channel = 0;
  decoded.sid = 0;
  decoded.rank = 0;
  decoded.bank_group = 0;
  decoded.bank = 0;

  hbm_sim::TimingEngine engine(spec);
  require(
      engine.constraint_ready(spec, decoded, Command::RD, 10),
      "TimingEngine should allow RD before any ACT->RD constraint is applied");
  engine.apply_constraints(spec, decoded, Command::ACT, 10);
  require(!engine.constraint_ready(spec, decoded, Command::RD, 16),
          "TimingEngine allowed RD before table-driven ACT->RD latency");
  require(engine.constraint_ready(spec, decoded, Command::RD, 17),
          "TimingEngine did not release RD after table-driven ACT->RD latency");

  for (Cycle clk = 1; clk <= 4; clk++) {
    engine.record_activate(spec, decoded, clk);
  }
  require(!engine.faw_ready(spec, decoded),
          "TimingEngine should block the fifth activate inside tFAW");
  engine.prune_recent_acts(spec, 17);
  require(engine.faw_ready(spec, decoded),
          "TimingEngine did not release tFAW window after old ACT expired");

  DramSpec lpddr = hbm_sim::make_spec("lpddr6");
  hbm_sim::TimingEngine lpddr_engine(lpddr);
  auto &wck = lpddr_engine.wck_state(lpddr, decoded);
  wck.wck_ready_at = 12;
  wck.wck_active_until = 20;
  require(!lpddr_engine.wck_ready_for_data(lpddr, decoded, 11),
          "TimingEngine made WCK ready before ready_at");
  require(lpddr_engine.wck_ready_for_data(lpddr, decoded, 12),
          "TimingEngine did not make WCK ready at ready_at");
  require(!lpddr_engine.wck_ready_for_data(lpddr, decoded, 20),
          "TimingEngine kept WCK ready at active_until boundary");

  DramSpec hbm = hbm_sim::make_spec("hbm4");
  hbm.tick_multiplier = 1;
  hbm.org.sids = 2;
  const auto ccdr =
      std::find_if(hbm.timing_constraints.begin(), hbm.timing_constraints.end(),
                   [](const hbm_sim::TimingConstraint &constraint) {
                     return constraint.parameter == "nCCDR";
                   });
  require(ccdr != hbm.timing_constraints.end() && ccdr->sibling,
          "HBM nCCDR is not encoded as a different-SID constraint");
  hbm.timing_constraints = {*ccdr};
  hbm_sim::TimingEngine ccdr_engine(hbm);
  DecodedAddress other_sid = decoded;
  other_sid.sid = 1;
  ccdr_engine.apply_constraints(hbm, decoded, Command::RD, 100);
  require(ccdr_engine.constraint_ready(hbm, decoded, Command::RD, 100),
          "nCCDR incorrectly blocked consecutive READs to the same SID");
  require(!ccdr_engine.constraint_ready(
              hbm, other_sid, Command::RD,
              100 + static_cast<Cycle>(hbm.timing.nCCDR - 1)),
          "nCCDR did not block consecutive READs to a different SID at t-1");
  require(
      ccdr_engine.constraint_ready(hbm, other_sid, Command::RD,
                                   100 + static_cast<Cycle>(hbm.timing.nCCDR)),
      "nCCDR did not release different SID at t");
}

void test_command_executor_state_transitions() {
  DramSpec spec = hbm_sim::make_spec("hbm4");
  spec.supports_refresh = false;
  spec.supports_rfm = true;
  spec.rfm_act_threshold = 1;
  spec.tick_multiplier = 1;
  hbm_sim::refresh_timing_constraints(spec);

  std::vector<hbm_sim::BankState> banks(
      static_cast<std::size_t>(spec.total_banks()));
  hbm_sim::TimingEngine timing_engine(spec);
  hbm_sim::RfmManager rfm_manager;
  rfm_manager.reset(spec);
  hbm_sim::Stats stats;
  hbm_sim::CommandExecutor executor(spec, banks, timing_engine, rfm_manager,
                                    stats);

  Request req = make_request(300, RequestType::Read, 0, 0, 0, 77);
  auto result = executor.issue(req, Command::ACT, 10);
  int flat = req.decoded.flat_bank(spec);
  require(banks[flat].open_row == 77,
          "CommandExecutor ACT did not open target row");
  require(banks[flat].next_rd == 10 + static_cast<Cycle>(spec.timing.nRCDRD),
          "CommandExecutor ACT did not program read gate");
  require(result.rfm_command.has_value() &&
              result.rfm_command->command == Command::RFMPB,
          "CommandExecutor ACT did not report RFMpb target");
  require(stats.act == 1 && stats.rfm_events == 1,
          "CommandExecutor ACT did not update command/RFM stats");

  executor.issue(req, Command::RDA, 40);
  require(banks[flat].open_row == -1,
          "CommandExecutor RDA did not auto-precharge row");
  require(stats.rda == 1, "CommandExecutor RDA did not update stats");

  banks[flat].open_row = 88;
  DecodedAddress other = req.decoded;
  other.bank = 1;
  banks[other.flat_bank(spec)].open_row = 99;
  executor.issue(req, Command::PREAB, 80);
  require(banks[flat].open_row == -1 &&
              banks[other.flat_bank(spec)].open_row == -1,
          "CommandExecutor PREab did not close all banks");
  require(stats.preab == 1, "CommandExecutor PREab did not update stats");

  DramSpec lpddr = hbm_sim::make_spec("lpddr6");
  lpddr.tick_multiplier = 1;
  lpddr.timing.nAADMin = 1;
  lpddr.timing.nAADMax = 8;
  lpddr.timing.nRCDRD = 12;
  std::vector<hbm_sim::BankState> lpddr_banks(
      static_cast<std::size_t>(lpddr.total_banks()));
  hbm_sim::TimingEngine lpddr_timing(lpddr);
  hbm_sim::RfmManager lpddr_rfm;
  lpddr_rfm.reset(lpddr);
  hbm_sim::Stats lpddr_stats;
  hbm_sim::CommandExecutor lpddr_executor(lpddr, lpddr_banks, lpddr_timing,
                                          lpddr_rfm, lpddr_stats);
  Request split = make_request(301, RequestType::Read, 0, 0, 0, 5);
  lpddr_executor.issue(split, Command::ACT1, 10);
  const int split_flat = split.decoded.flat_bank(lpddr);
  require(lpddr_banks[split_flat].next_act2 == 11 &&
              lpddr_banks[split_flat].act2_deadline == 18,
          "ACT1 did not establish distinct nAADMin/nAADMax boundaries");
  require(lpddr_banks[split_flat].next_rd == 22,
          "ACT1 did not establish absolute nRCDRD gate");
  lpddr_executor.issue(split, Command::ACT2, 12);
  require(lpddr_banks[split_flat].next_rd == 22,
          "ACT2 issue position incorrectly changed ACT1-relative nRCDRD");
}

void test_command_trace_plugins() {
  DramSpec spec = hbm_sim::make_spec("hbm4");
  spec.supports_refresh = false;
  spec.supports_rfm = false;
  hbm_sim::refresh_timing_constraints(spec);

  Controller controller(spec);
  controller.enqueue(make_request(310, RequestType::Read, 0, 0, 0, 81));
  controller.run_until_done(2000);

  const auto &trace = controller.issued_commands();
  require(!trace.empty(), "command trace plugin test did not produce commands");
  auto report = hbm_sim::validate_command_trace(spec, trace);
  require(report.ok(), "command trace validator rejected a valid HBM4 trace");
  require(report.checked_commands == trace.size(),
          "command trace validator did not report checked command count");
  require(report.state_checks == trace.size(),
          "command trace validator did not report state-check coverage");
  require(report.bus_checks == trace.size() &&
              report.edge_checks == trace.size(),
          "command trace validator did not report HBM bus/edge coverage");
  require(report.timing_constraint_checks > 0 &&
              report.timing_constraint_updates > 0,
          "command trace validator did not report timing coverage");
  require(report.faw_events_checked > 0,
          "command trace validator did not report activation-window coverage");

  std::stringstream csv;
  hbm_sim::write_command_trace_csv(csv, trace);
  require(csv.str().find("cycle,request_id,command,bus,stack_id,channel") == 0,
          "command trace recorder wrote an invalid CSV header");

  std::vector<IssuedCommand> broken = trace;
  for (auto &issued : broken) {
    if (is_column(issued.command)) {
      issued.cycle = issued.cycle % 2 == 0 ? issued.cycle : issued.cycle + 1;
      break;
    }
  }
  auto broken_report = hbm_sim::validate_command_trace(spec, broken);
  require(!broken_report.ok(),
          "command trace validator missed a falling-edge column violation");

  DecodedAddress decoded;
  decoded.channel = 0;
  decoded.pseudo_channel = 0;
  decoded.sid = 0;
  decoded.rank = 0;
  decoded.bank_group = 0;
  decoded.bank = 0;
  decoded.row = 9;
  std::vector<IssuedCommand> closed_bank_rd = {
      IssuedCommand{1, 900, Command::RD, hbm_sim::BusClass::Column, decoded},
  };
  auto state_report = hbm_sim::validate_command_trace(spec, closed_bank_rd);
  require(!state_report.ok(),
          "command trace validator missed RD from a closed bank");

  std::vector<IssuedCommand> double_act = {
      IssuedCommand{1, 901, Command::ACT, hbm_sim::BusClass::Row, decoded},
      IssuedCommand{20, 902, Command::ACT, hbm_sim::BusClass::Row, decoded},
  };
  auto double_act_report = hbm_sim::validate_command_trace(spec, double_act);
  require(!double_act_report.ok(),
          "command trace validator missed ACT to an already-open bank");

  std::vector<IssuedCommand> invalid_coordinate = trace;
  invalid_coordinate.front().decoded.bank = spec.org.banks_per_group;
  auto coordinate_report =
      hbm_sim::validate_command_trace(spec, invalid_coordinate);
  require(!coordinate_report.ok(),
          "command trace validator silently clamped an invalid bank");
}

void test_dfi_trace_generation() {
  DramSpec spec = hbm_sim::make_spec("hbm4");
  spec.supports_refresh = false;
  spec.supports_rfm = false;
  spec.hbm_edge_pairing = false;
  spec.tick_multiplier = 1;
  spec.dfi_phase_count = 2;
  spec.dfi_data_lane_bytes = 16;
  spec.dfi_read_latency_nck = 5;
  spec.dfi_write_latency_nck = 3;
  hbm_sim::refresh_timing_constraints(spec);

  DecodedAddress decoded;
  decoded.channel = 0;
  decoded.pseudo_channel = 0;
  decoded.sid = 0;
  decoded.rank = 0;
  decoded.bank_group = 0;
  decoded.bank = 0;
  decoded.row = 1;
  decoded.column = 2;
  std::vector<IssuedCommand> commands = {
      IssuedCommand{10, 1100, Command::RD, hbm_sim::BusClass::Column, decoded},
      IssuedCommand{20, 1101, Command::WR, hbm_sim::BusClass::Column, decoded},
  };
  commands[0].payload.resize(64);
  commands[0].initialized_mask.assign(64, 0xff);
  for (std::size_t i = 0; i < commands[0].payload.size(); i++) {
    commands[0].payload[i] = static_cast<std::uint8_t>(0x80 + i);
  }
  commands[0].has_payload = true;
  commands[0].expected_payload = commands[0].payload;
  commands[0].has_expected_payload = true;
  commands[0].payload_initialized = true;

  commands[1].payload.resize(64);
  commands[1].byte_mask.assign(64, 0xff);
  for (std::size_t i = 0; i < commands[1].payload.size(); i++) {
    commands[1].payload[i] = static_cast<std::uint8_t>(i);
  }
  commands[1].byte_mask[0] = 0x00;
  commands[1].has_payload = true;
  commands[1].has_byte_mask = true;
  commands[1].payload_initialized = true;

  auto events = hbm_sim::build_dfi_trace(spec, commands);
  require(hbm_sim::dfi_phase_count(spec) == 2,
          "DFI phase count override was ignored");
  require(hbm_sim::dfi_payload_beat_bytes(spec) == 16,
          "DFI data lane override was ignored");
  require(events.size() == 10,
          "DFI trace should contain two command events and eight data beats");

  int command_events = 0;
  int read_data_events = 0;
  int write_data_events = 0;
  bool saw_read_first_beat = false;
  bool saw_write_first_beat = false;
  bool saw_command_signal = false;
  bool saw_read_signal = false;
  bool saw_write_signal = false;
  bool saw_real_read_payload = false;
  bool saw_real_write_payload = false;
  bool saw_dfi_write_mask = false;
  for (const auto &event : events) {
    if (event.kind == hbm_sim::DfiEventKind::Command) {
      command_events++;
      if (!event.dfi_cs_n && event.dfi_address != 0 && event.dfi_reset_n &&
          event.dfi_cke) {
        saw_command_signal = true;
      }
    } else if (event.kind == hbm_sim::DfiEventKind::ReadData) {
      read_data_events++;
      if (event.request_id == 1100 && event.beat == 0 && event.cycle == 15 &&
          event.beat_count == 4 && event.beat_bytes == 16) {
        saw_read_first_beat = true;
      }
      if (event.dfi_rddata_valid && !event.dfi_rddata.empty()) {
        saw_read_signal = true;
      }
      if (event.request_id == 1100 && event.beat == 0 &&
          event.payload_source == "memory_image" &&
          event.dfi_rddata == "808182838485868788898a8b8c8d8e8f") {
        saw_real_read_payload = true;
      }
    } else if (event.kind == hbm_sim::DfiEventKind::WriteData) {
      write_data_events++;
      if (event.request_id == 1101 && event.beat == 0 && event.cycle == 23 &&
          event.beat_count == 4 && event.beat_bytes == 16) {
        saw_write_first_beat = true;
      }
      if (event.dfi_wrdata_en && !event.dfi_wrdata.empty() &&
          !event.dfi_wrdata_mask.empty()) {
        saw_write_signal = true;
      }
      if (event.request_id == 1101 && event.beat == 0 &&
          event.payload_source == "request_payload" &&
          event.dfi_wrdata == "000102030405060708090a0b0c0d0e0f") {
        saw_real_write_payload = true;
      }
      if (event.request_id == 1101 && event.beat == 0 &&
          event.dfi_wrdata_mask == "ff000000000000000000000000000000") {
        saw_dfi_write_mask = true;
      }
    }
  }
  require(command_events == 2, "DFI trace command event count is wrong");
  require(read_data_events == 4, "DFI trace read beat count is wrong");
  require(write_data_events == 4, "DFI trace write beat count is wrong");
  require(saw_read_first_beat,
          "DFI trace did not place first read beat at dfi_read_latency");
  require(saw_write_first_beat,
          "DFI trace did not place first write beat at dfi_write_latency");
  require(saw_command_signal && saw_read_signal && saw_write_signal,
          "DFI trace did not populate signal-level fields");
  require(saw_real_read_payload && saw_real_write_payload,
          "DFI trace did not use real payload snapshots for data beats");
  require(saw_dfi_write_mask,
          "DFI trace did not export active-high write mask semantics");

  auto valid_report = hbm_sim::validate_dfi_trace(spec, commands, events);
  require(valid_report.ok(),
          "DFI validator rejected a valid beat/signal trace");
  require(valid_report.checked_events == events.size() &&
              valid_report.command_checks == commands.size() &&
              valid_report.data_beat_checks == 8 &&
              valid_report.latency_checks == 8 &&
              valid_report.payload_checks == 8 &&
              valid_report.expected_payload_checks == 4,
          "DFI validator coverage counters are incomplete");

  // 不同 stack 的控制器可能独立分配 request_id；DFI 校验必须以 stack_id
  // 隔离同 ID、同命令、同周期事件，不能把它们合并成重复 beat。
  auto cross_stack_commands = commands;
  cross_stack_commands.resize(2);
  cross_stack_commands[1] = cross_stack_commands[0];
  cross_stack_commands[1].stack_id = 1;
  cross_stack_commands[1].system_address += 0x100000;
  cross_stack_commands[1].payload[0] ^= 0xff;
  cross_stack_commands[1].expected_payload = cross_stack_commands[1].payload;
  const auto cross_stack_events =
      hbm_sim::build_dfi_trace(spec, cross_stack_commands);
  const auto cross_stack_report = hbm_sim::validate_dfi_trace(
      spec, cross_stack_commands, cross_stack_events);
  require(cross_stack_report.ok() && cross_stack_report.command_checks == 2 &&
              cross_stack_report.data_beat_checks == 8,
          "DFI validator aliased identical request IDs across stacks");

  auto bad_latency = events;
  for (auto &event : bad_latency) {
    if (event.kind == hbm_sim::DfiEventKind::ReadData && event.beat == 0) {
      event.cycle++;
      break;
    }
  }
  require(!hbm_sim::validate_dfi_trace(spec, commands, bad_latency).ok(),
          "DFI validator missed a corrupted read-data latency");

  auto bad_payload = events;
  for (auto &event : bad_payload) {
    if (event.kind == hbm_sim::DfiEventKind::WriteData && event.beat == 0) {
      event.dfi_wrdata.pop_back();
      break;
    }
  }
  require(!hbm_sim::validate_dfi_trace(spec, commands, bad_payload).ok(),
          "DFI validator missed a corrupted write payload width");

  auto bad_payload_value = events;
  for (auto &event : bad_payload_value) {
    if (event.kind == hbm_sim::DfiEventKind::ReadData && event.beat == 0) {
      event.dfi_rddata[0] = event.dfi_rddata[0] == '0' ? '1' : '0';
      break;
    }
  }
  require(!hbm_sim::validate_dfi_trace(spec, commands, bad_payload_value).ok(),
          "DFI validator missed a same-width payload corruption");

  auto wrong_expectation = commands;
  wrong_expectation[0].expected_payload[0] ^= 0xff;
  require(!hbm_sim::validate_dfi_trace(spec, wrong_expectation, events).ok(),
          "DFI validator did not independently check the request expectation");

  auto bad_command_enable = events;
  for (auto &event : bad_command_enable) {
    if (event.kind == hbm_sim::DfiEventKind::Command &&
        event.command == Command::RD) {
      event.dfi_rddata_en = false;
      break;
    }
  }
  require(!hbm_sim::validate_dfi_trace(spec, commands, bad_command_enable).ok(),
          "DFI validator missed an invalid read command enable");

  auto orphan_data = events;
  for (const auto &event : events) {
    if (event.kind == hbm_sim::DfiEventKind::ReadData) {
      auto orphan = event;
      orphan.request_id += 10000;
      orphan_data.push_back(std::move(orphan));
      break;
    }
  }
  require(!hbm_sim::validate_dfi_trace(spec, commands, orphan_data).ok(),
          "DFI validator missed orphan data indexed to no issued command");

  const std::string signal_path = "/tmp/hbm_sim_dfi_signal_trace.csv";
  hbm_sim::write_dfi_signal_trace_csv(signal_path, events);
  {
    std::ifstream in(signal_path);
    std::stringstream buffer;
    buffer << in.rdbuf();
    require(buffer.str().find("dfi_wrdata_en") != std::string::npos &&
                buffer.str().find("dfi_rddata_valid") != std::string::npos &&
                buffer.str().find("dfi_address") != std::string::npos &&
                buffer.str().find("payload_source") != std::string::npos,
            "DFI signal trace CSV did not include signal fields");
  }
}

void test_hbm4_edge_pairing_validator_matrix() {
  DramSpec spec = hbm_sim::make_spec("hbm4");
  spec.supports_refresh = false;
  spec.supports_rfm = false;
  hbm_sim::refresh_timing_constraints(spec);

  DecodedAddress bank0;
  bank0.channel = 0;
  bank0.pseudo_channel = 0;
  bank0.sid = 0;
  bank0.rank = 0;
  bank0.bank_group = 0;
  bank0.bank = 0;
  bank0.row = 7;
  std::vector<IssuedCommand> invalid_preab_pairing = {
      IssuedCommand{1, 930, Command::ACT, hbm_sim::BusClass::Row, bank0},
      IssuedCommand{4, 931, Command::PREAB, hbm_sim::BusClass::Row, bank0},
  };
  auto invalid_report =
      hbm_sim::validate_command_trace(spec, invalid_preab_pairing);
  require(!invalid_report.ok(),
          "validator missed illegal HBM4 falling-edge PREab pairing");
  require(invalid_report.edge_pairing_checks > 0,
          "validator did not count HBM4 edge-pairing checks");
  require(report_contains(invalid_report, "edge pairing matrix"),
          "validator did not report the HBM4 edge-pairing violation reason");
}

void test_command_state_machine() {
  using hbm_sim::BankProtocolState;
  using hbm_sim::Command;
  using hbm_sim::CommandStateSnapshot;

  CommandStateSnapshot closed;
  closed.bank_state = BankProtocolState::Closed;
  require(hbm_sim::check_command_state(closed, Command::ACT).legal,
          "closed HBM bank should accept ACT");
  require(!hbm_sim::check_command_state(closed, Command::RD).legal,
          "closed bank must reject RD");

  CommandStateSnapshot opened;
  opened.bank_state = BankProtocolState::Opened;
  opened.row_hit = true;
  require(hbm_sim::check_command_state(opened, Command::RD).legal,
          "opened row-hit bank should accept RD");
  opened.row_hit = false;
  require(!hbm_sim::check_command_state(opened, Command::RDA).legal,
          "opened row-conflict bank must reject RDA");

  CommandStateSnapshot lpddr = opened;
  lpddr.row_hit = true;
  lpddr.lpddr_family = true;
  lpddr.wck_ready = false;
  require(hbm_sim::check_command_state(lpddr, Command::CASRD).legal,
          "LPDDR row-hit bank should accept CAS_RD before WCK is ready");
  require(!hbm_sim::check_command_state(lpddr, Command::RD).legal,
          "LPDDR data RD must wait for WCK ready window");
}

void test_control_command_state_and_validator() {
  using hbm_sim::BankProtocolState;
  using hbm_sim::Command;
  using hbm_sim::CommandStateSnapshot;

  CommandStateSnapshot idle_control;
  idle_control.bank_state = BankProtocolState::Closed;
  idle_control.maintenance = true;
  require(hbm_sim::check_command_state(idle_control, Command::MRW).legal,
          "MRW should be legal for an idle maintenance/control sequence");
  idle_control.any_bank_busy = true;
  require(!hbm_sim::check_command_state(idle_control, Command::MRW).legal,
          "MRW must be blocked while any bank is busy");

  CommandStateSnapshot power_exit;
  power_exit.maintenance = true;
  power_exit.low_power_active = true;
  require(hbm_sim::check_command_state(power_exit, Command::PDX).legal,
          "PDX should be legal only from power-down state");
  power_exit.low_power_active = false;
  require(!hbm_sim::check_command_state(power_exit, Command::PDX).legal,
          "PDX must reject active state");

  CommandStateSnapshot ras;
  ras.maintenance = true;
  ras.ras_ecc_supported = true;
  require(hbm_sim::check_command_state(ras, Command::ECCSCRUB).legal,
          "ECC_SCRUB should be legal when RAS/ECC support is present and "
          "channel idle");
  ras.ras_ecc_supported = false;
  require(!hbm_sim::check_command_state(ras, Command::ECCSCRUB).legal,
          "ECC_SCRUB must reject standards without RAS/ECC support");

  DramSpec lpddr = hbm_sim::make_spec("lpddr6");
  lpddr.supports_refresh = false;
  lpddr.supports_rfm = false;
  lpddr.lpddr_link_protection = true;
  lpddr.lpddr_link_ecc_enabled = true;
  hbm_sim::refresh_timing_constraints(lpddr);

  DecodedAddress decoded;
  decoded.channel = 0;
  decoded.pseudo_channel = 0;
  decoded.rank = 0;
  decoded.bank_group = 0;
  decoded.bank = 0;
  decoded.row = 9;

  std::vector<IssuedCommand> valid_control = {
      IssuedCommand{1, 1000, Command::MRW, hbm_sim::BusClass::Unified, decoded},
      IssuedCommand{80, 1001, Command::MRR, hbm_sim::BusClass::Unified,
                    decoded},
      IssuedCommand{200, 1002, Command::DVFS, hbm_sim::BusClass::Unified,
                    decoded},
      IssuedCommand{600, 1003, Command::WCKTRAIN, hbm_sim::BusClass::Unified,
                    decoded},
      IssuedCommand{900, 1004, Command::PDE, hbm_sim::BusClass::Unified,
                    decoded},
      IssuedCommand{930, 1005, Command::PDX, hbm_sim::BusClass::Unified,
                    decoded},
      IssuedCommand{980, 1006, Command::SREFEN, hbm_sim::BusClass::Unified,
                    decoded},
      IssuedCommand{1260, 1007, Command::SREFEX, hbm_sim::BusClass::Unified,
                    decoded},
      IssuedCommand{1540, 1008, Command::ECCSCRUB, hbm_sim::BusClass::Unified,
                    decoded},
      IssuedCommand{1700, 1009, Command::RASERR, hbm_sim::BusClass::Unified,
                    decoded},
  };
  auto valid_report = hbm_sim::validate_command_trace(lpddr, valid_control);
  require(valid_report.ok(),
          "validator rejected a valid LPDDR6 control-state sequence");

  std::vector<IssuedCommand> invalid_dvfs_without_training = {
      IssuedCommand{1, 1020, Command::MRW, hbm_sim::BusClass::Unified, decoded},
      IssuedCommand{200, 1021, Command::DVFS, hbm_sim::BusClass::Unified,
                    decoded},
      IssuedCommand{700, 1022, Command::ACT1, hbm_sim::BusClass::Unified,
                    decoded},
      IssuedCommand{760, 1022, Command::ACT2, hbm_sim::BusClass::Unified,
                    decoded},
      IssuedCommand{900, 1022, Command::CASRD, hbm_sim::BusClass::Unified,
                    decoded},
  };
  auto invalid_dvfs_report =
      hbm_sim::validate_command_trace(lpddr, invalid_dvfs_without_training);
  require(!invalid_dvfs_report.ok(),
          "validator missed CAS_RD after DVFS without WCK_TRAIN");
  require(invalid_dvfs_report.checked_commands ==
              invalid_dvfs_without_training.size(),
          "validator report lost command count on invalid LPDDR6 trace");

  DramSpec hbm4 = hbm_sim::make_spec("hbm4");
  hbm4.supports_refresh = false;
  hbm4.supports_rfm = false;
  hbm4.hbm_edge_pairing = false;
  hbm_sim::refresh_timing_constraints(hbm4);
  std::vector<IssuedCommand> invalid_mrw = {
      IssuedCommand{1, 1010, Command::ACT, hbm_sim::BusClass::Row, decoded},
      IssuedCommand{40, 1011, Command::MRW, hbm_sim::BusClass::Row, decoded},
  };
  auto invalid_report = hbm_sim::validate_command_trace(hbm4, invalid_mrw);
  require(!invalid_report.ok(), "validator missed MRW while a bank was open");
}

void test_initialization_control_sequence_execution() {
  DramSpec hbm4 = hbm_sim::make_spec("hbm4");
  hbm4.org.channels = 2;
  set_fixture_density_from_geometry(hbm4);
  hbm4.supports_refresh = false;
  hbm4.supports_rfm = false;
  hbm4.hbm_edge_pairing = false;
  hbm4.hbm_link_retry_enabled = true;
  hbm4.hbm_link_crc_bits_per_request = 16;
  hbm4.hbm_ras_metadata_bits_per_request = 16;
  hbm_sim::refresh_timing_constraints(hbm4);

  hbm_sim::TrafficOptions hbm_opts;
  hbm_opts.requests = 0;
  hbm_opts.init_sequence = "hbm4";
  std::vector<Request> hbm_requests;
  hbm_sim::prepend_control_sequence(hbm4, hbm_opts, hbm_requests);
  require(hbm_requests.size() == 8,
          "HBM4 init sequence should emit MRW/MRR/ECC/RAS for each channel");

  MemorySystem hbm_memory(hbm4);
  hbm_memory.run(hbm_requests, 5000);
  require(!hbm_memory.stats().hit_cycle_limit,
          "HBM4 init sequence hit cycle limit");
  require(hbm_memory.stats().mrw == 2 && hbm_memory.stats().mrr == 2,
          "HBM4 init sequence did not execute MRW/MRR on both channels");
  require(hbm_memory.stats().ecc_scrub == 2 && hbm_memory.stats().ras_err == 2,
          "HBM4 init sequence did not execute RAS/ECC control commands");
  require(hbm_memory.stats().mode_register_ops == 4 &&
              hbm_memory.stats().ras_ecc_events == 4,
          "MemorySystem did not aggregate HBM4 control command stats");

  DramSpec lpddr = hbm_sim::make_spec("lpddr6");
  lpddr.supports_refresh = false;
  lpddr.supports_rfm = false;
  lpddr.lpddr_link_protection = true;
  lpddr.lpddr_link_ecc_enabled = true;
  hbm_sim::refresh_timing_constraints(lpddr);

  hbm_sim::TrafficOptions lpddr_opts;
  lpddr_opts.requests = 0;
  lpddr_opts.init_sequence = "lpddr6_full";
  std::vector<Request> lpddr_requests;
  hbm_sim::prepend_control_sequence(lpddr, lpddr_opts, lpddr_requests);
  MemorySystem lpddr_memory(lpddr);
  lpddr_memory.run(lpddr_requests, 10000);
  require(!lpddr_memory.stats().hit_cycle_limit,
          "LPDDR6 full init sequence hit cycle limit");
  require(lpddr_memory.stats().mrw == 1 && lpddr_memory.stats().mrr == 1,
          "LPDDR6 init sequence did not execute MRW/MRR");
  require(lpddr_memory.stats().wck_train == 1 && lpddr_memory.stats().dvfs == 1,
          "LPDDR6 init sequence did not execute WCK training and DVFS");
  const IssuedCommand *dvfs =
      find_first(lpddr_memory.issued_commands(), Command::DVFS);
  const IssuedCommand *train =
      find_first(lpddr_memory.issued_commands(), Command::WCKTRAIN);
  require(dvfs != nullptr && train != nullptr && dvfs->cycle < train->cycle,
          "LPDDR6 init sequence should train WCK after DVFS");
  require(
      lpddr_memory.stats().pde == 1 && lpddr_memory.stats().pdx == 1 &&
          lpddr_memory.stats().srefen == 1 && lpddr_memory.stats().srefex == 1,
      "LPDDR6 full init sequence did not execute low-power state transitions");
  auto report =
      hbm_sim::validate_command_trace(lpddr, lpddr_memory.issued_commands());
  require(report.ok(), "validator rejected LPDDR6 full init sequence trace");

  bool rejected_cross_family = false;
  try {
    hbm_sim::TrafficOptions wrong;
    wrong.init_sequence = "lpddr6";
    (void)hbm_sim::generate_control_sequence(hbm4, wrong);
  } catch (const std::invalid_argument&) {
    rejected_cross_family = true;
  }
  require(rejected_cross_family,
          "HBM model accepted an LPDDR initialization sequence");

  rejected_cross_family = false;
  try {
    hbm_sim::TrafficOptions wrong;
    wrong.init_sequence = "hbm4";
    (void)hbm_sim::generate_control_sequence(lpddr, wrong);
  } catch (const std::invalid_argument&) {
    rejected_cross_family = true;
  }
  require(rejected_cross_family,
          "LPDDR model accepted an HBM initialization sequence");
}

void test_hbm4_scoped_timing() {
  DramSpec spec = hbm_sim::make_spec("hbm4");
  Controller controller(spec);

  // 同一 pseudo-channel 内覆盖同 bank-group 和跨 bank-group 两类命令间隔。
  // 另外加入 pseudo_channel=1 的请求，确保不同 pseudo-channel 不被错误地
  // 放进同一个 row/column timing bucket。
  controller.enqueue(make_request(1, RequestType::Read, 0, 0, 0, 10));
  controller.enqueue(make_request(2, RequestType::Read, 0, 0, 1, 10));
  controller.enqueue(make_request(3, RequestType::Read, 0, 1, 0, 10));
  controller.enqueue(make_request(4, RequestType::Read, 0, 1, 1, 10));
  controller.enqueue(make_request(5, RequestType::Read, 1, 0, 0, 10));
  controller.enqueue(make_request(6, RequestType::Read, 1, 1, 0, 10));
  controller.run_until_done(5000);

  require(!controller.stats().hit_cycle_limit, "HBM4 run hit cycle limit");
  require(controller.stats().completed_reads == 6,
          "HBM4 did not complete all reads");

  const auto &trace = controller.issued_commands();
  for (const auto &issued : trace) {
    if (is_column(issued.command)) {
      require(issued.cycle % 2 == 1,
              "HBM4 issued a column command on falling edge");
    } else if (issued.cycle % 2 == 0) {
      require(is_falling_pre(issued.command),
              "HBM4 falling edge issued a non-PRE row command");
    }
  }

  for (std::size_t i = 0; i < trace.size(); i++) {
    for (std::size_t j = i + 1; j < trace.size(); j++) {
      const auto &a = trace[i];
      const auto &b = trace[j];
      if (!same_rank_scope(a, b)) {
        // 不同 scope 的命令不应该互相约束；这里只检查同 rank scope 内的间隔。
        continue;
      }
      Cycle gap = b.cycle - a.cycle;
      bool same_bg = a.decoded.bank_group == b.decoded.bank_group;

      if (is_activate(a.command) && is_activate(b.command)) {
        Cycle need =
            static_cast<Cycle>(same_bg ? spec.timing.nRRDL : spec.timing.nRRDS);
        require(gap >= need, "HBM4 ACT spacing violated scoped nRRD");
      }
      if (is_data(a.command) && is_data(b.command)) {
        Cycle need =
            static_cast<Cycle>(same_bg ? spec.timing.nCCDL : spec.timing.nCCDS);
        require(gap >= need, "HBM4 column spacing violated scoped nCCD");
      }
    }
  }
}

void test_hbm4_refresh_manager() {
  DramSpec spec = hbm_sim::make_spec("hbm4");
  spec.org.channels = 1;
  spec.org.pseudo_channels = 1;
  spec.org.sids = 1;
  spec.org.bank_groups = 1;
  spec.org.banks_per_group = 2;
  set_fixture_density_from_geometry(spec);
  spec.timing.nREFIpb = 16;
  spec.timing.nRFCpb = 2;
  hbm_sim::refresh_timing_constraints(spec);

  Controller controller(spec);
  controller.enqueue(make_request(30, RequestType::Read, 0, 0, 0, 11));
  controller.run_until_done(5000);

  require(!controller.stats().hit_cycle_limit,
          "HBM4 refresh run hit cycle limit");
  require(controller.stats().refresh_batches > 0,
          "HBM4 refresh manager did not seed batches");
  require(controller.stats().refpb > 0,
          "HBM4 refresh manager did not issue REFpb");
  require(controller.stats().maintenance_served ==
              controller.stats().maintenance_requests,
          "HBM4 refresh maintenance requests were not fully served");
}

void test_hbm4_all_bank_refresh_policy() {
  DramSpec spec = hbm_sim::make_spec("hbm4");
  spec.org.channels = 1;
  spec.org.pseudo_channels = 1;
  spec.org.sids = 1;
  spec.org.bank_groups = 1;
  spec.org.banks_per_group = 2;
  set_fixture_density_from_geometry(spec);
  spec.hbm_edge_pairing = false;
  spec.refresh_policy = hbm_sim::MaintenancePolicyKind::AllBank;
  spec.supports_rfm = false;
  spec.timing.nREFI = 1;
  spec.timing.nRFC = 2;
  hbm_sim::refresh_timing_constraints(spec);

  Controller controller(spec);
  controller.enqueue(make_request(35, RequestType::Read, 0, 0, 0, 11));
  controller.run_until_done(5000);

  require(!controller.stats().hit_cycle_limit,
          "HBM4 all-bank refresh run hit cycle limit");
  require(controller.stats().refresh_all_bank_batches > 0,
          "HBM4 all-bank refresh policy did not create all-bank batches");
  require(controller.stats().refab > 0,
          "HBM4 all-bank refresh policy did not issue REFab");
  auto report =
      hbm_sim::validate_command_trace(spec, controller.issued_commands());
  require(report.ok(), "validator rejected valid HBM4 all-bank refresh trace");
}

void test_hbm4_rfm_manager() {
  DramSpec spec = hbm_sim::make_spec("hbm4");
  spec.supports_refresh = false;
  spec.supports_rfm = true;
  spec.rfm_act_threshold = 1;

  Controller controller(spec);
  controller.enqueue(make_request(40, RequestType::Read, 0, 0, 0, 12));
  controller.run_until_done(5000);

  require(!controller.stats().hit_cycle_limit, "HBM4 RFM run hit cycle limit");
  require(controller.stats().rfm_events == 1,
          "HBM4 RFM manager did not create one event");
  require(controller.stats().rfmpb == 1,
          "HBM4 RFM manager did not issue RFMpb");
  require(controller.stats().rfm_decrements == 1,
          "HBM4 RFM did not decrement RAA count on issue");
}

void test_hbm4_all_bank_rfm_policy() {
  DramSpec spec = hbm_sim::make_spec("hbm4");
  spec.org.channels = 1;
  spec.org.pseudo_channels = 1;
  spec.org.sids = 1;
  spec.org.bank_groups = 1;
  spec.org.banks_per_group = 2;
  set_fixture_density_from_geometry(spec);
  spec.supports_refresh = false;
  spec.supports_rfm = true;
  spec.rfm_policy = hbm_sim::MaintenancePolicyKind::AllBank;
  spec.rfm_act_threshold = 1;
  spec.rfm_decrement = 1;
  spec.hbm_edge_pairing = false;
  hbm_sim::refresh_timing_constraints(spec);

  Controller controller(spec);
  controller.enqueue(make_request(45, RequestType::Read, 0, 0, 0, 12));
  controller.run_until_done(5000);

  require(!controller.stats().hit_cycle_limit,
          "HBM4 all-bank RFM run hit cycle limit");
  require(controller.stats().rfm_all_bank_events == 1,
          "HBM4 all-bank RFM did not create one event");
  require(controller.stats().rfmab == 1,
          "HBM4 all-bank RFM did not issue RFMab");
  require(controller.stats().rfmpb == 0,
          "HBM4 all-bank RFM incorrectly issued RFMpb");
  auto report =
      hbm_sim::validate_command_trace(spec, controller.issued_commands());
  require(report.ok(), "validator rejected valid HBM4 all-bank RFM trace");
}

void test_timing_source_override() {
  DramSpec spec = hbm_sim::make_spec("hbm4");
  require(
      spec.timing_table.source_count(
          hbm_sim::TimingValueSource::ResearchDefault) > 0,
      "HBM4 should expose research-default timing sources before calibration");

  hbm_sim::set_timing_source(spec, "nCL", hbm_sim::TimingValueSource::Vendor,
                             "unit-test override");
  hbm_sim::refresh_timing_table(spec);
  bool found_vendor_ncl = false;
  for (const auto &entry : spec.timing_table.entries) {
    if (entry.name == "nCL") {
      found_vendor_ncl = entry.source == hbm_sim::TimingValueSource::Vendor &&
                         !entry.vendor_required_for_numeric;
    }
  }
  require(found_vendor_ncl,
          "timing source override did not update nCL metadata");
}

void test_timing_profile_dimensions() {
  DramSpec spec = hbm_sim::make_spec("hbm4");
  spec.timing_profile = "hbm4_9g_48gb_16hi";
  spec.vendor_profile = "unit_vendor";
  spec.speed_bin_mbps = 9000;
  spec.density_gb = 48;
  spec.stack_height = 16;
  hbm_sim::apply_standard_timing_profile(spec);
  spec.org.rows = 24576;  // 48 Gibit/die with this 16Hi/4-SID geometry.
  hbm_sim::finalize_spec(spec);

  require(spec.data_rate_mbps == 9000,
          "HBM4 timing profile did not apply speed bin");
  require(spec.org.sids == 4,
          "HBM4 timing profile did not derive SID count from stack height");
  require(spec.timing.nRFC > hbm_sim::make_spec("hbm4").timing.nRFC,
          "HBM4 timing profile did not apply density-dependent tRFC");
  require(!hbm_sim::validate_timing_table(spec, true).empty(),
          "a vendor profile name alone must not certify uncalibrated timings");

  DramSpec generic_again = spec;
  generic_again.vendor_profile = "generic";
  hbm_sim::apply_standard_timing_profile(generic_again);
  generic_again.org.rows = 24576;
  hbm_sim::finalize_spec(generic_again);
  require(!hbm_sim::validate_timing_table(generic_again, true).empty(),
          "reapplying a generic profile retained stale vendor timing sources");

  DramSpec hbm3 = hbm_sim::make_spec("hbm3");
  hbm3.timing_profile = "hbm3_jedec_6p4g_16gb_8hi";
  hbm3.speed_bin_mbps = 6400;
  hbm3.density_gb = 16;
  hbm3.stack_height = 8;
  hbm_sim::apply_standard_timing_profile(hbm3);
  hbm_sim::finalize_spec(hbm3);
  require(hbm3.timing.nRFC ==
              hbm_sim::jedec::ns_to_nck(350.0, hbm3.timing.tCK_ps),
          "HBM3 profile did not apply 16Gb/8Hi tRFCab from standard table");
  require(hbm3.timing.nRFCpb ==
              hbm_sim::jedec::ns_to_nck(200.0, hbm3.timing.tCK_ps),
          "HBM3 profile did not apply 16Gb tRFCpb from standard table");
  require(hbm3.timing.nRREFD == 8,
          "HBM3 external-reference baseline changed its 8 nCK tRREFD");

  DramSpec hbm3_16hi = hbm3;
  hbm3_16hi.stack_height = 16;
  hbm_sim::apply_standard_timing_profile(hbm3_16hi);
  hbm3_16hi.org.sids = 4;
  hbm_sim::finalize_spec(hbm3_16hi);
  require(hbm3_16hi.timing.nREFIpb < hbm3.timing.nREFIpb,
          "HBM3 profile did not scale tREFIpb with stack height");

  DramSpec lpddr = hbm_sim::make_spec("lpddr6");
  lpddr.lpddr_dvfs_mode = hbm_sim::LpddrDvfsMode::Low;
  lpddr.lpddr_low_data_rate_mbps = 4267;
  hbm_sim::apply_standard_timing_profile(lpddr);
  hbm_sim::finalize_spec(lpddr);
  require(lpddr.data_rate_mbps == 4267,
          "LPDDR6 low DVFS profile did not change data rate");
  require(lpddr.timing.tCK_ps > hbm_sim::make_spec("lpddr6").timing.tCK_ps,
          "LPDDR6 low DVFS profile did not increase tCK");
  require(lpddr.timing.nRP ==
              27 + hbm_sim::jedec::max_ns_or_nck(20.7, 4, lpddr.timing.tCK_ps),
          "LPDDR6 low DVFS profile did not use JESD209-6 nACU speed band");
  require(lpddr.timing.nREFDB2ACT ==
              hbm_sim::jedec::ns_to_nck(7.5, lpddr.timing.tCK_ps),
          "LPDDR6 profile did not expose REFdb->ACT timing");
  require(lpddr.timing.nREFDB2REFDBS ==
              hbm_sim::jedec::ns_to_nck(47.0, lpddr.timing.tCK_ps),
          "LPDDR6 profile did not expose short REFdb->REFdb timing");
  require(lpddr.timing.nREFDB2REFDBL ==
              hbm_sim::jedec::ns_to_nck(90.0, lpddr.timing.tCK_ps),
          "LPDDR6 profile did not expose long REFdb->REFdb timing");

  DramSpec lpddr_link_eff = hbm_sim::make_spec("lpddr6");
  lpddr_link_eff.lpddr_link_protection = true;
  lpddr_link_eff.lpddr_efficiency_mode = hbm_sim::LpddrEfficiencyMode::Static;
  hbm_sim::apply_standard_timing_profile(lpddr_link_eff);
  hbm_sim::finalize_spec(lpddr_link_eff);
  require(
      lpddr_link_eff.timing.nWR ==
          hbm_sim::jedec::max_ns_or_nck(20.7, 6, lpddr_link_eff.timing.tCK_ps),
      "LPDDR6 link+efficiency profile did not use the correct tWTP branch");
  require(
      lpddr_link_eff.timing.nWTRS ==
          hbm_sim::jedec::max_ns_or_nck(14.1, 6, lpddr_link_eff.timing.tCK_ps),
      "LPDDR6 link+efficiency profile did not use the correct tWTR_S branch");

  DramSpec lpddr8 = hbm_sim::make_spec("lpddr6");
  lpddr8.density_gb = 8;
  hbm_sim::apply_standard_timing_profile(lpddr8);
  lpddr8.org.rows = 32768;
  hbm_sim::finalize_spec(lpddr8);
  require(lpddr8.timing.nRFC ==
              hbm_sim::jedec::ns_to_nck(280.0, lpddr8.timing.tCK_ps),
          "LPDDR6 8Gb/subchannel must use Table 302's 16Gb pair density");

  const auto configured = hbm_sim::config::build_model("hbm3", {
      {"speed_bin_mbps", "6400"}, {"density_gb", "24"}, {"stack_height", "12"},
      {"sids", "3"}, {"rows", "24576"}, {"tck_ps", "625"},
      {"trrd_s_ns", "5"}, {"trrd_l_ns", "7.5"}, {"trfcab_ns", "450"},
      {"trfcpb_ns", "240"}, {"ncl", "28"}, {"timing_override_source", "vendor"}});
  require(configured.timing.nRFC == hbm_sim::jedec::ns_to_nck(450, 625) &&
              configured.timing.nRFCpb == hbm_sim::jedec::ns_to_nck(240, 625),
          "inline timing config did not override refresh durations");
  require(configured.timing.nRRDS == 8 && configured.timing.nRRDL == 12,
          "inline timing config did not accept time-unit aliases");
  bool ncl_vendor = false;
  for (const auto& entry : configured.timing_table.entries)
    if (entry.name == "nCL")
      ncl_vendor = entry.source == hbm_sim::TimingValueSource::Vendor &&
                   !entry.vendor_required_for_numeric;
  require(ncl_vendor, "inline config lost explicit timing source");
}

void test_multi_controller_parallel_channels() {
  DramSpec spec = hbm_sim::make_spec("hbm4");
  spec.org.channels = 2;
  spec.org.pseudo_channels = 1;
  spec.org.sids = 1;
  spec.org.bank_groups = 1;
  spec.org.banks_per_group = 1;
  set_fixture_density_from_geometry(spec);
  spec.supports_refresh = false;
  spec.supports_rfm = false;
  spec.hbm_edge_pairing = false;
  spec.tick_multiplier = 1;
  spec.timing.nRCDRD = 2;
  spec.timing.nRCDWR = 2;
  spec.timing.nRRDS = 1;
  spec.timing.nRRDL = 1;
  spec.timing.nFAW = 8;
  hbm_sim::refresh_timing_constraints(spec);

  Request a = make_request(60, RequestType::Read, 0, 0, 0, 21);
  Request b = make_request(61, RequestType::Read, 0, 0, 0, 21);
  a.decoded.channel = 0;
  b.decoded.channel = 1;

  MemorySystem memory(spec);
  memory.run({a, b}, 200);

  require(!memory.stats().hit_cycle_limit,
          "multi-controller run hit cycle limit");
  require(memory.stats().controller_count == 2,
          "memory system did not create two controllers");
  require(memory.stats().active_controllers == 2,
          "memory system did not use both controllers");
  require(memory.stats().completed_reads == 2,
          "multi-controller run did not complete both reads");

  int same_cycle_reads = 0;
  Cycle first_rd_cycle = 0;
  for (const auto &issued : memory.issued_commands()) {
    if (issued.command != Command::RD) {
      continue;
    }
    if (same_cycle_reads == 0) {
      first_rd_cycle = issued.cycle;
    }
    if (issued.cycle == first_rd_cycle) {
      same_cycle_reads++;
    }
  }
  require(same_cycle_reads == 2,
          "different channel controllers did not issue RD in parallel");

  hbm_sim::MemorySystemOptions options;
  options.channel_mapper = hbm_sim::ChannelMapperKind::RoundRobin;
  MemorySystem rr_memory(spec, options);
  a.decoded.channel = 0;
  b.decoded.channel = 0;
  rr_memory.run({a, b}, 200);
  require(rr_memory.stats().active_controllers == 2,
          "round-robin mapper did not distribute requests");

  hbm_sim::MemorySystemOptions decoded_options;
  decoded_options.channel_mapper = hbm_sim::ChannelMapperKind::Decoded;
  MemorySystem invalid_channel_memory(spec, decoded_options);
  Request invalid_channel = a;
  invalid_channel.decoded.channel = spec.org.channels;
  bool invalid_channel_rejected = false;
  try {
    (void)invalid_channel_memory.try_submit(invalid_channel);
  } catch (const std::out_of_range &) {
    invalid_channel_rejected = true;
  }
  require(invalid_channel_rejected,
          "decoded channel mapper silently clamped an invalid channel");

  MemorySystem invalid_selector_memory(spec);
  Request invalid_selector = a;
  invalid_selector.qos_class = -1;
  bool invalid_selector_rejected = false;
  try {
    (void)invalid_selector_memory.try_submit(invalid_selector);
  } catch (const std::invalid_argument &) {
    invalid_selector_rejected = true;
  }
  require(invalid_selector_rejected,
          "MemorySystem accepted a negative request QoS selector");

  hbm_sim::MemorySystemOptions invalid_interleave_options;
  invalid_interleave_options.stack_count = 2;
  invalid_interleave_options.stack_interleave_bytes =
      static_cast<std::uint64_t>(spec.transaction_bytes());
  bool invalid_interleave_rejected = false;
  try {
    MemorySystem invalid_interleave(spec, invalid_interleave_options);
  } catch (const std::invalid_argument &) {
    invalid_interleave_rejected = true;
  }
  require(invalid_interleave_rejected,
          "MemorySystem accepted a stripe smaller than one host line");

  hbm_sim::MemorySystemOptions blocked_options = invalid_interleave_options;
  blocked_options.stack_mapping = hbm_sim::StackMappingKind::Blocked;
  MemorySystem blocked_memory(spec, blocked_options);
  require(blocked_memory.stack_count() == 2,
          "blocked mapping incorrectly required an interleave-sized stripe");
}

void test_active_six_stack_memory_system_routing_qos_and_stats() {
  DramSpec spec = hbm_sim::make_spec("hbm4");
  spec.org.channels = 2;
  spec.org.pseudo_channels = 1;
  spec.org.sids = 1;
  spec.org.bank_groups = 1;
  spec.org.banks_per_group = 2;
  spec.org.rows = 64;
  spec.org.columns = 16;
  set_fixture_density_from_geometry(spec);
  spec.supports_refresh = false;
  spec.supports_rfm = false;
  hbm_sim::refresh_timing_constraints(spec);

  hbm_sim::MemorySystemOptions options;
  options.stack_count = 6;
  options.stack_interleave_bytes = 64;
  options.stack_ingress_buffer_size = 2;
  options.stack_dispatch_width = 1;
  options.stack_qos_policy = hbm_sim::StackQosPolicy::StrictPriority;
  for (int stack = 0; stack < options.stack_count; stack++) {
    hbm_sim::StorageModelOptions storage;
    storage.stack_id = stack;
    options.stack_memory_images.push_back(
        std::make_shared<hbm_sim::MemoryImage>(spec, 0, storage));
  }

  std::vector<Request> writes;
  for (int stack = 0; stack < options.stack_count; stack++) {
    Request req;
    req.id = 2000 + static_cast<std::uint64_t>(stack);
    req.type = RequestType::Write;
    req.address = 0x100;
    req.target_stack = stack;
    req.has_explicit_stack = true;
    req.qos_class = stack == 5 ? 7 : 0;
    req.payload.assign(32, static_cast<std::uint8_t>(0x20 + stack));
    req.has_payload = true;
    writes.push_back(req);
  }
  Request high = writes.front();
  high.id = 2100;
  high.address = 0x200;
  high.qos_class = 9;
  high.payload.assign(32, 0xa5);
  writes.push_back(high);
  Request extra = writes.front();
  extra.id = 2101;
  extra.address = 0x300;
  extra.payload.assign(32, 0x5a);
  writes.push_back(extra);

  MemorySystem memory(spec, options);
  memory.set_response_delivery_mode(hbm_sim::ResponseDeliveryMode::Both);
  memory.run(writes, 2000);
  require(!memory.stats().hit_cycle_limit,
          "active six-stack run hit cycle limit");
  require(memory.stack_count() == 6 && memory.controller_count() == 12,
          "active six-stack system did not create stack_count * channels "
          "controllers");
  require(memory.stats().active_stacks == 6 && memory.stats().stack_count == 6,
          "active six-stack stats did not report all stacks");
  require(memory.stats().storage_stacks_touched == 6,
          "active six-stack storage stats lost stack identity");
  require(memory.stats().stack_ingress_stall_cycles > 0,
          "one-entry stack ingress did not expose backpressure");
  require(memory.stats().qos_priority_dispatches >= 2,
          "stack ingress QoS did not account the priority request");
  require(memory.per_stack_stats().size() == 6,
          "active six-stack system did not expose per-stack stats");

  for (int stack = 0; stack < options.stack_count; stack++) {
    bool initialized = false;
    hbm_sim::ByteVector data =
        options.stack_memory_images[static_cast<std::size_t>(stack)]->read(
            0x100, 32, &initialized);
    require(initialized &&
                data == writes[static_cast<std::size_t>(stack)].payload,
            "active multi-stack routing aliased stack-local payloads");
    const std::uint64_t expected_writes = stack == 0 ? 3 : 1;
    require(memory.per_stack_stats()[static_cast<std::size_t>(stack)]
                    .completed_writes == expected_writes,
            "per-stack completion statistics are incorrect");
  }

  std::vector<bool> traced(6, false);
  std::uint64_t first_stack0_request = 0;
  for (const auto &command : memory.issued_commands()) {
    require(command.stack_id >= 0 && command.stack_id < 6,
            "multi-stack command trace contains invalid stack_id");
    traced[static_cast<std::size_t>(command.stack_id)] = true;
    if (command.stack_id == 0 && first_stack0_request == 0) {
      first_stack0_request = command.request_id;
    }
  }
  require(std::all_of(traced.begin(), traced.end(),
                      [](bool value) { return value; }),
          "multi-stack command trace did not cover every stack");
  require(first_stack0_request == high.id,
          "strict-priority stack QoS did not dispatch the high-priority "
          "request first");

  std::vector<bool> response_stacks(6, false);
  std::size_t response_count = 0;
  while (memory.has_transaction_response()) {
    const hbm_sim::TransactionResponse response =
        memory.pop_transaction_response();
    require(response.stack >= 0 && response.stack < 6 &&
                response.channel >= 0 && response.channel < 2 &&
                response.type == RequestType::Write,
            "multi-stack async response lost its execution endpoint");
    response_stacks[static_cast<std::size_t>(response.stack)] = true;
    response_count++;
  }
  require(
      response_count == writes.size() &&
          std::all_of(response_stacks.begin(), response_stacks.end(),
                      [](bool seen) { return seen; }),
      "multi-stack async response did not cover every accepted request/stack");

  hbm_sim::StackAddressMapper mapper(6, 64, spec.addressable_capacity_bytes());
  for (int stack = 0; stack < 6; stack++) {
    const hbm_sim::Address system = mapper.encode(stack, 0x180);
    const hbm_sim::StackAddress decoded = mapper.decode(system);
    require(decoded.stack == stack && decoded.local_address == 0x180,
            "global-to-stack address mapping failed round trip");
  }
  hbm_sim::StackAddressMapper blocked(6, 64, 1024,
                                      hbm_sim::StackMappingKind::Blocked);
  for (int stack = 0; stack < 6; stack++) {
    const hbm_sim::Address system = blocked.encode(stack, 0x180);
    const hbm_sim::StackAddress decoded = blocked.decode(system);
    require(decoded.stack == stack && decoded.local_address == 0x180,
            "blocked global-to-stack address mapping failed round trip");
  }

  bool blocked_overflow_rejected = false;
  try {
    hbm_sim::StackAddressMapper huge_blocked(
        2, 64, std::numeric_limits<hbm_sim::Address>::max(),
        hbm_sim::StackMappingKind::Blocked);
    (void)huge_blocked.encode(
        1, std::numeric_limits<hbm_sim::Address>::max() - 1);
  } catch (const std::overflow_error &) {
    blocked_overflow_rejected = true;
  }
  require(blocked_overflow_rejected,
          "blocked stack mapping silently wrapped the system address");

  bool interleaved_overflow_rejected = false;
  try {
    hbm_sim::StackAddressMapper huge_interleaved(
        2, 1, 0, hbm_sim::StackMappingKind::Interleaved);
    (void)huge_interleaved.encode(
        1, std::numeric_limits<hbm_sim::Address>::max());
  } catch (const std::overflow_error &) {
    interleaved_overflow_rejected = true;
  }
  require(interleaved_overflow_rejected,
          "interleaved stack mapping silently wrapped the system address");

  hbm_sim::StackAddressMapper single(1, 64, 1024);
  bool single_decode_rejected = false;
  try {
    (void)single.decode(1024);
  } catch (const std::out_of_range &) {
    single_decode_rejected = true;
  }
  require(single_decode_rejected,
          "single-stack decode accepted an address at capacity");
  bool single_encode_rejected = false;
  try {
    (void)single.encode(0, 1024);
  } catch (const std::out_of_range &) {
    single_encode_rejected = true;
  }
  require(single_encode_rejected,
          "single-stack encode accepted an address at capacity");
}

void test_write_forward_and_coalesce() {
  DramSpec spec = hbm_sim::make_spec("hbm4");
  spec.org.channels = 1;
  spec.org.pseudo_channels = 1;
  spec.org.sids = 1;
  spec.org.bank_groups = 1;
  spec.org.banks_per_group = 1;
  set_fixture_density_from_geometry(spec);
  spec.supports_refresh = false;
  spec.supports_rfm = false;
  hbm_sim::refresh_timing_constraints(spec);

  Request w0 = make_request(70, RequestType::Write, 0, 0, 0, 31);
  Request w1 = w0;
  w1.id = 71;
  Request r0 = w0;
  r0.id = 72;
  r0.type = RequestType::Read;

  Controller controller(spec);
  require(controller.enqueue(w0), "first write enqueue failed");
  require(controller.enqueue(w1), "coalesced write enqueue failed");
  require(controller.enqueue(r0), "forwarded read enqueue failed");
  controller.run_until_done(5000);

  require(!controller.stats().hit_cycle_limit,
          "write forwarding run hit cycle limit");
  require(controller.stats().write_coalesces == 1,
          "duplicate write was not coalesced");
  require(controller.stats().read_forwards == 1,
          "read did not forward from write buffer");
  require(controller.stats().rd == 0,
          "forwarded read incorrectly issued DRAM RD");
  require(controller.stats().wr == 1,
          "coalesced writes should issue one DRAM WR");
  require(controller.stats().completed_reads == 1,
          "forwarded read did not complete");
  require(controller.stats().completed_writes == 2,
          "coalesced writes did not both count complete");
  int response_count = 0;
  int forwarded_responses = 0;
  int coalesced_responses = 0;
  while (controller.has_response()) {
    const hbm_sim::TransactionResponse response = controller.pop_response();
    response_count++;
    if (response.forwarded) {
      forwarded_responses++;
      require(response.type == RequestType::Read && !response.data.empty(),
              "forwarded response did not carry the read data snapshot");
    }
    if (response.coalesced) {
      coalesced_responses++;
      require(response.type == RequestType::Write,
              "coalesced response was not a write acknowledgement");
    }
  }
  require(response_count == 3 && forwarded_responses == 1 &&
              coalesced_responses == 1,
          "Controller did not return one response for each accepted bypass "
          "request");
}

void test_closed_page_row_policy() {
  DramSpec spec = hbm_sim::make_spec("hbm4");
  spec.org.channels = 1;
  spec.org.pseudo_channels = 1;
  spec.org.sids = 1;
  spec.org.bank_groups = 1;
  spec.org.banks_per_group = 1;
  set_fixture_density_from_geometry(spec);
  spec.supports_refresh = false;
  spec.supports_rfm = false;
  spec.hbm_edge_pairing = false;
  spec.tick_multiplier = 1;
  hbm_sim::refresh_timing_constraints(spec);

  hbm_sim::ControllerOptions options;
  options.row_policy = hbm_sim::RowPolicyKind::ClosedPage;
  Controller controller(spec, options);
  controller.enqueue(make_request(80, RequestType::Read, 0, 0, 0, 41));
  controller.enqueue(make_request(81, RequestType::Read, 0, 0, 0, 41));
  controller.run_until_done(5000);

  require(!controller.stats().hit_cycle_limit,
          "closed-page run hit cycle limit");
  require(controller.stats().completed_reads == 2,
          "closed-page did not complete both reads");
  require(controller.stats().rda == 2 && controller.stats().rd == 0,
          "closed-page policy did not use RDA for reads");
  require(controller.stats().act >= 2,
          "closed-page policy did not close row after RDA");
}

void test_closed_cap_row_policy() {
  DramSpec spec = hbm_sim::make_spec("hbm4");
  spec.org.channels = 1;
  spec.org.pseudo_channels = 1;
  spec.org.sids = 1;
  spec.org.bank_groups = 1;
  spec.org.banks_per_group = 1;
  set_fixture_density_from_geometry(spec);
  spec.supports_refresh = false;
  spec.supports_rfm = false;
  spec.hbm_edge_pairing = false;
  spec.tick_multiplier = 1;
  hbm_sim::refresh_timing_constraints(spec);

  hbm_sim::ControllerOptions options;
  options.row_policy = hbm_sim::RowPolicyKind::ClosedCap;
  options.row_policy_cap = 1;
  Controller controller(spec, options);
  controller.enqueue(make_request(82, RequestType::Read, 0, 0, 0, 42));
  controller.enqueue(make_request(83, RequestType::Read, 0, 0, 0, 42));
  controller.run_until_done(5000);

  require(!controller.stats().hit_cycle_limit,
          "closed-cap run hit cycle limit");
  require(controller.stats().completed_reads == 2,
          "closed-cap did not complete both reads");
  require(controller.stats().row_policy_ap_upgrades >= 1,
          "closed-cap policy did not upgrade RD to RDA after cap");
  require(controller.stats().rda >= 1, "closed-cap policy did not issue RDA");
}

void test_maintenance_progress_dependencies() {
  DramSpec spec = hbm_sim::make_spec("hbm4");
  spec.org.channels = spec.org.pseudo_channels = spec.org.sids = 1;
  spec.org.bank_groups = spec.org.banks_per_group = 1;
  set_fixture_density_from_geometry(spec);
  spec.supports_refresh = false;
  spec.supports_rfm = false;  // explicit maintenance below, no automatic stream
  spec.hbm_edge_pairing = false;
  spec.tick_multiplier = 1;
  hbm_sim::finalize_spec(spec);

  // Two queued PRE requests become redundant after the first closes the bank.
  Controller pre(spec);
  require(pre.enqueue(make_request(9100, RequestType::Read, 0, 0, 0, 1)), "enqueue PRE setup read");
  pre.run_until_done(5000);
  Request close = make_request(9101, RequestType::Maintenance, 0, 0, 0, 1);
  close.next = Command::PREPB;
  require(pre.enqueue(close), "enqueue first PRE");
  ++close.id;
  require(pre.enqueue(close), "enqueue redundant PRE");
  pre.run_until_done(5000);
  require(pre.done(), "redundant PRE to closed bank blocked priority queue");
  require(hbm_sim::validate_command_trace(spec, pre.issued_commands()).ok(),
          "retiring redundant PRE must not emit an illegal command");

  // A write-drain watermark prefers the younger write, but the older read must
  // complete before this write acquires an active bank that RFM needs to close.
  hbm_sim::ControllerOptions options;
  options.write_buffer_size = 1;
  Controller dependency(spec, options);
  auto read = make_request(9200, RequestType::Read, 0, 0, 0, 2);
  auto write = read;
  write.id = 9201;
  write.type = RequestType::Write;
  require(dependency.enqueue(read) && dependency.enqueue(write), "enqueue ordered read/write");
  for (int tick = 0; tick < 100 && dependency.issued_commands().empty(); ++tick)
    dependency.tick();
  require(!dependency.issued_commands().empty(), "dependency must make initial progress");
  auto maintenance = make_request(9202, RequestType::Maintenance, 0, 0, 0, 2);
  maintenance.next = Command::RFMPB;
  require(dependency.enqueue(maintenance), "enqueue RFM");
  dependency.run_until_done(5000);
  require(dependency.done() && dependency.stats().completed_reads == 1 &&
              dependency.stats().completed_writes == 1,
          "RFM/active-write/older-read circular dependency prevented completion");
  require(hbm_sim::validate_command_trace(spec, dependency.issued_commands()).ok(),
          "maintenance dependency progress violated command timing");
}

void test_lpddr6_refdb_counter_boundaries() {
  auto spec = hbm_sim::make_spec("lpddr6");
  spec.org.channels = 1;
  spec.org.ranks = 2;
  set_fixture_density_from_geometry(spec);
  const Cycle scale = spec.tick_multiplier;
  const Cycle short_gap = spec.timing.nREFDB2REFDBS * scale;
  const Cycle long_gap = spec.timing.nREFDB2REFDBL * scale;
  const int pairs = spec.org.bank_groups * spec.org.banks_per_group / 2;
  hbm_sim::TimingEngine engine(spec);
  std::vector<IssuedCommand> trace;
  Cycle time = 1000;
  auto address_for = [&](int pair) {
    DecodedAddress d;
    d.bank_group = (pair / spec.org.banks_per_group) * 2;
    d.bank = pair % spec.org.banks_per_group;
    return d;
  };
  // Two complete sweeps: the 8th->9th interval must be L, all others S.
  for (int i = 0; i < 2 * pairs; ++i) {
    auto d = address_for(i % pairs);
    if (i > 0) {
      require(!engine.constraint_ready(spec, d, Command::REFDB, time - 1),
              "REFdb accepted one tick before counter-dependent boundary");
      require(engine.constraint_ready_at(spec, d, Command::REFDB) == time,
              "REFdb ready_at disagrees with refresh-counter boundary");
    }
    require(engine.constraint_ready(spec, d, Command::REFDB, time),
            "REFdb rejected at counter-dependent boundary");
    trace.emplace_back(time, i + 1, Command::REFDB, hbm_sim::BusClass::Unified, d);
    engine.apply_constraints(spec, d, Command::REFDB, time);
    auto other_pc = d;
    other_pc.pseudo_channel = 1;
    auto other_rank = d;
    other_rank.rank = 1;
    require(engine.constraint_ready(spec, other_pc, Command::REFDB, time) &&
                engine.constraint_ready(spec, other_rank, Command::REFDB, time),
            "REFdb counters leaked across subchannels or ranks");
    time += (i + 1) % pairs == 0 ? long_gap : short_gap;
  }
  require(hbm_sim::validate_command_trace(spec, trace).ok(),
          "validator rejected legal REFdb short/long sweeps");
  for (int bad_index : {1, pairs}) {
    auto bad = trace;
    --bad[bad_index].cycle;
    require(!hbm_sim::validate_command_trace(spec, bad).ok(),
            "validator missed an early REFdb S/L transition");
  }
  // REFab synchronizes a partial sweep; the following REFdb starts at count 0.
  trace.resize(3);
  engine.reset(spec);
  for (const auto &event : trace)
    engine.apply_constraints(spec, event.decoded, event.command, event.cycle);
  time = trace.back().cycle + spec.timing.nRFCpb * scale;
  const auto d = address_for(0);
  trace.emplace_back(time, 100, Command::REFAB, hbm_sim::BusClass::Unified, d);
  engine.apply_constraints(spec, d, Command::REFAB, time);
  time += spec.timing.nRFC * scale;
  for (int i = 0; i < pairs; ++i) {
    auto target = address_for(i);
    require(engine.constraint_ready(spec, target, Command::REFDB, time),
            "REFab did not reset the partial REFdb counter");
    trace.emplace_back(time, 101 + i, Command::REFDB, hbm_sim::BusClass::Unified, target);
    engine.apply_constraints(spec, target, Command::REFDB, time);
    time += short_gap;
  }
  require(hbm_sim::validate_command_trace(spec, trace).ok(),
          "validator failed to synchronize REFdb counter on REFab");
  // Both members, including the implicit partner, retain tRFCdb recovery.
  engine.reset(spec);
  engine.apply_constraints(spec, d, Command::REFDB, 1000);
  const auto partner = hbm_sim::lpddr_refdb_partner(spec, d);
  const Cycle recovery = 1000 + spec.timing.nRFCpb * scale;
  require(!engine.constraint_ready(spec, partner, Command::ACT1, recovery - 1) &&
              engine.constraint_ready(spec, partner, Command::ACT1, recovery),
          "implicit REFdb partner lost tRFCdb recovery");
  require(!engine.constraint_ready(spec, partner, Command::REFDB, recovery + long_gap),
          "REFdb repeated a bank pair before the sweep completed");
  std::vector<IssuedCommand> duplicate{
      {1000, 1, Command::REFDB, hbm_sim::BusClass::Unified, d},
      {recovery + long_gap, 2, Command::REFDB, hbm_sim::BusClass::Unified, partner}};
  require(!hbm_sim::validate_command_trace(spec, duplicate).ok(),
          "validator accepted a repeated bank despite a long enough time gap");
  // SRX resets every subchannel/rank in this model's channel-level control domain.
  auto other_pc = d;
  other_pc.pseudo_channel = 1;
  auto other_rank = d;
  other_rank.rank = 1;
  std::vector<IssuedCommand> isolated;
  for (int sweep_step = 0; sweep_step < 2; ++sweep_step) {
    int offset = 0;
    for (auto target : {d, other_pc, other_rank}) {
      target.bank = sweep_step;
      isolated.emplace_back(1000 + sweep_step * short_gap + offset,
                            10 + sweep_step * 3 + offset,
                            Command::REFDB, hbm_sim::BusClass::Unified, target);
      ++offset;
    }
  }
  require(hbm_sim::validate_command_trace(spec, isolated).ok(),
          "validator shared a REFdb counter across subchannels/ranks");
  engine.apply_constraints(spec, other_pc, Command::REFDB, 1000);
  engine.apply_constraints(spec, other_rank, Command::REFDB, 1000);
  const Cycle exit = recovery + 10000;
  engine.apply_constraints(spec, d, Command::SREFEX, exit);
  for (const auto &target : {d, other_pc, other_rank})
    require(engine.constraint_ready(spec, target, Command::REFDB, exit + 10000),
            "SREFEX did not synchronize all channel-local REFdb counters");
  std::vector<IssuedCommand> self_refresh{
      {1000, 1, Command::REFDB, hbm_sim::BusClass::Unified, d},
      {1001, 5, Command::REFDB, hbm_sim::BusClass::Unified, other_pc},
      {1002, 6, Command::REFDB, hbm_sim::BusClass::Unified, other_rank},
      {exit, 2, Command::SREFEN, hbm_sim::BusClass::Unified, d},
      {exit + 10000, 3, Command::SREFEX, hbm_sim::BusClass::Unified, d},
      {exit + 20000, 4, Command::REFDB, hbm_sim::BusClass::Unified, d},
      {exit + 20001, 7, Command::REFDB, hbm_sim::BusClass::Unified, other_pc},
      {exit + 20002, 8, Command::REFDB, hbm_sim::BusClass::Unified, other_rank}};
  require(hbm_sim::validate_command_trace(spec, self_refresh).ok(),
          "validator failed to reset REFdb state at self-refresh exit");

  engine.reset(spec);
  engine.apply_constraints(spec, partner, Command::PREPB, 1000);
  const Cycle precharge_ready = 1000 + spec.timing.nRP * scale;
  require(!engine.constraint_ready(spec, d, Command::REFDB, precharge_ready - 1) &&
              engine.constraint_ready(spec, d, Command::REFDB, precharge_ready) &&
              engine.constraint_ready_at(spec, d, Command::REFDB) == precharge_ready,
          "REFdb ignored its implicit partner's precharge recovery");
  const Cycle act2 = 100 + spec.timing.nAADMin * scale;
  const Cycle precharge = act2 + spec.timing.nRAS * scale + 100;
  std::vector<IssuedCommand> precharged{
      {100, 1, Command::ACT1, hbm_sim::BusClass::Unified, partner},
      {act2, 1, Command::ACT2, hbm_sim::BusClass::Unified, partner},
      {precharge, 2, Command::PREPB, hbm_sim::BusClass::Unified, partner},
      {precharge + spec.timing.nRP * scale, 3, Command::REFDB, hbm_sim::BusClass::Unified, d}};
  require(hbm_sim::validate_command_trace(spec, precharged).ok(),
          "validator rejected exact implicit-partner precharge boundary");
  --precharged.back().cycle;
  require(!hbm_sim::validate_command_trace(spec, precharged).ok(),
          "validator missed early REFdb after implicit-partner precharge");

  spec.supports_refresh = false;
  spec.supports_rfm = false;
  Controller controller(spec);
  for (int i = 0; i < 2 * pairs; ++i) {
    auto target = address_for(i % pairs);
    auto request = make_request(200 + i, RequestType::Maintenance, 0,
                                target.bank_group, target.bank, 0);
    request.next = Command::REFDB;
    require(controller.enqueue(request), "failed to enqueue REFdb sweep");
  }
  controller.run_until_done(20000);
  require(controller.done() && controller.stats().refdb == static_cast<std::uint64_t>(2 * pairs),
          "online controller failed to complete two legal REFdb sweeps");
  require(hbm_sim::validate_command_trace(spec, controller.issued_commands()).ok(),
          "online REFdb schedule failed independent validation");
}

void test_lpddr6_refresh_manager() {
  DramSpec spec = hbm_sim::make_spec("lpddr6");
  spec.org.channels = 1;
  spec.org.pseudo_channels = 1;
  spec.org.sids = 1;
  spec.org.bank_groups = 2;
  spec.org.banks_per_group = 2;
  set_fixture_density_from_geometry(spec);
  spec.timing.nREFIpb = 32;
  spec.timing.nRFCpb = 2;
  spec.timing.nRREFD = 1;
  spec.timing.nREFDB2ACT = 1;
  spec.timing.nREFDB2REFDBS = 1;
  spec.timing.nREFDB2REFDBL = 1;
  hbm_sim::refresh_timing_constraints(spec);

  Controller controller(spec);
  controller.enqueue(make_request(50, RequestType::Read, 0, 0, 0, 13));
  controller.run_until_done(5000);

  require(!controller.stats().hit_cycle_limit,
          "LPDDR6 refresh run hit cycle limit");
  require(controller.stats().refresh_batches > 0,
          "LPDDR6 refresh manager did not seed batches");
  require(controller.stats().refdb > 0,
          "LPDDR6 refresh manager did not issue REFdb");
  require(controller.stats().refpb == 0,
          "LPDDR6 refresh manager incorrectly used REFpb");
}

void test_lpddr6_dual_bank_refresh_pair() {
  DramSpec spec = hbm_sim::make_spec("lpddr6");
  spec.org.channels = 1;
  spec.org.pseudo_channels = 1;
  spec.org.sids = 1;
  spec.org.bank_groups = 2;
  spec.org.banks_per_group = 2;
  set_fixture_density_from_geometry(spec);
  spec.supports_rfm = false;
  spec.timing.nREFIpb = 32;
  spec.timing.nRFCpb = 8;
  spec.timing.nRREFD = 1;
  spec.timing.nREFDB2ACT = 1;
  spec.timing.nREFDB2REFDBS = 1;
  spec.timing.nREFDB2REFDBL = 1;
  hbm_sim::refresh_timing_constraints(spec);

  Controller controller(spec);
  controller.enqueue(make_request(90, RequestType::Read, 0, 0, 1, 52));
  controller.run_until_done(2000);

  require(!controller.stats().hit_cycle_limit,
          "LPDDR6 REFdb pair run hit cycle limit");
  require(controller.stats().refdb > 0, "LPDDR6 did not issue REFdb");
  require(controller.stats().preab > 0 || controller.stats().prepb > 0,
          "LPDDR6 REFdb did not close a busy dual-bank pair before refresh");

  DecodedAddress primary;
  primary.bank_group = 0;
  primary.bank = 1;
  DecodedAddress partner = hbm_sim::lpddr_refdb_partner(spec, primary);
  require(partner.bank_group == 1 && partner.bank == primary.bank,
          "REFdb pair table must keep BA and select adjacent dBG");

  hbm_sim::RefreshManager refresh;
  refresh.reset(spec, 0);
  auto first = refresh.tick(spec, 32, false, false);
  auto second = refresh.tick(spec, 64, false, false);
  require(
      !first.commands.empty() && !second.commands.empty() &&
          first.commands.front().decoded.bank_group == 0 &&
          second.commands.front().decoded.bank_group == 0 &&
          first.commands.front().decoded.bank == 0 &&
          second.commands.front().decoded.bank == 1,
      "REFdb rotation did not enumerate each adjacent-BG pair exactly once");
}

void test_refdb_does_not_precharge_other_pseudo_channel() {
  DramSpec spec = hbm_sim::make_spec("lpddr6");
  spec.org.channels = 1;
  spec.org.pseudo_channels = 2;
  spec.org.sids = 1;
  spec.org.ranks = 1;
  spec.org.bank_groups = 2;
  spec.org.banks_per_group = 2;
  set_fixture_density_from_geometry(spec);
  spec.supports_refresh = false;
  spec.supports_rfm = false;
  spec.timing.nREFDB2ACT = 1;
  spec.timing.nREFDB2REFDBS = 1;
  spec.timing.nREFDB2REFDBL = 1;
  hbm_sim::refresh_timing_constraints(spec);

  Controller controller(spec);
  Request open_pc0 = make_request(920, RequestType::Write, 0, 0, 0, 7);
  open_pc0.payload = hbm_sim::ByteVector(32, 0x6b);
  open_pc0.has_payload = true;
  require(controller.enqueue(open_pc0),
          "cross-PC REFdb setup write was not accepted");
  controller.run_until_done(5000);

  Request refresh = make_request(921, RequestType::Maintenance, 1, 0, 0, 0);
  refresh.next = Command::REFDB;
  require(controller.enqueue(refresh), "cross-PC REFdb was not accepted");
  controller.run_until_done(6000);

  require(!controller.stats().hit_cycle_limit && controller.stats().refdb == 1,
          "REFdb was blocked by an unrelated pseudo-channel");
  require(
      controller.stats().preab == 0,
      "REFdb repeatedly issued rank-local PREab because another PC was busy");
}

void test_lpddr6_prac_rfm_manager() {
  DramSpec spec = hbm_sim::make_spec("lpddr6");
  spec.supports_refresh = false;
  spec.supports_rfm = true;
  spec.rfm_act_threshold = 1;
  spec.rfm_decrement = 1;
  hbm_sim::refresh_timing_constraints(spec);

  Controller controller(spec);
  controller.enqueue(make_request(93, RequestType::Read, 0, 0, 0, 54));
  controller.run_until_done(5000);

  require(!controller.stats().hit_cycle_limit,
          "LPDDR6 PRAC/RFM run hit cycle limit");
  require(controller.stats().rfm_events == 1,
          "LPDDR6 PRAC did not create one RFM event");
  require(controller.stats().rfmpb == 1, "LPDDR6 PRAC did not issue RFMpb");
  require(controller.stats().rfm_decrements == 1,
          "LPDDR6 RFM did not decrement activation count");
}

void test_lpddr6_cas_read_sequence() {
  DramSpec spec = hbm_sim::make_spec("lpddr6");
  Controller controller(spec);
  // 单个读请求应走完整路径：ACT1 -> ACT2 -> CAS_RD -> RD。
  controller.enqueue(make_request(10, RequestType::Read, 0, 0, 0, 5));
  controller.run_until_done(2000);

  require(!controller.stats().hit_cycle_limit, "LPDDR6 read hit cycle limit");
  require(controller.stats().act1 == 1 && controller.stats().act2 == 1,
          "LPDDR6 read did not use split activate");
  require(controller.stats().cas_rd == 1 && controller.stats().rd == 1,
          "LPDDR6 read did not use CAS_RD then RD");

  const auto &trace = controller.issued_commands();
  const IssuedCommand *act1 = find_first(trace, Command::ACT1);
  const IssuedCommand *act2 = find_first(trace, Command::ACT2);
  const IssuedCommand *cas = find_first(trace, Command::CASRD);
  const IssuedCommand *rd = find_first(trace, Command::RD);
  require(act1 != nullptr && act2 != nullptr && cas != nullptr && rd != nullptr,
          "LPDDR6 read trace missing command");
  require(act1->cycle < act2->cycle && act2->cycle < cas->cycle &&
              cas->cycle < rd->cycle,
          "LPDDR6 read command order is wrong");
  require(rd->cycle - cas->cycle >= static_cast<Cycle>(spec.timing.nWCK2CK),
          "LPDDR6 read violated WCK-to-data spacing");
  auto report = hbm_sim::validate_command_trace(spec, trace);
  require(report.ok(), "validator rejected valid LPDDR6 CAS/RD trace");
  require(report.wck_window_checks > 0,
          "validator did not report LPDDR6 WCK window coverage");
}

void test_lpddr6_wck_sync_skip() {
  DramSpec spec = hbm_sim::make_spec("lpddr6");
  spec.supports_refresh = false;
  spec.supports_rfm = false;
  hbm_sim::refresh_timing_constraints(spec);

  Controller controller(spec);
  controller.enqueue(make_request(91, RequestType::Read, 0, 0, 0, 53));
  controller.enqueue(make_request(92, RequestType::Read, 0, 0, 0, 53));
  controller.run_until_done(5000);

  require(!controller.stats().hit_cycle_limit,
          "LPDDR6 WCK skip run hit cycle limit");
  require(controller.stats().completed_reads == 2,
          "LPDDR6 WCK skip did not complete reads");
  require(controller.stats().wck_syncs == 1,
          "LPDDR6 should need one WCK sync for back-to-back reads");
  require(controller.stats().wck_sync_skips == 1,
          "LPDDR6 did not skip CAS while WCK sync state was active");
}

void test_lpddr6_wck_always_on_mode() {
  DramSpec spec = hbm_sim::make_spec("lpddr6");
  spec.supports_refresh = false;
  spec.supports_rfm = false;
  spec.lpddr_wck_mode = hbm_sim::LpddrWckMode::AlwaysOn;
  hbm_sim::apply_standard_timing_profile(spec);
  hbm_sim::finalize_spec(spec);

  Controller controller(spec);
  controller.enqueue(make_request(94, RequestType::Read, 0, 0, 0, 55));
  controller.run_until_done(5000);

  require(!controller.stats().hit_cycle_limit,
          "LPDDR6 always-on WCK run hit cycle limit");
  require(controller.stats().cas_rd == 0,
          "LPDDR6 always-on WCK should not issue CAS_RD");
  require(controller.stats().rd == 1,
          "LPDDR6 always-on WCK should still issue RD");
}

void test_lpddr6_ca_parity_command_overhead() {
  DramSpec spec = hbm_sim::make_spec("lpddr6");
  spec.supports_refresh = false;
  spec.supports_rfm = false;
  spec.lpddr_wck_mode = hbm_sim::LpddrWckMode::AlwaysOn;
  spec.lpddr_ca_parity_enabled = true;
  spec.lpddr_ca_parity_bits_per_command = 1;
  hbm_sim::apply_standard_timing_profile(spec);
  hbm_sim::finalize_spec(spec);

  Controller controller(spec);
  controller.enqueue(make_request(95, RequestType::Read, 0, 0, 0, 56));
  controller.run_until_done(5000);

  require(!controller.stats().hit_cycle_limit,
          "LPDDR6 CA parity run hit cycle limit");
  require(controller.stats().completed_reads == 1,
          "LPDDR6 CA parity run did not complete read");
  require(controller.stats().interface_command_bits ==
              controller.issued_commands().size(),
          "CA parity should add one interface command bit per issued LPDDR "
          "command");
  require(
      controller.stats().interface_overhead_bits >=
          controller.stats().interface_command_bits,
      "CA parity command bits must be included in total interface overhead");
}

void test_refresh_credit_and_low_power() {
  DramSpec refresh_spec = hbm_sim::make_spec("hbm4");
  refresh_spec.org.channels = 1;
  refresh_spec.org.pseudo_channels = 1;
  refresh_spec.org.sids = 1;
  refresh_spec.org.bank_groups = 1;
  refresh_spec.org.banks_per_group = 1;
  set_fixture_density_from_geometry(refresh_spec);
  refresh_spec.supports_rfm = false;
  refresh_spec.hbm_edge_pairing = false;
  refresh_spec.tick_multiplier = 1;
  refresh_spec.timing.nREFIpb = 2;
  refresh_spec.timing.nRFCpb = 2;
  refresh_spec.refresh_postpone_limit = 2;
  refresh_spec.refresh_credit_limit = 2;
  hbm_sim::refresh_timing_constraints(refresh_spec);

  Controller refresh_controller(refresh_spec);
  for (int i = 0; i < 8; i++) {
    refresh_controller.enqueue(
        make_request(200 + i, RequestType::Read, 0, 0, 0, i));
  }
  refresh_controller.run_until_done(5000);
  require(refresh_controller.stats().refresh_postpones > 0,
          "refresh manager did not postpone refresh under ordinary traffic "
          "pressure");
  require(refresh_controller.stats().refresh_credit_peak > 0,
          "refresh manager did not expose positive refresh credit");

  DramSpec low_power_spec = hbm_sim::make_spec("hbm4");
  low_power_spec.org.channels = 1;
  low_power_spec.org.pseudo_channels = 1;
  low_power_spec.org.sids = 1;
  low_power_spec.org.bank_groups = 1;
  low_power_spec.org.banks_per_group = 1;
  set_fixture_density_from_geometry(low_power_spec);
  low_power_spec.supports_refresh = false;
  low_power_spec.supports_rfm = false;
  low_power_spec.hbm_edge_pairing = false;
  low_power_spec.tick_multiplier = 1;
  low_power_spec.low_power_mode = hbm_sim::LowPowerMode::PowerDown;
  low_power_spec.low_power_entry_cycles = 2;
  low_power_spec.low_power_exit_cycles = 5;
  hbm_sim::refresh_timing_constraints(low_power_spec);

  Request first = make_request(300, RequestType::Read, 0, 0, 0, 1);
  Request second = make_request(301, RequestType::Read, 0, 0, 0, 2);
  second.inject_cycle = 200;
  Controller low_power_controller(low_power_spec);
  low_power_controller.run({first, second}, 5000);
  require(low_power_controller.stats().low_power_entries > 0,
          "controller did not enter low-power state during idle gap");
  require(low_power_controller.stats().low_power_exits > 0,
          "controller did not exit low-power state when new request arrived");
  require(low_power_controller.stats().low_power_exit_blocked_cycles > 0,
          "low-power exit latency did not block command issue");
}

void test_refresh_credit_conservation_and_rank_rotation() {
  DramSpec spec = hbm_sim::make_spec("hbm4");
  spec.org.channels = 1;
  spec.org.pseudo_channels = 1;
  spec.org.sids = 1;
  spec.org.ranks = 1;
  spec.org.bank_groups = 1;
  spec.org.banks_per_group = 2;
  set_fixture_density_from_geometry(spec);
  spec.tick_multiplier = 1;
  spec.timing.nREFIpb = 10;
  spec.refresh_postpone_limit = 2;
  spec.refresh_pullin_limit = 1;
  spec.refresh_credit_limit = 2;

  hbm_sim::RefreshManager postponed;
  postponed.reset(spec, 0);
  auto at_10 = postponed.tick(spec, 10, true, false);
  require(at_10.postponed && at_10.credit == 1,
          "refresh postpone did not create one outstanding obligation");
  auto at_20 = postponed.tick(spec, 20, false, false);
  auto at_21 = postponed.tick(spec, 21, false, false);
  require(at_20.started_batch && at_20.credit == 1 && at_21.started_batch &&
              at_21.credit == 0,
          "postponed refresh debt was erased without issuing both batches");

  hbm_sim::RefreshManager pulled;
  pulled.reset(spec, 0);
  auto at_5 = pulled.tick(spec, 5, false, true);
  auto pull_deadline = pulled.tick(spec, 10, false, true);
  auto at_15 = pulled.tick(spec, 15, false, true);
  require(at_5.pulled_in && at_5.credit == -1,
          "refresh pull-in did not record an advance");
  require(!pull_deadline.started_batch && pull_deadline.credit == 0,
          "future deadline did not consume the pulled-in refresh");
  require(
      at_15.pulled_in,
      "pull-in limit did not reset after the future deadline consumed credit");

  DramSpec ranked = spec;
  ranked.org.ranks = 2;
  set_fixture_density_from_geometry(ranked);
  ranked.refresh_postpone_limit = 0;
  ranked.refresh_pullin_limit = 0;
  hbm_sim::RefreshManager ranks;
  ranks.reset(ranked, 0);
  auto rank_first = ranks.tick(ranked, 10, false, false);
  auto rank_second = ranks.tick(ranked, 11, false, false);
  require(rank_first.started_batch && rank_second.started_batch &&
              !rank_first.commands.empty() && !rank_second.commands.empty() &&
              rank_first.commands.front().decoded.rank !=
                  rank_second.commands.front().decoded.rank,
          "multi-rank refresh did not independently service both ranks");
}

void test_lpddr6_efficiency_mode_mapping() {
  DramSpec spec = hbm_sim::make_spec("lpddr6");
  spec.lpddr_efficiency_mode = hbm_sim::LpddrEfficiencyMode::Static;
  hbm_sim::AddressMapper mapper(spec);
  for (int i = 0; i < 64; i++) {
    DecodedAddress decoded =
        mapper.decode(static_cast<hbm_sim::Address>(i * spec.org.line_size));
    require(decoded.pseudo_channel == 0,
            "LPDDR6 efficiency mode mapped request to secondary subchannel");
  }
}

void test_lpddr6_shared_metadata_lane_overhead() {
  DramSpec spec = hbm_sim::make_spec("lpddr6");
  spec.lpddr_dbi_enabled = true;
  spec.lpddr_link_ecc_enabled = true;
  spec.lpddr_dbi_bits_per_request = 8;
  spec.lpddr_link_ecc_bits_per_request = 16;

  require(hbm_sim::lpddr_metadata_lane_bits_per_request(spec) == 16,
          "LPDDR metadata lane should use max(DBI, LinkECC), not sum");
  require(hbm_sim::request_protocol_overhead_bits(spec) == 16,
          "LPDDR request overhead should count shared metadata lane once");
  require(hbm_sim::request_interface_bytes(spec) ==
              spec.transaction_bytes() + 2,
          "LPDDR shared metadata lane should add two bytes to each 32B "
          "transaction");
}

void test_lpddr6_host_line_transaction_split() {
  DramSpec spec = hbm_sim::make_spec("lpddr6");
  spec.supports_refresh = false;
  spec.supports_rfm = false;
  hbm_sim::refresh_timing_constraints(spec);

  require(spec.org.pseudo_channels == 2 && spec.data_bus_bits == 24,
          "LPDDR6 must expose two x12 subchannels");
  require(
      spec.org.rows == 65536 && spec.org.columns == 64,
      "LPDDR6 16Gb/subchannel organization must use 65536 rows and 64 columns");
  require(spec.internal_prefetch_size == 24,
          "LPDDR6 interface burst length must be BL24");
  require(spec.bytes_per_request() == 64,
          "LPDDR6 frontend request size should remain one 64B host line");
  require(spec.transaction_bytes() == 32,
          "LPDDR6 x12 BL24 transaction must carry 32B data");
  require(spec.addressable_capacity_bytes() == 4294967296ULL,
          "two 16Gb LPDDR6 subchannels should expose a 4 GiB device");
  require(
      hbm_sim::request_interface_bytes(spec) == 32,
      "LPDDR6 protection-off accounting is 32B demand, not 36B physical DQ occupancy");

  hbm_sim::TrafficOptions options;
  options.pattern = "stream";
  options.requests = 1;
  options.read_ratio = 100;
  std::vector<Request> requests = hbm_sim::generate_traffic(spec, options);
  require(requests.size() == 2,
          "one 64B host request should split into two LPDDR6 transactions");
  require(requests[0].host_request_id == requests[1].host_request_id &&
              requests[0].transaction_count == 2 &&
              requests[1].transaction_count == 2,
          "LPDDR6 split transactions did not preserve host request identity");
  require(requests[0].address == 0 && requests[1].address == 32 &&
              requests[0].transfer_bytes == 32 &&
              requests[1].transfer_bytes == 32,
          "LPDDR6 split transaction address or payload size is incorrect");
  require(
      requests[0].decoded.column == 0 && requests[1].decoded.column == 1,
      "LPDDR6 32B transactions should consume adjacent column transactions");

  MemorySystem memory(spec);
  memory.run(requests, 20000);
  require(!memory.stats().hit_cycle_limit,
          "LPDDR6 split-transaction run hit cycle limit");
  require(memory.stats().completed_reads == 2 && memory.stats().rd == 2,
          "two LPDDR6 child transactions should issue and complete two RD "
          "commands");
  require(memory.stats().read_bytes == 64 &&
              memory.stats().interface_read_bytes == 64,
          "LPDDR6 split transaction payload or interface-byte accounting is "
          "incorrect");
}

void test_hbm3_lpddr5_host_line_transaction_split() {
  struct Case {
    const char *standard;
    std::uint64_t capacity_bytes;
  };
  for (const Case test : {Case{"hbm3", 17179869184ULL},
                          Case{"lpddr5", 2147483648ULL}}) {
    DramSpec spec = hbm_sim::make_spec(test.standard);
    require(spec.bytes_per_request() == 64,
            std::string(test.standard) + " host request must remain 64B");
    require(spec.transaction_bytes() == 32,
            std::string(test.standard) +
                " protocol transaction must be 32B");
    require(spec.addressable_capacity_bytes() == test.capacity_bytes,
            std::string(test.standard) +
                " address geometry does not match its density profile");

    hbm_sim::TrafficOptions options;
    options.pattern = "stream";
    options.requests = 1;
    options.read_ratio = 100;
    const std::vector<Request> requests =
        hbm_sim::generate_traffic(spec, options);
    require(requests.size() == 2 && requests[0].address == 0 &&
                requests[1].address == 32 &&
                requests[0].transaction_count == 2 &&
                requests[1].transaction_count == 2,
            std::string(test.standard) +
                " must split one 64B host line into adjacent 32B transactions");
  }
}

void test_hbm4_host_line_transaction_split() {
  DramSpec spec = hbm_sim::make_spec("hbm4");
  spec.supports_refresh = false;
  spec.supports_rfm = false;
  hbm_sim::refresh_timing_constraints(spec);

  require(spec.bytes_per_request() == 64,
          "HBM4 frontend request size should remain one 64B host line");
  require(spec.transaction_bytes() == 32,
          "HBM4 DRAM transaction must match one PC x32 BL8 payload");
  require(spec.addressable_capacity_bytes() == 34359738368ULL,
          "HBM4 32Gb/die x8Hi organization should expose 32 GiB");
  require(
      spec.hbm_ecc_bits_per_request == 16,
      "HBM4 external ECC metadata should be 16 bits per 32B PC transaction");
  require(hbm_sim::request_interface_bytes(spec) == 34,
          "HBM4 32B transaction plus 16-bit metadata should occupy 34 "
          "interface bytes");

  hbm_sim::TrafficOptions options;
  options.pattern = "stream";
  options.requests = 1;
  options.read_ratio = 100;
  std::vector<Request> requests = hbm_sim::generate_traffic(spec, options);
  require(requests.size() == 2,
          "one 64B host request should split into two HBM4 transactions");
  require(requests[0].host_request_id == requests[1].host_request_id &&
              requests[0].transaction_count == 2 &&
              requests[1].transaction_count == 2,
          "split transactions did not preserve host request identity");
  require(requests[0].address == 0 && requests[1].address == 32 &&
              requests[0].transfer_bytes == 32 &&
              requests[1].transfer_bytes == 32,
          "split HBM4 transaction address or payload size is incorrect");
  require(requests[0].decoded.column == 0 && requests[1].decoded.column == 1,
          "32B HBM4 transactions should consume adjacent column transactions");

  MemorySystem memory(spec);
  memory.run(requests, 5000);
  require(!memory.stats().hit_cycle_limit,
          "HBM4 split-transaction run hit cycle limit");
  require(
      memory.stats().completed_reads == 2 && memory.stats().rd == 2,
      "two HBM4 child transactions should issue and complete two RD commands");
  require(memory.stats().read_bytes == 64 &&
              memory.stats().interface_read_bytes == 68,
          "HBM4 split transaction payload or interface-byte accounting is "
          "incorrect");
  require(!memory.has_response() && !memory.has_transaction_response() &&
              std::none_of(memory.controllers().begin(),
                           memory.controllers().end(),
                           [](const Controller &controller) {
                             return controller.has_response();
                           }),
          "legacy batch run retained async payload responses without opt-in");

  std::vector<hbm_sim::DfiEvent> events =
      hbm_sim::build_dfi_trace(spec, memory.issued_commands());
  int read_beats = 0;
  for (const auto &event : events) {
    if (event.kind == hbm_sim::DfiEventKind::ReadData) {
      read_beats++;
      require(event.beat_count == 2 && event.beat_bytes == 16,
              "each 32B HBM4 transaction should produce two 16B DFI beats");
    }
  }
  require(read_beats == 4,
          "one 64B host read should produce four DFI data beats in total");

  const std::string trace_path = "/tmp/hbm4_64b_split_roundtrip.trace";
  const std::string payload = "00112233445566778899aabbccddeeff"
                              "102132435465768798a9bacbdcedfe0f"
                              "2031425364758697a8b9cadbecfd0e1f"
                              "30415263748596a7b8c9daebfc0d1e2f";
  {
    std::ofstream out(trace_path);
    out << "0 W 0x1000 data=" << payload << '\n';
    out << "400 R 0x1000 expect=" << payload << '\n';
  }
  hbm_sim::TrafficOptions trace_options;
  trace_options.trace_path = trace_path;
  trace_options.requests = 0;
  std::vector<Request> roundtrip =
      hbm_sim::generate_traffic(spec, trace_options);
  require(roundtrip.size() == 4,
          "64B write/read pair should produce four physical transactions");

  MemorySystem roundtrip_memory(spec);
  roundtrip_memory.run(roundtrip, 5000);
  std::remove(trace_path.c_str());
  require(!roundtrip_memory.stats().hit_cycle_limit &&
              roundtrip_memory.stats().completed_writes == 2 &&
              roundtrip_memory.stats().completed_reads == 2,
          "64B split write/read roundtrip did not complete all transactions");
  require(roundtrip_memory.stats().storage_lines_allocated == 2 &&
              roundtrip_memory.stats().unique_written_lines == 2,
          "two 32B write transactions must allocate two independent backend "
          "blocks");
  require(
      roundtrip_memory.stats().data_checked_reads == 2 &&
          roundtrip_memory.stats().data_mismatches == 0,
      "64B payload was corrupted while splitting or reassembling transactions");
}

void test_async_frontend_response_interface() {
  DramSpec spec = hbm_sim::make_spec("hbm4");
  spec.supports_refresh = false;
  spec.supports_rfm = false;
  hbm_sim::refresh_timing_constraints(spec);

  hbm_sim::MemorySystemOptions system_options;
  // 一项 ingress 强制调用方走 try_submit() 的重试语义。
  system_options.stack_ingress_buffer_size = 1;
  system_options.stack_dispatch_width = 1;
  MemorySystem memory(spec, system_options);

  std::size_t transaction_notifications = 0;
  std::size_t host_notifications = 0;
  memory.set_transaction_response_callback(
      [&](const hbm_sim::TransactionResponse &) {
        transaction_notifications++;
      });
  memory.set_response_callback(
      [&](const hbm_sim::HostResponse &) { host_notifications++; });

  hbm_sim::TrafficOptions traffic;
  traffic.pattern = "stream";
  traffic.requests = 1;
  traffic.read_ratio = 0;
  std::vector<Request> writes = hbm_sim::generate_traffic(spec, traffic);
  require(writes.size() == 2,
          "async response test requires a two-transaction HBM4 host line");
  require(memory.try_submit(writes[0]),
          "async frontend rejected the first ready request");
  require(!memory.try_submit(writes[1]),
          "full stack ingress did not deassert request ready");
  memory.step();
  require(memory.try_submit(writes[1]),
          "async frontend did not accept a held request after one step");

  while (!memory.idle() && memory.clock() < 5000) {
    memory.step();
  }
  require(memory.idle(), "async write requests did not become idle");
  require(memory.has_response(), "async write host response is missing");
  hbm_sim::HostResponse write_response = memory.pop_response();
  require(
      write_response.type == RequestType::Write &&
          write_response.transaction_count == 2 &&
          write_response.transactions.size() == 2 &&
          write_response.status == hbm_sim::ResponseStatus::Ok,
      "async write transactions were not aggregated into one host response");
  require(write_response.transactions[0].transaction_index == 0 &&
              write_response.transactions[1].transaction_index == 1,
          "host response did not preserve transaction_index order");

  std::size_t write_transaction_responses = 0;
  while (memory.has_transaction_response()) {
    const hbm_sim::TransactionResponse response =
        memory.pop_transaction_response();
    require(response.type == RequestType::Write &&
                response.completion_cycle >= response.issued_cycle &&
                response.stack == 0 && response.channel >= 0,
            "write transaction response lost timing or route metadata");
    write_transaction_responses++;
  }
  require(write_transaction_responses == 2,
          "async interface did not return one response per write transaction");

  std::vector<Request> reads = writes;
  hbm_sim::ByteVector expected_host_data;
  for (std::size_t index = 0; index < reads.size(); index++) {
    expected_host_data.insert(expected_host_data.end(),
                              writes[index].payload.begin(),
                              writes[index].payload.end());
    reads[index].id = 9000 + index;
    reads[index].host_request_id = 9000;
    reads[index].type = RequestType::Read;
    reads[index].expected_payload = writes[index].payload;
    reads[index].has_expected_payload = true;
    reads[index].payload.clear();
    reads[index].has_payload = false;
  }

  std::size_t next = 0;
  while (next < reads.size() && memory.clock() < 5000) {
    if (memory.try_submit(reads[next])) {
      next++;
    } else {
      memory.step();
    }
  }
  while (!memory.idle() && memory.clock() < 5000) {
    memory.step();
  }
  require(next == reads.size() && memory.idle(),
          "async read requests did not complete");
  require(memory.has_response(), "async read host response is missing");
  const hbm_sim::HostResponse read_response = memory.pop_response();
  require(read_response.type == RequestType::Read &&
              read_response.data == expected_host_data &&
              read_response.initialized &&
              read_response.initialized_mask.size() ==
                  expected_host_data.size() &&
              std::all_of(read_response.initialized_mask.begin(),
                          read_response.initialized_mask.end(),
                          [](std::uint8_t byte) { return byte == 0xff; }) &&
              read_response.status == hbm_sim::ResponseStatus::Ok,
          "async host read response lost data, initialization mask, or status");
  require(transaction_notifications == 4 && host_notifications == 2,
          "non-consuming async response callbacks fired an incorrect number of "
          "times");
}

void test_async_response_backpressure_holds_completion() {
  DramSpec spec = hbm_sim::make_spec("hbm4");
  spec.supports_refresh = false;
  spec.supports_rfm = false;
  hbm_sim::refresh_timing_constraints(spec);

  hbm_sim::MemorySystemOptions options;
  options.controller.response_queue_capacity = 1;
  options.transaction_response_queue_capacity = 1;
  options.host_response_queue_capacity = 1;
  MemorySystem memory(spec, options);
  memory.set_response_delivery_mode(hbm_sim::ResponseDeliveryMode::Both);

  hbm_sim::TrafficOptions traffic;
  traffic.pattern = "stream";
  traffic.requests = 1;
  traffic.read_ratio = 0;
  const std::vector<Request> writes = hbm_sim::generate_traffic(spec, traffic);
  require(writes.size() == 2 && memory.try_submit(writes[0]) &&
              memory.try_submit(writes[1]),
          "bounded response test could not submit both transactions");

  while (!memory.idle() && memory.clock() < 5000) {
    memory.step();
  }
  require(
      memory.idle() && memory.has_transaction_response() &&
          !memory.has_response(),
      "bounded transaction queue did not hold the incomplete host response");
  const std::uint64_t held_request =
      memory.front_transaction_response().request_id;
  memory.step();
  require(memory.front_transaction_response().request_id == held_request,
          "response changed while valid was held without ready/pop");

  (void)memory.pop_transaction_response();
  memory.step();
  require(memory.has_transaction_response() && memory.has_response(),
          "releasing response ready did not deliver the retained completion");
  const hbm_sim::HostResponse host = memory.pop_response();
  require(host.transactions.size() == 2 &&
              host.status == hbm_sim::ResponseStatus::Ok,
          "response backpressure dropped or corrupted a child transaction");
}

void test_async_frontend_contract_and_host_only_delivery() {
  DramSpec spec = hbm_sim::make_spec("hbm4");
  spec.supports_refresh = false;
  spec.supports_rfm = false;
  hbm_sim::refresh_timing_constraints(spec);

  hbm_sim::MemorySystemOptions options;
  options.controller.response_queue_capacity = 1;
  options.transaction_response_queue_capacity = 1;
  options.host_response_queue_capacity = 1;
  MemorySystem memory(spec, options);

  Request maintenance;
  maintenance.type = RequestType::Maintenance;
  maintenance.next = Command::REFAB;
  bool rejected_maintenance = false;
  try {
    (void)memory.try_submit(maintenance);
  } catch (const std::invalid_argument &) {
    rejected_maintenance = true;
  }
  require(rejected_maintenance,
          "try_submit accepted maintenance that cannot produce HostResponse");

  hbm_sim::TrafficOptions traffic;
  traffic.pattern = "stream";
  traffic.requests = 1;
  traffic.read_ratio = 0;
  std::vector<Request> writes = hbm_sim::generate_traffic(spec, traffic);
  require(writes.size() == 2 && memory.try_submit(writes[0]),
          "host-only contract test could not submit transaction zero");

  bool rejected_duplicate = false;
  try {
    (void)memory.try_submit(writes[0]);
  } catch (const std::invalid_argument &) {
    rejected_duplicate = true;
  }
  require(rejected_duplicate,
          "duplicate in-flight host transaction was not rejected");
  require(memory.try_submit(writes[1]),
          "host-only contract test could not submit transaction one");

  while (!memory.idle() && memory.clock() < 5000)
    memory.step();
  require(memory.idle() && !memory.quiescent() && memory.has_response(),
          "idle/quiescent did not distinguish an unconsumed HostResponse");
  require(!memory.has_transaction_response(),
          "HostOnly mode retained a duplicate TransactionResponse view");

  bool rejected_early_tag_reuse = false;
  try {
    (void)memory.try_submit(writes[0]);
  } catch (const std::invalid_argument &) {
    rejected_early_tag_reuse = true;
  }
  require(rejected_early_tag_reuse,
          "host_request_id was released before response pop/ready handshake");

  const hbm_sim::HostResponse response = memory.pop_response();
  require(response.transactions.size() == 2 && memory.quiescent(),
          "HostResponse pop did not release the tag or drain the system");

  // pop 后允许 frontend 复用同一 tag；这是有限 tag 空间适配器需要的行为。
  require(memory.try_submit(writes[0]) && memory.try_submit(writes[1]),
          "host_request_id was not reusable after HostResponse consumption");
  while (!memory.idle() && memory.clock() < 10000)
    memory.step();
  require(memory.has_response(), "reused host tag did not complete");
  (void)memory.pop_response();
  require(memory.quiescent(), "reused host tag left response state behind");
}

void test_streaming_host_response_consumer() {
  DramSpec spec = hbm_sim::make_spec("hbm4");
  spec.supports_refresh = false;
  spec.supports_rfm = false;
  hbm_sim::refresh_timing_constraints(spec);

  hbm_sim::TrafficOptions traffic;
  traffic.pattern = "stream";
  traffic.requests = 3;
  traffic.read_ratio = 100;
  std::unique_ptr<hbm_sim::TrafficStream> source =
      hbm_sim::make_traffic_stream(spec, traffic);

  MemorySystem memory(spec);
  std::uint64_t consumed = 0;
  memory.run(*source, 10000, [&](const hbm_sim::HostResponse &response) {
    require(response.transactions.size() == 2,
            "streaming consumer received an incomplete HBM4 host response");
    consumed++;
  });
  require(consumed == 3 && memory.quiescent() &&
              !memory.has_transaction_response(),
          "streaming host consumer retained responses or lost a request");
}

void test_async_ecc_response_status() {
  for (const hbm_sim::MemPhyMode mode :
       {hbm_sim::MemPhyMode::Direct, hbm_sim::MemPhyMode::Behavioral}) {
    DramSpec spec = hbm_sim::make_spec("hbm4");
    spec.org.channels = 1;
    spec.org.pseudo_channels = 1;
    spec.org.sids = 1;
    spec.org.bank_groups = 1;
    spec.org.banks_per_group = 1;
    set_fixture_density_from_geometry(spec);
    spec.supports_refresh = false;
    spec.supports_rfm = false;
    hbm_sim::refresh_timing_constraints(spec);

    hbm_sim::StorageModelOptions storage;
    storage.ecc_shadow_enabled = true;
    storage.ecc_check_on_read = true;
    storage.ecc_correct_single_bit = true;
    storage.ecc_inject_period = 1;
    auto image = std::make_shared<hbm_sim::MemoryImage>(spec, 0, storage);

    hbm_sim::MemorySystemOptions options;
    options.controller.phy.mode = mode;
    options.stack_memory_images.push_back(image);
    MemorySystem memory(spec, options);

    const hbm_sim::Address address = 0x2800;
    const hbm_sim::ByteVector payload(32, 0xa5);
    Request write;
    write.id = mode == hbm_sim::MemPhyMode::Direct ? 9200 : 9210;
    write.type = RequestType::Write;
    write.address = address;
    write.decoded = hbm_sim::AddressMapper(spec).decode(address);
    write.host_request_id = write.id;
    write.transfer_bytes = payload.size();
    write.payload = payload;
    write.has_payload = true;
    require(memory.try_submit(write), "ECC async write was not accepted");
    while (!memory.idle() && memory.clock() < 5000)
      memory.step();
    require(memory.idle() && memory.has_response(),
            "ECC async write did not complete");
    (void)memory.pop_response();
    while (memory.has_transaction_response()) {
      (void)memory.pop_transaction_response();
    }

    Request read;
    read.id = write.id + 1;
    read.type = RequestType::Read;
    read.address = address;
    read.decoded = hbm_sim::AddressMapper(spec).decode(address);
    read.host_request_id = read.id;
    read.transfer_bytes = payload.size();
    read.expected_payload = payload;
    read.has_expected_payload = true;
    require(memory.try_submit(read), "ECC async read was not accepted");
    while (!memory.idle() && memory.clock() < 5000)
      memory.step();
    require(memory.idle() && memory.has_response(),
            "ECC async read did not complete");
    const hbm_sim::HostResponse response = memory.pop_response();
    require(
        response.data == payload && response.ecc_corrected &&
            !response.ecc_uncorrectable &&
            response.status == hbm_sim::ResponseStatus::EccCorrected &&
            response.transactions.size() == 1 &&
            response.transactions[0].ecc_corrected,
        "ECC correction status was not propagated through the async response");
  }
}

void test_address_mapping_templates() {
  DramSpec spec = hbm_sim::make_spec("hbm4");
  spec.org.channels = 4;
  spec.org.pseudo_channels = 1;
  spec.org.sids = 1;
  spec.org.ranks = 2;
  spec.org.bank_groups = 2;
  spec.org.banks_per_group = 2;
  spec.org.columns = 4;
  spec.org.rows = 64;
  spec.org.line_size = 64;
  spec.org.dram_transaction_bytes = 64;
  set_fixture_density_from_geometry(spec);

  spec.address_mapping = hbm_sim::AddressMappingKind::Default;
  hbm_sim::AddressMapper mapper_default(spec);
  require(mapper_default.decode(1 * 64).column == 1,
          "default address mapping should put low bits into column");

  spec.address_mapping = hbm_sim::AddressMappingKind::RoBaRaCoCh;
  hbm_sim::AddressMapper mapper_ro_ba_ra_co_ch(spec);
  DecodedAddress ro_ba_line1 = mapper_ro_ba_ra_co_ch.decode(1 * 64);
  DecodedAddress ro_ba_line4 = mapper_ro_ba_ra_co_ch.decode(4 * 64);
  require(ro_ba_line1.channel == 1,
          "RoBaRaCoCh should put low bits into channel first");
  require(ro_ba_line4.column == 1,
          "RoBaRaCoCh should put bits after channel into column");

  spec.address_mapping = hbm_sim::AddressMappingKind::ChRaBaRoCo;
  hbm_sim::AddressMapper mapper_ch_ra_ba_ro_co(spec);
  DecodedAddress ch_ra_line4 = mapper_ch_ra_ba_ro_co.decode(4 * 64);
  require(ch_ra_line4.row == 1,
          "ChRaBaRoCo should put bits after column into row");

  spec.address_mapping = hbm_sim::AddressMappingKind::RoCoRaBaCh;
  hbm_sim::AddressMapper mapper_ro_co_ra_ba_ch(spec);
  DecodedAddress ro_co_line4 = mapper_ro_co_ra_ba_ch.decode(4 * 64);
  require(ro_co_line4.bank == 1,
          "RoCoRaBaCh should put bits after channel into bank composite");
}

void test_lpddr5_cas_write_sequence() {
  DramSpec spec = hbm_sim::make_spec("lpddr5");
  Controller controller(spec);
  // 写请求验证 CAS_WR 与 WR 的顺序，覆盖读路径以外的 WCK/CAS 行为。
  controller.enqueue(make_request(20, RequestType::Write, 0, 0, 0, 7));
  controller.run_until_done(2000);

  require(!controller.stats().hit_cycle_limit, "LPDDR5 write hit cycle limit");
  require(controller.stats().act1 == 1 && controller.stats().act2 == 1,
          "LPDDR5 write did not use split activate");
  require(controller.stats().cas_wr == 1 && controller.stats().wr == 1,
          "LPDDR5 write did not use CAS_WR then WR");

  const auto &trace = controller.issued_commands();
  const IssuedCommand *cas = find_first(trace, Command::CASWR);
  const IssuedCommand *wr = find_first(trace, Command::WR);
  require(cas != nullptr && wr != nullptr,
          "LPDDR5 write trace missing command");
  require(cas->cycle < wr->cycle, "LPDDR5 write command order is wrong");
  require(wr->cycle - cas->cycle >= static_cast<Cycle>(spec.timing.nWCK2CK),
          "LPDDR5 write violated WCK-to-data spacing");
}

Request make_addressed_request(const DramSpec &spec, std::uint64_t id,
                               RequestType type, hbm_sim::Address address) {
  hbm_sim::AddressMapper mapper(spec);
  Request req;
  req.id = id;
  req.address = address;
  req.type = type;
  req.decoded = mapper.decode(address);
  return req;
}

void test_real_storage_write_read_correctness() {
  DramSpec spec = hbm_sim::make_spec("hbm4");
  spec.supports_refresh = false;
  spec.supports_rfm = false;
  hbm_sim::refresh_timing_constraints(spec);

  Controller controller(spec);
  hbm_sim::ByteVector payload = hbm_sim::parse_hex_bytes("0011223344556677");
  Request write =
      make_addressed_request(spec, 1200, RequestType::Write, 0x1000);
  write.payload = payload;
  write.has_payload = true;
  controller.enqueue(write);
  controller.run_until_done(4000);

  Request read = make_addressed_request(spec, 1201, RequestType::Read, 0x1000);
  read.expected_payload = payload;
  read.has_expected_payload = true;
  controller.enqueue(read);
  controller.run_until_done(4000);

  require(controller.stats().data_write_commits == 1,
          "real storage did not commit the write payload");
  require(controller.stats().data_checked_reads == 1,
          "real storage did not check the read payload");
  require(controller.stats().data_mismatches == 0,
          "real storage reported a mismatch after write/read");
  require(controller.stats().storage_lines_allocated >= 1,
          "real storage did not allocate a sparse memory line");
}

void test_overlapping_read_before_write_ordering() {
  DramSpec spec = hbm_sim::make_spec("hbm4");
  spec.supports_refresh = false;
  spec.supports_rfm = false;
  hbm_sim::refresh_timing_constraints(spec);

  const hbm_sim::Address address = 0x1800;
  const hbm_sim::ByteVector old_payload = hbm_sim::parse_hex_bytes("00112233");
  const hbm_sim::ByteVector new_payload = hbm_sim::parse_hex_bytes("aabbccdd");
  hbm_sim::AddressMapper mapper(spec);
  DecodedAddress decoded = mapper.decode(address);
  auto image = std::make_shared<hbm_sim::MemoryImage>(spec);
  image->write(address, old_payload, nullptr, &decoded, 0, 0);

  hbm_sim::ControllerOptions options;
  options.memory_image = image;
  options.retain_command_trace = true;
  Controller controller(spec, options);

  Request read = make_addressed_request(spec, 1202, RequestType::Read, address);
  read.expected_payload = old_payload;
  read.has_expected_payload = true;
  Request write =
      make_addressed_request(spec, 1203, RequestType::Write, address);
  write.payload = new_payload;
  write.has_payload = true;

  require(controller.enqueue(read) && controller.enqueue(write),
          "overlapping ordering requests were not accepted");
  controller.run_until_done(4000);

  Cycle read_cycle = 0;
  Cycle write_cycle = 0;
  for (const auto &issued : controller.issued_commands()) {
    if ((issued.command == Command::RD || issued.command == Command::RDA) &&
        issued.request_id == read.id) {
      read_cycle = issued.cycle;
    }
    if ((issued.command == Command::WR || issued.command == Command::WRA) &&
        issued.request_id == write.id) {
      write_cycle = issued.cycle;
    }
  }
  require(read_cycle > 0 && write_cycle > read_cycle,
          "later overlapping write bypassed the earlier read");
  require(controller.stats().data_checked_reads == 1 &&
              controller.stats().data_mismatches == 0,
          "read-before-write ordering returned future data");
  image->flush_all_row_buffers(controller.clock());
  require(image->read(address, new_payload.size()) == new_payload,
          "ordered write did not eventually commit");

  auto events = hbm_sim::build_dfi_trace(spec, controller.issued_commands());
  auto report =
      hbm_sim::validate_dfi_trace(spec, controller.issued_commands(), events);
  require(report.ok() && report.expected_payload_checks > 0,
          "DFI validation did not retain the ordered read expectation");
}

void test_real_storage_read_forward_correctness() {
  DramSpec spec = hbm_sim::make_spec("hbm4");
  spec.supports_refresh = false;
  spec.supports_rfm = false;
  hbm_sim::refresh_timing_constraints(spec);

  Controller controller(spec);
  hbm_sim::ByteVector payload = hbm_sim::parse_hex_bytes("8899aabbccddeeff");
  Request write =
      make_addressed_request(spec, 1210, RequestType::Write, 0x2000);
  write.payload = payload;
  write.has_payload = true;
  Request read = make_addressed_request(spec, 1211, RequestType::Read, 0x2000);
  read.expected_payload = payload;
  read.has_expected_payload = true;

  controller.enqueue(write);
  controller.enqueue(read);
  controller.run_until_done(4000);

  require(controller.stats().read_forwards == 1,
          "real storage did not exercise read forwarding");
  require(controller.stats().data_forward_checks == 1,
          "real storage did not check forwarded payload");
  require(controller.stats().data_mismatches == 0,
          "forwarded payload did not match pending write data");
}

void test_overlapping_masked_coalesce_preserves_byte_order() {
  DramSpec spec = hbm_sim::make_spec("hbm4");
  spec.supports_refresh = false;
  spec.supports_rfm = false;
  hbm_sim::refresh_timing_constraints(spec);

  Controller controller(spec);
  hbm_sim::ByteVector first(32, 0xaa);
  hbm_sim::ByteVector overlap(32, 0xbb);
  hbm_sim::ByteVector newest(32, 0xcc);

  Request write_first =
      make_addressed_request(spec, 1212, RequestType::Write, 0x1000);
  write_first.payload = first;
  write_first.has_payload = true;
  Request write_overlap =
      make_addressed_request(spec, 1213, RequestType::Write, 0x1010);
  write_overlap.payload = overlap;
  write_overlap.has_payload = true;
  Request write_newest =
      make_addressed_request(spec, 1214, RequestType::Write, 0x1000);
  write_newest.payload = newest;
  write_newest.byte_mask.assign(newest.size(), 0xff);
  write_newest.has_payload = true;
  write_newest.has_byte_mask = true;

  hbm_sim::ByteVector expected(32, 0xbb);
  std::fill(expected.begin(), expected.begin() + 16, 0xcc);
  Request read = make_addressed_request(spec, 1215, RequestType::Read, 0x1010);
  read.expected_payload = expected;
  read.has_expected_payload = true;

  require(controller.enqueue(write_first) &&
              controller.enqueue(write_overlap) &&
              controller.enqueue(write_newest) && controller.enqueue(read),
          "overlapping coalesced write sequence was not accepted");
  controller.run_until_done(8000);

  require(controller.stats().write_coalesces == 1,
          "overlapping test did not exercise same-address coalescing");
  require(controller.stats().read_forwards == 1,
          "overlapping test did not exercise buffered read forwarding");
  require(controller.stats().data_checked_reads == 1 &&
              controller.stats().data_mismatches == 0,
          "masked coalescing lost per-byte last-writer-wins ordering");
}

void test_real_storage_masked_write_correctness() {
  DramSpec spec = hbm_sim::make_spec("hbm4");
  spec.supports_refresh = false;
  spec.supports_rfm = false;
  hbm_sim::refresh_timing_constraints(spec);

  Controller controller(spec);
  Request base = make_addressed_request(spec, 1220, RequestType::Write, 0x3000);
  base.payload = hbm_sim::parse_hex_bytes("0001020304050607");
  base.has_payload = true;
  controller.enqueue(base);
  controller.run_until_done(4000);

  Request masked =
      make_addressed_request(spec, 1221, RequestType::Write, 0x3000);
  masked.payload = hbm_sim::parse_hex_bytes("aabbccddeeff1122");
  masked.byte_mask = hbm_sim::parse_hex_bytes("00ff0000ff000000");
  masked.has_payload = true;
  masked.has_byte_mask = true;
  controller.enqueue(masked);
  controller.run_until_done(4000);

  Request read = make_addressed_request(spec, 1222, RequestType::Read, 0x3000);
  read.expected_payload = hbm_sim::parse_hex_bytes("00bb0203ee050607");
  read.has_expected_payload = true;
  controller.enqueue(read);
  controller.run_until_done(4000);

  require(controller.stats().data_masked_write_commits == 1,
          "real storage did not count masked write commit");
  require(controller.stats().data_mismatches == 0,
          "masked write did not preserve unmasked bytes");
}

void test_physical_storage_coordinates_and_stats() {
  DramSpec spec = hbm_sim::make_spec("hbm4");
  spec.org.channels = 4;
  spec.org.pseudo_channels = 1;
  spec.org.sids = 2;
  spec.org.bank_groups = 2;
  spec.org.banks_per_group = 2;
  spec.org.rows = 64;
  spec.org.columns = 4;
  spec.org.line_size = 64;
  set_fixture_density_from_geometry(spec);
  spec.stack_height = 8;
  spec.address_mapping = hbm_sim::AddressMappingKind::RoBaRaCoCh;

  hbm_sim::Address address =
      static_cast<hbm_sim::Address>(spec.transaction_bytes());
  hbm_sim::AddressMapper mapper(spec);
  DecodedAddress decoded = mapper.decode(address);
  require(
      decoded.channel == 1,
      "physical storage coordinate test expected line 1 to map to channel 1");

  hbm_sim::MemoryImage image(spec);
  hbm_sim::ByteVector payload = hbm_sim::parse_hex_bytes("cafebabefeedface");
  image.write(address, payload, nullptr, &decoded);
  bool initialized = false;
  hbm_sim::ByteVector actual =
      image.read(address, payload.size(), &initialized, &decoded);

  hbm_sim::PhysicalAddress physical = image.physical_address(address, &decoded);
  hbm_sim::PhysicalStorageStats stats = image.storage_stats();
  require(initialized && actual == payload,
          "physical storage image did not preserve payload bytes");
  require(physical.channel == decoded.channel &&
              physical.bank == decoded.bank && physical.row == decoded.row &&
              physical.column == decoded.column,
          "physical storage coordinate does not match decoded DRAM address");
  require(physical.layer >= 0 && physical.layer < spec.stack_height,
          "physical storage layer is outside stack height");
  require(physical.tile_x >= 0 && physical.tile_y >= 0 &&
              physical.tile_z == physical.layer &&
              physical.floorplan_cols > 0 && physical.floorplan_rows > 0,
          "physical storage floorplan tile is not populated");
  require(physical.subarray >= 0 &&
              physical.subarray < physical.subarrays_per_bank &&
              physical.mat_x >= 0 && physical.mat_y >= 0 &&
              physical.cell_x >= 0 && physical.cell_y >= 0 &&
              physical.microbump_x >= 0 && physical.microbump_y >= 0,
          "physical storage subarray/mat/cell/microbump coordinate is not "
          "populated");
  require(stats.lines_allocated == 1 &&
              stats.bytes_allocated ==
                  static_cast<std::uint64_t>(spec.transaction_bytes()),
          "physical storage stats did not count one allocated line");
  require(stats.channels_touched == 1 && stats.banks_touched == 1 &&
              stats.rows_touched == 1,
          "physical storage stats did not count touched hierarchy levels");
  require(
      stats.subarrays_touched == 1 && stats.mats_touched == 1 &&
          stats.cells_touched == 1 && stats.microbumps_touched == 1,
      "physical storage stats did not count fine-grained physical hierarchy");
  require(stats.floorplan_tiles_touched == 1,
          "physical storage stats did not count touched floorplan tile");
  require(stats.read_line_accesses == 1 && stats.write_line_accesses == 1,
          "physical storage stats did not count line read/write accesses");
}

void test_memory_image_cross_line_read_uses_each_line_address() {
  DramSpec spec = hbm_sim::make_spec("hbm4");
  spec.org.line_size = 16;
  spec.org.dram_transaction_bytes = 16;
  set_fixture_density_from_geometry(spec);
  hbm_sim::MemoryImage image(spec);
  hbm_sim::AddressMapper mapper(spec);
  const DecodedAddress first = mapper.decode(0);
  const DecodedAddress second = mapper.decode(16);
  const hbm_sim::ByteVector a(16, 0x11);
  const hbm_sim::ByteVector b(16, 0x22);

  image.activate_row(first, 1);
  image.write(0, a, nullptr, &first, 1, 2);
  image.write(16, b, nullptr, &second, 2, 3);

  bool initialized = false;
  const hbm_sim::ByteVector actual = image.read(8, 16, &initialized, &first);
  hbm_sim::ByteVector expected(8, 0x11);
  expected.insert(expected.end(), 8, 0x22);
  const hbm_sim::ByteVector init_mask =
      image.read_initialized_mask(8, 16, &first);
  require(initialized && actual == expected,
          "cross-line MemoryImage read reused the first line's decoded row");
  require(std::all_of(init_mask.begin(), init_mask.end(),
                      [](std::uint8_t value) { return value == 0xff; }),
          "cross-line initialized mask reused the first line's decoded row");
}

void test_all_bank_refresh_covers_every_pc_and_sid() {
  DramSpec spec = hbm_sim::make_spec("hbm4");
  spec.org.channels = 1;
  spec.org.pseudo_channels = 2;
  spec.org.sids = 2;
  spec.org.ranks = 1;
  spec.org.bank_groups = 1;
  spec.org.banks_per_group = 1;
  set_fixture_density_from_geometry(spec);
  spec.refresh_policy = hbm_sim::MaintenancePolicyKind::AllBank;
  spec.timing.nREFI = 1;

  hbm_sim::RefreshManager manager;
  manager.reset(spec, 0);
  const auto result = manager.tick(
      spec, static_cast<hbm_sim::Cycle>(spec.tick_multiplier), false, false);
  require(result.started_batch && result.commands.size() == 4,
          "all-bank refresh did not cover every pseudo-channel/SID scope");
  std::set<std::pair<int, int>> scopes;
  for (const auto &command : result.commands) {
    require(command.command == hbm_sim::Command::REFAB,
            "all-bank refresh batch contains a non-REFab command");
    scopes.emplace(command.decoded.pseudo_channel, command.decoded.sid);
  }
  require(scopes.size() == 4,
          "all-bank refresh generated duplicate PC/SID scopes");
}

void test_passive_multistack_memory_model_isolation() {
  DramSpec spec = hbm_sim::make_spec("hbm4");
  spec.org.channels = 1;
  spec.org.pseudo_channels = 1;
  spec.org.sids = 1;
  spec.org.bank_groups = 1;
  spec.org.banks_per_group = 1;
  spec.org.rows = 128;
  spec.org.columns = 16;
  spec.org.line_size = 64;
  set_fixture_density_from_geometry(spec);
  spec.stack_height = 4;
  set_fixture_density_from_geometry(spec);

  hbm_sim::StorageModelOptions options;
  options.thermal_grid_cols_per_tile = 2;
  options.thermal_grid_rows_per_tile = 2;

  hbm_sim::MultiStackMemoryModel model(spec, options);
  require(model.stack_count() == hbm_sim::kDefaultStackCount &&
              hbm_sim::kDefaultStackCount == 6,
          "passive multi-stack default should instantiate six stacks");
  hbm_sim::Address address = 0x1000;
  hbm_sim::AddressMapper mapper(spec);
  DecodedAddress decoded = mapper.decode(address);
  std::vector<hbm_sim::ByteVector> payloads = {
      hbm_sim::parse_hex_bytes("0011223344556677"),
      hbm_sim::parse_hex_bytes("8899aabbccddeeff"),
      hbm_sim::parse_hex_bytes("1021324354657687"),
      hbm_sim::parse_hex_bytes("98badcfe10325476"),
      hbm_sim::parse_hex_bytes("0f1e2d3c4b5a6978"),
      hbm_sim::parse_hex_bytes("ffeeddccbbaa9988"),
  };

  for (int stack_id = 0; stack_id < model.stack_count(); stack_id++) {
    model.write(stack_id, address, payloads[static_cast<std::size_t>(stack_id)],
                nullptr, &decoded, 1800 + static_cast<std::uint64_t>(stack_id),
                10 + stack_id);
  }

  std::vector<hbm_sim::StorageKey> keys;
  for (int stack_id = 0; stack_id < model.stack_count(); stack_id++) {
    const hbm_sim::ByteVector &payload =
        payloads[static_cast<std::size_t>(stack_id)];
    hbm_sim::StackReadResult read =
        model.read(stack_id, address, payload.size(), &decoded);
    require(read.initialized && read.data == payload,
            "passive six-stack model allowed same local address to alias "
            "across stacks");

    hbm_sim::PhysicalAddress physical =
        model.stack(stack_id).memory_image().physical_address(address,
                                                              &decoded);
    hbm_sim::StorageKey key =
        model.stack(stack_id).memory_image().storage_key(address, &decoded);
    require(
        physical.stack == stack_id && key.stack == stack_id,
        "passive six-stack physical coordinates or storage keys lost stack_id");
    keys.push_back(key);
  }
  for (std::size_t i = 0; i < keys.size(); i++) {
    for (std::size_t j = i + 1; j < keys.size(); j++) {
      require(!(keys[i] == keys[j]),
              "passive six-stack storage keys do not distinguish stack_id");
    }
  }

  hbm_sim::Address command_address = address + 64;
  DecodedAddress command_decoded = mapper.decode(command_address);
  const int command_stack = hbm_sim::kDefaultStackCount - 1;

  hbm_sim::StackCommand act;
  act.stack_id = command_stack;
  act.command = Command::ACT;
  act.decoded = command_decoded;
  act.cycle = 20;
  model.issue_command(act);

  hbm_sim::StackCommand wr;
  wr.stack_id = command_stack;
  wr.command = Command::WR;
  wr.decoded = command_decoded;
  wr.cycle = 30;
  wr.address = command_address;
  wr.has_address = true;
  wr.request_id = 1802;
  wr.payload = hbm_sim::parse_hex_bytes("1020304050607080");
  model.issue_command(wr);

  hbm_sim::StackCommand rd;
  rd.stack_id = command_stack;
  rd.command = Command::RD;
  rd.decoded = command_decoded;
  rd.cycle = 40;
  rd.address = command_address;
  rd.has_address = true;
  rd.payload_bytes = wr.payload.size();
  hbm_sim::StackCommandResult rd_result = model.issue_command(rd);
  require(rd_result.read_data_valid && rd_result.initialized &&
              rd_result.read_data == wr.payload,
          "passive multi-stack command port did not return the stack-local "
          "write payload");

  std::vector<hbm_sim::PhysicalStorageStats> per_stack =
      model.per_stack_storage_stats();
  hbm_sim::PhysicalStorageStats total = model.storage_stats();
  require(
      per_stack.size() ==
              static_cast<std::size_t>(hbm_sim::kDefaultStackCount) &&
          total.stacks_touched ==
              static_cast<std::uint64_t>(hbm_sim::kDefaultStackCount),
      "passive multi-stack stats did not report per-stack topology coverage");
  std::uint64_t per_stack_lines = 0;
  std::uint64_t per_stack_power_events = 0;
  for (const auto &stats : per_stack) {
    require(stats.stacks_touched == 1,
            "passive multi-stack per-stack stats should see exactly one stack");
    per_stack_lines += stats.lines_allocated;
    per_stack_power_events += stats.power_events;
  }
  require(total.lines_allocated == per_stack_lines &&
              total.power_events == per_stack_power_events,
          "passive multi-stack aggregate stats did not sum stack-local model "
          "stats");

  bool rejected_bad_stack = false;
  try {
    static_cast<void>(model.read(hbm_sim::kDefaultStackCount, address,
                                 payloads[0].size(), &decoded));
  } catch (const std::out_of_range &) {
    rejected_bad_stack = true;
  }
  require(rejected_bad_stack,
          "passive multi-stack model accepted an invalid stack_id");
}

void test_thermal_neighbor_state_and_coordinates() {
  char directory[] = "/tmp/hbm_thermal_contract_XXXXXX";
  require(mkdtemp(directory) != nullptr, "cannot create thermal test directory");
  const std::string path = std::string(directory) + "/map.txt";
  using Row = std::map<std::string, std::string>;
  auto read_map = [&]() {
    std::ifstream input(path);
    std::vector<std::string> header;
    std::vector<Row> rows;
    for (std::string line; std::getline(input, line);) {
      if (line.rfind("# stack layer", 0) == 0) {
        std::istringstream fields(line.substr(2));
        for (std::string key; fields >> key;) header.push_back(key);
      } else if (!line.empty() && line[0] != '#') {
        std::istringstream fields(line);
        Row row;
        for (const auto& key : header) {
          require(static_cast<bool>(fields >> row[key]), "short thermal row");
        }
        rows.push_back(std::move(row));
      }
    }
    require(!rows.empty(), "empty thermal map");
    return rows;
  };
  auto total_excess = [&]() {
    double total = 0;
    for (const auto& row : read_map()) total += std::stod(row.at("temperature_c")) - 40;
    return total;
  };
  for (bool vertical : {false, true}) {
    for (double cooling : {0.0, 0.01}) {
      hbm_sim::StorageModelOptions options;
      options.thermal_cooling_per_cycle = cooling;
      options.thermal_rise_c_per_pj = 0.01;
      options.floorplan_enabled = !vertical;
      options.thermal_lateral_coupling = vertical ? 0 : 0.1;
      options.thermal_vertical_coupling = vertical ? 0.1 : 0;
      options.thermal_tsv_coupling_scale = 0;
      options.act_energy_pj = 1000;
      hbm_sim::MemoryImage image(hbm_sim::make_spec(vertical ? "hbm4" : "lpddr5"), 0, options);
      DecodedAddress source{};
      image.record_command_event(Command::ACT, source, 10);
      // With all nodes relaxed to the same time, pairwise coupling must only
      // redistribute temperature; first direct access must retain preheating.
      image.advance_thermal(20);
      image.dump_thermal_text(path);
      const double before = total_excess();
      const auto nodes = read_map();
      require(std::any_of(nodes.begin(), nodes.end(), [&](const Row& node) {
        return std::stoi(node.at("layer")) == (vertical ? 1 : 0) &&
               std::stoi(node.at("thermal_x")) == (vertical ? 0 : 1) &&
               std::stoi(node.at("thermal_y")) == 0 &&
               std::stoi(node.at("events")) == 0 &&
               std::stod(node.at("temperature_c")) > 40;
      }), "first-access test must target a preheated coupling-only node");
      source.bank = 1;
      image.record_command_event(Command::ACT, source, 20);
      image.dump_thermal_text(path);
      require(std::abs(total_excess() - before - 10) < 0.001,
              "first direct event discarded an existing neighbor's temperature");
    }
  }
  for (const char* standard : {"hbm3", "hbm4", "lpddr5", "lpddr6"}) {
    for (bool floorplan : {false, true}) {
      auto spec = hbm_sim::make_spec(standard);
      hbm_sim::StorageModelOptions options;
      options.stack_id = 2;
      options.floorplan_enabled = floorplan;
      options.thermal_grid_cols_per_tile = 2;
      options.thermal_grid_rows_per_tile = 3;
      hbm_sim::MemoryImage image(spec, 0, options);
      DecodedAddress source{};
      source.row = spec.org.rows - 1;
      source.column = spec.org.columns - 1;
      image.record_command_event(Command::ACT, source, 10);
      image.dump_thermal_text(path);
      bool neighbor_seen = false, vertical_seen = false, cross_tile_seen = false;
      for (const auto& row : read_map()) {
        auto n = [&](const char* key) { return std::stoi(row.at(key)); };
        require(n("stack") == 2 && n("thermal_z") == n("layer"), "thermal stack/layer mismatch");
        require(n("thermal_x") == n("tile_x") * 2 + n("grid_x") &&
                    n("thermal_y") == n("tile_y") * 3 + n("grid_y"), "neighbor tile/grid mismatch");
        require(n("grid_x") >= 0 && n("grid_x") < 2 && n("grid_y") >= 0 && n("grid_y") < 3,
                "invalid local thermal grid");
        require(n("tile_id") == (n("layer") * (n("thermal_rows") / 3) + n("tile_y")) *
                    (n("thermal_cols") / 2) + n("tile_x"), "neighbor tile ID mismatch");
        if (n("events") == 0) {
          neighbor_seen = true;
          vertical_seen |= n("layer") != 0;
          cross_tile_seen |= n("tile_x") != 0 || n("tile_y") != 0;
          require(row.at("address_kind") == "coupling_only" && n("ch") == -1 && n("bank") == -1,
                  "coupling-only node must not impersonate a source DRAM address");
        } else {
          require(row.at("address_kind") == "direct_event" && n("ch") == 0,
                  "direct node lost its representative address");
        }
      }
      require(neighbor_seen, "coupling test did not create a neighbor");
      require(spec.lpddr_family || vertical_seen, "HBM test missed vertical coupling");
      require(!floorplan || cross_tile_seen, "test missed cross-tile coupling");
    }
  }
  std::remove(path.c_str());
  std::remove(directory);
}

void test_floorplan_power_and_thermal_model() {
  const std::string thermal_path = "/tmp/hbm_sim_thermal_map.txt";
  DramSpec spec = hbm_sim::make_spec("hbm4");
  spec.org.channels = 2;
  spec.org.pseudo_channels = 2;
  spec.org.sids = 2;
  spec.org.bank_groups = 2;
  spec.org.banks_per_group = 2;
  set_fixture_density_from_geometry(spec);
  spec.stack_height = 8;

  hbm_sim::Address address = 0x7000;
  hbm_sim::AddressMapper mapper(spec);
  DecodedAddress decoded = mapper.decode(address);
  hbm_sim::MemoryImage image(spec);

  hbm_sim::PhysicalAddress physical = image.physical_address(address, &decoded);
  image.record_command_event(Command::ACT, decoded, 10);
  image.record_command_event(Command::RD, decoded, 20, 64);
  image.record_command_event(Command::WR, decoded, 30, 64);
  hbm_sim::PhysicalStorageStats stats = image.storage_stats();

  require(stats.power_events == 3,
          "power model did not record all command events");
  require(stats.power_act_energy_pj > 0.0 && stats.power_read_energy_pj > 0.0 &&
              stats.power_write_energy_pj > 0.0 &&
              stats.power_energy_pj >= stats.power_act_energy_pj,
          "power model did not classify command energy");
  require(stats.thermal_updates == 3 && stats.thermal_tiles_touched >= 1 &&
              stats.thermal_peak_temp_c > 40.0,
          "thermal model did not heat the touched floorplan tile");
  require(stats.thermal_hotspot_layer == physical.layer &&
              stats.thermal_hotspot_x == physical.tile_x &&
              stats.thermal_hotspot_y == physical.tile_y,
          "thermal hotspot does not match physical floorplan tile");
  image.dump_thermal_text(thermal_path);
  {
    std::ifstream in(thermal_path);
    std::stringstream buffer;
    buffer << in.rdbuf();
    require(buffer.str().find("temperature_c") != std::string::npos &&
                buffer.str().find("energy_pj") != std::string::npos &&
                buffer.str().find("thermal_coupling_enabled") !=
                    std::string::npos,
            "thermal map dump did not include temperature/energy fields");
  }

  hbm_sim::StorageModelOptions tuned;
  tuned.power_scale = 2.0;
  tuned.thermal_ambient_c = 35.0;
  tuned.thermal_cooling_per_cycle = 0.0;
  tuned.thermal_rise_c_per_pj = 0.001;
  hbm_sim::MemoryImage tuned_image(spec, 0, tuned);
  tuned_image.record_command_event(Command::ACT, decoded, 10);
  tuned_image.record_command_event(Command::RD, decoded, 20, 64);
  hbm_sim::PhysicalStorageStats tuned_stats = tuned_image.storage_stats();
  double expected_energy = (tuned.act_energy_pj + tuned.read_energy_pj +
                            tuned.read_energy_per_byte_pj * 64.0) *
                           tuned.power_scale;
  require(std::fabs(tuned_stats.power_energy_pj - expected_energy) < 0.0001,
          "custom storage power scale was not applied");
  require(tuned_stats.thermal_peak_temp_c > tuned.thermal_ambient_c + 1.0,
          "custom thermal rise parameter was not applied");

  // peak 是全运行历史最大值，avg 是当前 thermal grid 的空间平均值。后续事件
  // 触发冷却后，peak 不应被最终较低温度覆盖。
  hbm_sim::StorageModelOptions cooling;
  cooling.thermal_coupling_enabled = false;
  cooling.thermal_cooling_per_cycle = 1.0;
  cooling.thermal_rise_c_per_pj = 0.01;
  hbm_sim::MemoryImage cooling_image(spec, 0, cooling);
  cooling_image.record_command_event(Command::REFAB, decoded, 10);
  const double historical_peak =
      cooling_image.storage_stats().thermal_peak_temp_c;
  cooling_image.record_command_event(Command::MRW, decoded, 1000);
  const hbm_sim::PhysicalStorageStats cooled = cooling_image.storage_stats();
  require(std::fabs(cooled.thermal_peak_temp_c - historical_peak) < 1e-9 &&
              cooled.thermal_avg_temp_c < cooled.thermal_peak_temp_c,
          "thermal peak was recomputed from the cooled final-state map");

  hbm_sim::StorageModelOptions synchronized = cooling;
  synchronized.thermal_cooling_per_cycle = 0.01;
  hbm_sim::MemoryImage synchronized_image(spec, 0, synchronized);
  synchronized_image.record_command_event(Command::REFAB, decoded, 10);
  const double before_sync =
      synchronized_image.storage_stats().thermal_avg_temp_c;
  synchronized_image.advance_thermal(110);
  const auto after_sync = synchronized_image.storage_stats();
  require(after_sync.thermal_avg_temp_c < before_sync &&
              std::fabs(after_sync.thermal_avg_temp_c -
                        synchronized.thermal_ambient_c) < 1e-9 &&
              after_sync.thermal_peak_temp_c > after_sync.thermal_avg_temp_c,
          "explicit thermal synchronization did not cool all nodes to one final cycle");

  tuned.power_enabled = false;
  hbm_sim::MemoryImage power_off_image(spec, 0, tuned);
  power_off_image.record_command_event(Command::ACT, decoded, 10);
  hbm_sim::PhysicalStorageStats power_off_stats =
      power_off_image.storage_stats();
  require(power_off_stats.power_events == 0 &&
              power_off_stats.thermal_updates == 0 &&
              power_off_stats.thermal_peak_temp_c == tuned.thermal_ambient_c,
          "disabled power model still produced storage events");

  DecodedAddress invalid_decoded = decoded;
  invalid_decoded.bank = spec.org.banks_per_group;
  bool invalid_power_off_coordinate_rejected = false;
  try {
    power_off_image.record_command_event(Command::ACT, invalid_decoded, 11);
  } catch (const std::out_of_range &) {
    invalid_power_off_coordinate_rejected = true;
  }
  require(invalid_power_off_coordinate_rejected,
          "power-disabled command path accepted an invalid DRAM coordinate");
}

void test_dramsim3_idd_power_and_grid_thermal() {
  DramSpec spec = hbm_sim::make_spec("hbm4");
  spec.org.channels = 2;
  spec.org.pseudo_channels = 1;
  spec.org.sids = 1;
  spec.org.bank_groups = 2;
  spec.org.banks_per_group = 2;
  spec.org.rows = 64;
  spec.org.columns = 16;
  set_fixture_density_from_geometry(spec);
  spec.timing.tCK_ps = 500.0;
  spec.timing.nBL = 2;
  spec.timing.nRAS = 42;
  spec.timing.nRP = 20;
  spec.timing.nRC = 62;
  spec.timing.nRFC = 320;
  spec.timing.nRFCpb = 80;

  hbm_sim::StorageModelOptions options;
  options.power_source = "dramsim3_idd";
  options.thermal_grid_cols_per_tile = 4;
  options.thermal_grid_rows_per_tile = 4;
  options.thermal_coupling_enabled = false;
  options.idd_vdd = 1.2;
  options.idd0_ma = 65;
  options.idd2n_ma = 40;
  options.idd3n_ma = 55;
  options.idd4r_ma = 390;
  options.idd4w_ma = 500;
  options.idd5ab_ma = 250;
  options.idd5pb_ma = 5;
  options.idd_devices_per_rank = 1;

  DecodedAddress decoded;
  decoded.channel = 1;
  decoded.pseudo_channel = 0;
  decoded.sid = 0;
  decoded.rank = 0;
  decoded.bank_group = 1;
  decoded.bank = 1;
  decoded.row = 32;
  decoded.column = 8;

  hbm_sim::MemoryImage image(spec, 0, options);
  const auto &calibrated = image.options();
  const double expected_read = 1.2 * (390.0 - 55.0) * (2.0 * 500.0 / 1000.0);
  require(std::fabs(calibrated.read_energy_pj - expected_read) < 0.0001,
          "DRAMsim3-style IDD read energy calibration is wrong");
  require(calibrated.read_energy_per_byte_pj == 0.0 &&
              calibrated.write_energy_per_byte_pj == 0.0,
          "IDD power source should not also charge per-byte energy");

  hbm_sim::PhysicalAddress physical = image.physical_address(0x1000, &decoded);
  require(physical.thermal_grid_x == 2 && physical.thermal_grid_y == 2,
          "DRAMsim3-style row/column thermal grid coordinate is wrong");
  require(physical.thermal_x == physical.tile_x * 4 + 2 &&
              physical.thermal_y == physical.tile_y * 4 + 2,
          "thermal grid coordinate was not anchored inside the floorplan tile");

  image.record_command_event(Command::ACT, decoded, 10);
  image.record_command_event(Command::RD, decoded, 20, 64);
  image.record_command_event(Command::REFAB, decoded, 30);
  hbm_sim::PhysicalStorageStats stats = image.storage_stats();
  require(stats.power_act_energy_pj > 0.0 && stats.power_read_energy_pj > 0.0 &&
              stats.power_refresh_energy_pj > 0.0,
          "IDD-calibrated power model did not classify command energy");
  require(stats.thermal_grid_cells_touched == 1 &&
              stats.thermal_hotspot_x == physical.thermal_x &&
              stats.thermal_hotspot_y == physical.thermal_y,
          "thermal map did not use the grid-level hotspot coordinate");
}

void test_tsv_thermal_coupling_and_ecc_shadow() {
  DramSpec spec = hbm_sim::make_spec("hbm4");
  spec.org.channels = 1;
  spec.org.pseudo_channels = 1;
  spec.org.sids = 1;
  spec.org.bank_groups = 1;
  spec.org.banks_per_group = 1;
  spec.org.rows = 64;
  spec.org.columns = 16;
  spec.org.line_size = 64;
  set_fixture_density_from_geometry(spec);
  spec.stack_height = 4;

  set_fixture_density_from_geometry(spec);
  hbm_sim::StorageModelOptions options;
  options.thermal_grid_cols_per_tile = 2;
  options.thermal_grid_rows_per_tile = 2;
  options.thermal_coupling_enabled = true;
  options.thermal_lateral_coupling = 0.05;
  options.thermal_vertical_coupling = 0.03;
  options.thermal_tsv_coupling_scale = 0.08;
  options.thermal_tsvs_per_grid = 8;
  options.ecc_shadow_enabled = true;
  options.ecc_check_on_read = true;
  options.ecc_correct_single_bit = true;
  options.ecc_inject_period = 1;
  options.subarrays_per_bank = 8;
  options.mats_per_subarray_x = 2;
  options.mats_per_subarray_y = 2;
  options.microbumps_x = 4;
  options.microbumps_y = 4;

  hbm_sim::MemoryImage image(spec, 0, options);
  hbm_sim::Address address = 0x2400;
  hbm_sim::AddressMapper mapper(spec);
  DecodedAddress decoded = mapper.decode(address);
  hbm_sim::ByteVector payload = hbm_sim::parse_hex_bytes("0011223344556677");

  image.write(address, payload, nullptr, &decoded, 1700, 5);
  const auto ecc_before = image.ecc_status_counters();
  static_assert(noexcept(image.ecc_status_counters()));
  bool initialized = false;
  hbm_sim::ByteVector actual =
      image.read(address, payload.size(), &initialized, &decoded);
  require(initialized && actual == payload,
          "SECDED shadow did not correct injected single-bit data error");

  image.record_command_event(Command::ACT, decoded, 10);
  image.record_command_event(Command::RD, decoded, 20, 64);
  hbm_sim::PhysicalStorageStats stats = image.storage_stats();
  hbm_sim::PhysicalAddress physical = image.physical_address(address, &decoded);

  const auto ecc_after = image.ecc_status_counters();
  require(ecc_before.ecc_corrected_errors == 0 &&
              ecc_after.ecc_corrected_errors == 1 &&
              ecc_after.ecc_corrected_errors == stats.ecc_corrected_errors &&
              ecc_after.ecc_uncorrectable_errors == stats.ecc_uncorrectable_errors,
          "lightweight ECC snapshot differs from full storage statistics");

  require(stats.ecc_injected_errors == 1 && stats.ecc_checked_reads >= 1 &&
              stats.ecc_corrected_errors == 1 &&
              stats.ecc_uncorrectable_errors == 0,
          "SECDED shadow stats did not record single-bit injection/correction");
  require(stats.ecc_shadow_updates >= 2,
          "SECDED shadow was not refreshed after write and correction");
  require(stats.thermal_vertical_transfers > 0 &&
              stats.thermal_tsv_transfers > 0 &&
              stats.thermal_coupled_delta_c > 0.0,
          "TSV-aware thermal coupling did not move heat vertically");
  require(
      physical.subarray >= 0 && physical.mat_id >= 0 &&
          physical.microbump_x < options.microbumps_x &&
          physical.microbump_y < options.microbumps_y,
      "fine-grained physical placement was not populated for ECC/thermal line");
}

void test_memory_image_row_buffer_writeback() {
  DramSpec spec = hbm_sim::make_spec("hbm4");
  spec.org.line_size = 64;
  set_fixture_density_from_geometry(spec);

  hbm_sim::Address address = 0x4800;
  hbm_sim::AddressMapper mapper(spec);
  DecodedAddress decoded = mapper.decode(address);
  hbm_sim::MemoryImage image(spec);
  hbm_sim::ByteVector payload = hbm_sim::parse_hex_bytes("1021324354657687");

  image.activate_row(decoded, 10);
  image.write(address, payload, nullptr, &decoded, 501, 11);
  bool initialized = false;
  hbm_sim::ByteVector actual =
      image.read(address, payload.size(), &initialized, &decoded);
  auto metadata = image.metadata(address, &decoded);
  hbm_sim::PhysicalStorageStats open_stats = image.storage_stats();

  require(initialized && actual == payload,
          "row buffer did not serve the newly written payload before PRE");
  require(metadata.has_value() && metadata->last_writer_request_id == 501 &&
              metadata->last_write_cycle == 11,
          "row buffer metadata did not expose the dirty writer");
  require(open_stats.row_buffer_activations == 1 &&
              open_stats.row_buffer_open_rows == 1 &&
              open_stats.row_buffer_dirty_rows == 1 &&
              open_stats.row_buffer_writes == 1,
          "row buffer did not track an open dirty row after write");

  image.precharge_bank(decoded, 30);
  hbm_sim::PhysicalStorageStats closed_stats = image.storage_stats();
  actual = image.read(address, payload.size(), &initialized, &decoded);
  metadata = image.metadata(address, &decoded);

  require(initialized && actual == payload,
          "row buffer writeback did not preserve payload after PRE");
  require(metadata.has_value() && metadata->last_writer_request_id == 501,
          "row buffer writeback did not preserve last writer metadata");
  require(closed_stats.row_buffer_open_rows == 0 &&
              closed_stats.row_buffer_dirty_rows == 0 &&
              closed_stats.row_buffer_precharges == 1 &&
              closed_stats.row_buffer_dirty_writebacks == 1,
          "row buffer PRE did not close and write back the dirty row");
}

void test_controller_drives_row_buffer_storage_events() {
  DramSpec spec = hbm_sim::make_spec("hbm4");
  spec.supports_refresh = false;
  spec.supports_rfm = false;
  spec.hbm_edge_pairing = false;
  spec.tick_multiplier = 1;
  hbm_sim::refresh_timing_constraints(spec);

  auto image = std::make_shared<hbm_sim::MemoryImage>(spec);
  hbm_sim::ControllerOptions options;
  options.memory_image = image;
  options.row_policy = hbm_sim::RowPolicyKind::OpenPage;
  Controller controller(spec, options);

  hbm_sim::ByteVector payload = hbm_sim::parse_hex_bytes("33445566778899aa");
  Request write =
      make_addressed_request(spec, 1240, RequestType::Write, 0x5000);
  write.payload = payload;
  write.has_payload = true;
  controller.enqueue(write);
  controller.run_until_done(4000);

  bool initialized = false;
  hbm_sim::ByteVector actual =
      image->read(write.address, payload.size(), &initialized, &write.decoded);
  require(initialized && actual == payload,
          "controller-driven row buffer write was not visible through the open "
          "row buffer");
  require(controller.stats().data_write_commits == 1,
          "controller-driven row buffer test did not commit exactly one write");
  require(controller.stats().rowbuf_activations >= 1 &&
              controller.stats().rowbuf_writes >= 1 &&
              controller.stats().rowbuf_open_rows >= 1 &&
              controller.stats().rowbuf_dirty_rows == 0 &&
              controller.stats().rowbuf_dirty_writebacks >= 1,
          "normal completion did not checkpoint the dirty row buffer without "
          "changing DRAM state");

  Request read = make_addressed_request(spec, 1241, RequestType::Read, 0x5000);
  read.expected_payload = payload;
  read.has_expected_payload = true;
  controller.enqueue(read);
  controller.run_until_done(4000);

  require(controller.stats().data_checked_reads == 1 &&
              controller.stats().data_mismatches == 0,
          "controller-driven row buffer read did not validate payload");
  require(controller.stats().rowbuf_reads >= 1 &&
              controller.stats().rowbuf_lazy_loads >= 1,
          "controller did not model row-buffer read/lazy row load (reads=" +
              std::to_string(controller.stats().rowbuf_reads) + ", lazy=" +
              std::to_string(controller.stats().rowbuf_lazy_loads) + ")");
  require(controller.stats().power_events >= 3 &&
              controller.stats().power_energy_pj > 0.0 &&
              controller.stats().thermal_peak_temp_c > 40.0,
          "controller did not feed command events into power/thermal model");
  require(controller.stats().dfi_write_beats > 0 &&
              controller.stats().dfi_read_beats > 0 &&
              controller.stats().dfi_data_bytes >= payload.size() * 2,
          "controller did not split payload traffic into DFI beats");
}

void test_auto_precharge_storage_timing_matches_phy_modes() {
  std::uint64_t direct_precharges = 0;
  for (const hbm_sim::MemPhyMode mode :
       {hbm_sim::MemPhyMode::Direct, hbm_sim::MemPhyMode::Behavioral}) {
    DramSpec spec = hbm_sim::make_spec("hbm4");
    spec.org.channels = 1;
    spec.org.pseudo_channels = 1;
    spec.org.sids = 1;
    spec.org.ranks = 1;
    spec.org.bank_groups = 1;
    spec.org.banks_per_group = 1;
    set_fixture_density_from_geometry(spec);
    spec.supports_refresh = false;
    spec.supports_rfm = false;
    spec.hbm_edge_pairing = false;
    spec.tick_multiplier = 1;
    hbm_sim::refresh_timing_constraints(spec);

    auto image = std::make_shared<hbm_sim::MemoryImage>(spec);
    hbm_sim::ControllerOptions options;
    options.memory_image = image;
    options.row_policy = hbm_sim::RowPolicyKind::ClosedPage;
    options.phy.mode = mode;
    Controller controller(spec, options);

    Request write = make_addressed_request(
        spec, mode == hbm_sim::MemPhyMode::Direct ? 9300 : 9310,
        RequestType::Write, 0x5200);
    write.payload = hbm_sim::ByteVector(32, 0x5a);
    write.has_payload = true;
    require(controller.enqueue(write), "closed-page write was not accepted");
    controller.run_until_done(5000);

    const hbm_sim::PhysicalStorageStats stats = image->storage_stats();
    require(controller.stats().wra == 1 && stats.row_buffer_precharges == 1 &&
                stats.row_buffer_open_rows == 0,
            "WRA did not precharge MemoryImage at data completion");
    if (mode == hbm_sim::MemPhyMode::Direct) {
      direct_precharges = stats.row_buffer_precharges;
    } else {
      require(stats.row_buffer_precharges == direct_precharges,
              "Direct and Behavioral PHY produced different WRA row-buffer "
              "behavior");
    }
  }
}

void test_dram_geometry_overflow_is_rejected() {
  DramSpec spec = hbm_sim::make_spec("hbm4");
  spec.org.channels = 1;
  spec.org.pseudo_channels = 1;
  spec.org.sids = 1;
  spec.org.ranks = 1;
  spec.org.bank_groups = 1;
  spec.org.banks_per_group = 1;
  spec.org.rows = std::numeric_limits<int>::max();
  spec.org.columns = std::numeric_limits<int>::max();
  spec.org.dram_transaction_bytes = std::numeric_limits<int>::max();

  bool rejected_capacity_overflow = false;
  try {
    (void)spec.addressable_capacity_bytes();
  } catch (const std::overflow_error &) {
    rejected_capacity_overflow = true;
  }
  require(rejected_capacity_overflow,
          "addressable capacity silently wrapped uint64_t");

  spec.org.rows = 0;
  bool rejected_invalid_dimension = false;
  try {
    (void)spec.total_addressable_transactions();
  } catch (const std::invalid_argument &) {
    rejected_invalid_dimension = true;
  }
  require(rejected_invalid_dimension,
          "non-positive DRAM geometry was silently clamped");
}

void test_physical_storage_multichannel_memory_system() {
  DramSpec spec = hbm_sim::make_spec("hbm4");
  spec.org.channels = 4;
  spec.org.pseudo_channels = 1;
  spec.org.sids = 1;
  spec.org.bank_groups = 1;
  spec.org.banks_per_group = 1;
  spec.org.rows = 64;
  spec.org.columns = 4;
  spec.org.line_size = 64;
  set_fixture_density_from_geometry(spec);
  spec.address_mapping = hbm_sim::AddressMappingKind::RoBaRaCoCh;
  spec.supports_refresh = false;
  spec.supports_rfm = false;
  spec.hbm_edge_pairing = false;
  spec.tick_multiplier = 1;
  hbm_sim::refresh_timing_constraints(spec);

  Request write0 = make_addressed_request(spec, 1230, RequestType::Write, 0);
  write0.payload = hbm_sim::parse_hex_bytes("0102030405060708");
  write0.has_payload = true;
  Request write1 = make_addressed_request(
      spec, 1231, RequestType::Write,
      static_cast<hbm_sim::Address>(spec.transaction_bytes()));
  write1.payload = hbm_sim::parse_hex_bytes("1112131415161718");
  write1.has_payload = true;

  MemorySystem system(spec);
  system.run({write0, write1}, 4000);

  require(!system.stats().hit_cycle_limit,
          "physical storage multichannel run hit cycle limit");
  require(system.stats().completed_writes == 2,
          "physical storage multichannel run did not complete both writes");
  require(system.stats().storage_lines_allocated == 2,
          "physical storage multichannel run did not allocate two lines");
  require(
      system.stats().unique_written_lines == 2,
      "physical storage multichannel run did not count unique written lines");
  const double expected_density =
      100.0 * static_cast<double>(system.stats().unique_written_lines) /
      static_cast<double>(system.stats().total_addressable_lines);
  require(std::fabs(system.stats().storage_density_pct - expected_density) <
              1.0e-12,
          "multichannel storage density was calculated before final row-buffer "
          "writeback");
  require(system.stats().storage_channels_touched == 2,
          "physical storage did not preserve global channel placement");
  require(system.stats().storage_write_line_accesses == 2,
          "physical storage did not count both write-line accesses");
}

void test_memory_image_text_checkpoint_and_mismatch_report() {
  const std::string image_path = "/tmp/hbm_sim_memory_image.txt";
  const std::string dump_path = "/tmp/hbm_sim_final_memory.txt";
  const std::string report_path = "/tmp/hbm_sim_mismatch_report.txt";
  {
    std::ofstream out(image_path);
    out << "# address with explicit decoded storage coordinate\n";
    out << "0x1000 ch=1 pc=0 sid=0 rank=0 bg=1 bank=2 row=3 col=4 "
        << "data=0011223344556677\n";
  }

  DramSpec spec = hbm_sim::make_spec("hbm4");
  spec.org.line_size = 64;
  set_fixture_density_from_geometry(spec);
  hbm_sim::MemoryImage image(spec);
  image.load_text(image_path);

  bool initialized = false;
  hbm_sim::ByteVector actual = image.read(0x1000, 8, &initialized);
  require(initialized && hbm_sim::bytes_to_hex(actual) == "0011223344556677",
          "memory image text load did not initialize expected bytes");

  bool tail_initialized = true;
  image.read(0x1020, 1, &tail_initialized);
  require(!tail_initialized,
          "memory image text load should keep unwritten bytes uninitialized");

  auto metadata = image.metadata(0x1000);
  require(metadata.has_value(),
          "memory image text load did not create block metadata");
  require(metadata->storage_key.channel == 1 &&
              metadata->storage_key.bank_group == 1 &&
              metadata->storage_key.bank == 2 &&
              metadata->storage_key.row == 3 &&
              metadata->storage_key.column == 4,
          "memory image text load did not preserve decoded storage key");
  require(image.address_for_storage_key(metadata->storage_key).value_or(0) ==
              0x1000,
          "BankStorage view did not map storage key back to the line address");

  hbm_sim::ByteVector patch = hbm_sim::parse_hex_bytes("aabbccdd");
  image.write(0x1004, patch, nullptr, nullptr, 44, 123);
  metadata = image.metadata(0x1000);
  require(metadata->last_writer_request_id == 44 &&
              metadata->last_write_cycle == 123 && metadata->version > 0,
          "DataBlock metadata did not record last writer/version");

  image.dump_text(dump_path);
  {
    std::ifstream in(dump_path);
    std::stringstream buffer;
    buffer << in.rdbuf();
    require(buffer.str().find("last_writer=44") != std::string::npos,
            "final memory dump did not include last writer metadata");
    require(buffer.str().find("ch=1") != std::string::npos,
            "final memory dump did not include physical storage coordinate");
  }

  hbm_sim::DataValidator validator;
  hbm_sim::ByteVector expected = hbm_sim::parse_hex_bytes("ffffffff");
  hbm_sim::ByteVector readback = image.read(0x1004, 4, &initialized);
  validator.check_read(200, 45, 0x1004, metadata->physical, expected, readback,
                       initialized, false, metadata);
  validator.dump_text(report_path);
  require(validator.mismatch_count() == 1,
          "DataValidator did not record a deliberate mismatch");
  {
    std::ifstream in(report_path);
    std::stringstream buffer;
    buffer << in.rdbuf();
    require(
        buffer.str().find("ffffffff") != std::string::npos &&
            buffer.str().find("aabbccdd") != std::string::npos &&
            buffer.str().find("44") != std::string::npos,
        "mismatch report did not include expected/actual/last-writer details");
  }

  const std::string invalid_coordinate_path =
      "/tmp/hbm_sim_invalid_coordinate_image.txt";
  {
    std::ofstream out(invalid_coordinate_path);
    out << "0x2000 ch=" << spec.org.channels
        << " pc=0 sid=0 rank=0 bg=0 bank=0 row=0 col=0 data=01\n";
  }
  bool invalid_coordinate_rejected = false;
  try {
    hbm_sim::MemoryImage invalid_image(spec);
    invalid_image.load_text(invalid_coordinate_path);
  } catch (const std::out_of_range &) {
    invalid_coordinate_rejected = true;
  }
  require(invalid_coordinate_rejected,
          "memory text input accepted an out-of-range decoded coordinate");
}

void test_data_trace_payload_parsing() {
  const std::string path = "/tmp/hbm_sim_data_trace.trace";
  {
    std::ofstream out(path);
    out << "W 0x4000 data=01020304\n";
    out << "7 R 0x4000 expect=01020304\n";
    out << "W 0x5000 data=ffffffff\n";
    out << "R 0x5000 expect=last_write\n";
    out << "W 0x6000\n";
    out << "R 0x6000 expect=last_write\n";
  }

  DramSpec spec = hbm_sim::make_spec("hbm4");
  hbm_sim::TrafficOptions options;
  options.trace_path = path;
  options.requests = 0;
  std::vector<Request> requests = hbm_sim::generate_traffic(spec, options);
  require(requests.size() == 8, "data trace parser did not split default 64B "
                                "requests into HBM transactions");
  require(requests[0].has_payload &&
              hbm_sim::bytes_to_hex(requests[0].payload) == "01020304",
          "data trace parser did not attach write payload");
  require(requests[1].has_expected_payload &&
              hbm_sim::bytes_to_hex(requests[1].expected_payload) == "01020304",
          "data trace parser did not attach read expectation");
  require(requests[1].inject_cycle == 7,
          "data trace parser did not preserve timed trace cycle");
  require(requests[3].has_expected_payload &&
              hbm_sim::bytes_to_hex(requests[3].expected_payload) == "ffffffff",
          "data trace parser did not resolve explicit last_write expectation");
  require(requests[4].has_payload && requests[5].has_payload &&
              requests[6].has_expected_payload &&
              requests[7].has_expected_payload &&
              requests[4].payload == requests[6].expected_payload &&
              requests[5].payload == requests[7].expected_payload,
          "data trace parser did not resolve generated last_write expectation");

  const std::string multistack_path =
      "/tmp/hbm_sim_multistack_last_write.trace";
  {
    std::ofstream out(multistack_path);
    out << "W 0x7000 stack=0 data=0102\n";
    out << "W 0x7000 stack=1 data=aabb\n";
    // stack token 故意放在 expect 后，验证 token 顺序不会改变命名空间。
    out << "R 0x7000 expect=last_write stack=0\n";
    out << "R 0x7000 expect=last_write stack=1\n";
  }
  options.trace_path = multistack_path;
  options.stack_count = 2;
  requests = hbm_sim::generate_traffic(spec, options);
  require(requests.size() == 4 &&
              hbm_sim::bytes_to_hex(requests[2].expected_payload) == "0102" &&
              hbm_sim::bytes_to_hex(requests[3].expected_payload) == "aabb",
          "last_write expectations aliased identical local addresses across "
          "stacks");
}

void test_data_address_range_overflow_is_rejected() {
  DramSpec spec = hbm_sim::make_spec("hbm4");
  hbm_sim::MemoryImage image(spec);
  const hbm_sim::Address last =
      std::numeric_limits<hbm_sim::Address>::max();

  bool read_rejected = false;
  try {
    (void)image.read(last, 2);
  } catch (const std::overflow_error &) {
    read_rejected = true;
  }
  require(read_rejected,
          "MemoryImage accepted a read that wrapped Address space");

  bool write_rejected = false;
  try {
    image.write(last, hbm_sim::ByteVector(2, 0xaa));
  } catch (const std::overflow_error &) {
    write_rejected = true;
  }
  require(write_rejected,
          "MemoryImage accepted a write that wrapped Address space");

  const hbm_sim::Address capacity = spec.addressable_capacity_bytes();
  bool capacity_read_rejected = false;
  try {
    (void)image.read(capacity - 1, 2);
  } catch (const std::out_of_range &) {
    capacity_read_rejected = true;
  }
  require(capacity_read_rejected,
          "MemoryImage accepted a read that crossed configured capacity");
  bool capacity_write_rejected = false;
  try {
    image.write(capacity - 1, hbm_sim::ByteVector(2, 0xaa));
  } catch (const std::out_of_range &) {
    capacity_write_rejected = true;
  }
  require(capacity_write_rejected,
          "MemoryImage accepted a write that crossed configured capacity");

  const std::string path = "/tmp/hbm_sim_overflow_burst.trace";
  {
    std::ofstream out(path);
    out << "BR 0xffffffffffffffe0 len=64\n";
  }
  hbm_sim::TrafficOptions options;
  options.trace_path = path;
  options.requests = 0;
  bool burst_rejected = false;
  try {
    (void)hbm_sim::generate_traffic(spec, options);
  } catch (const std::overflow_error &) {
    burst_rejected = true;
  }
  require(burst_rejected,
          "trace frontend accepted a burst that wrapped Address space");
  std::remove(path.c_str());
}

void remove_backend_files(const std::string &data_path) {
  std::remove(data_path.c_str());
  std::remove((data_path + ".init").c_str());
  std::remove((data_path + ".meta").c_str());
  std::remove((data_path + ".present").c_str());
}

void test_streaming_traffic_source() {
  DramSpec spec = hbm_sim::make_spec("hbm4");
  spec.supports_refresh = false;
  spec.supports_rfm = false;
  hbm_sim::refresh_timing_constraints(spec);

  hbm_sim::TrafficOptions options;
  options.pattern = "random";
  options.requests = 256;
  options.read_ratio = 50;
  options.seed = 17;
  options.inject_interval = 1;
  std::unique_ptr<hbm_sim::TrafficStream> source =
      hbm_sim::make_traffic_stream(spec, options);

  hbm_sim::ControllerOptions controller_options;
  controller_options.retain_command_trace = false;
  Controller controller(spec, controller_options);
  controller.run(*source, 200000);

  const std::uint64_t expected_transactions = options.requests * 2;
  require(controller.stats().injected_requests == expected_transactions,
          "streaming source did not inject all split DRAM transactions");
  require(source->stream_stats().host_requests == options.requests,
          "streaming source host-request count is incorrect");
  require(source->stream_stats().dram_transactions == expected_transactions,
          "streaming source DRAM-transaction count is incorrect");
  require(source->stream_stats().emitted_requests == expected_transactions,
          "streaming source emitted-request count is incorrect");
  require(controller.issued_commands().empty(),
          "disabled command retention still accumulated issued commands");
  const hbm_sim::Stats &stats = controller.stats();
  const double expected_density =
      stats.total_addressable_lines == 0
          ? 0.0
          : 100.0 * static_cast<double>(stats.unique_written_lines) /
                static_cast<double>(stats.total_addressable_lines);
  require(std::fabs(stats.storage_density_pct - expected_density) < 1.0e-12,
          "storage density is not based on unique written lines");

  hbm_sim::TrafficOptions bounded;
  bounded.pattern = "random";
  bounded.requests = 128;
  bounded.seed = 99;
  bounded.random_address_space_bytes = 4096;
  std::unique_ptr<hbm_sim::TrafficStream> bounded_source =
      hbm_sim::make_traffic_stream(spec, bounded);
  Request bounded_request;
  std::uint64_t bounded_count = 0;
  while (bounded_source->next(bounded_request)) {
    require(bounded_request.address + bounded_request.transfer_bytes <= 4096,
            "bounded random traffic escaped random_address_space_bytes");
    bounded_count++;
  }
  require(bounded_count == bounded.requests * 2,
          "bounded random traffic changed host-request splitting");

  hbm_sim::TrafficOptions too_small = bounded;
  too_small.random_address_space_bytes = 32;
  bool small_space_rejected = false;
  try {
    (void)hbm_sim::make_traffic_stream(spec, too_small);
  } catch (const std::invalid_argument &) {
    small_space_rejected = true;
  }
  require(small_space_rejected,
          "random address space smaller than one host request was accepted");

  hbm_sim::TrafficOptions unaligned = bounded;
  unaligned.random_address_space_bytes = 4097;
  bool unaligned_space_rejected = false;
  try {
    (void)hbm_sim::make_traffic_stream(spec, unaligned);
  } catch (const std::invalid_argument &) {
    unaligned_space_rejected = true;
  }
  require(unaligned_space_rejected,
          "unaligned random address space was accepted");
}

void test_file_backed_memory_backend(hbm_sim::MemoryBackendKind kind,
                                     const std::string &suffix) {
  const std::string path = "/tmp/hbm_sim_backend_" + suffix + ".bin";
  remove_backend_files(path);

  DramSpec spec = hbm_sim::make_spec("hbm4");
  hbm_sim::StorageModelOptions options;
  options.memory_backend.kind = kind;
  options.memory_backend.capacity_bytes = 1024 * 1024;
  options.memory_backend.data_file = path;
  options.memory_backend.chunk_size_bytes = 64 * 1024;
  options.memory_backend.chunk_cache_entries = 2;
  options.topology_stats_scan_limit = 1;
  const hbm_sim::Address address = 0x3000;
  const hbm_sim::ByteVector payload =
      hbm_sim::parse_hex_bytes("0123456789abcdef");

  {
    hbm_sim::MemoryImage image(spec, 0, options);
    DecodedAddress decoded = hbm_sim::AddressMapper(spec).decode(address);
    image.write(address, payload, nullptr, &decoded, 99, 12);
    image.flush_backend();
    require(image.allocated_lines() == 1,
            "file-backed memory image did not allocate one line");
    const hbm_sim::PhysicalStorageStats stats = image.storage_stats();
    require(stats.unique_written_lines == 1,
            "file-backed unique-written counter is incorrect");
    require(
        stats.topology_scan_skipped == 1 && stats.topology_lines_scanned == 0,
        "file-backed topology scan limit did not prevent bitmap enumeration");
  }

  {
    hbm_sim::MemoryImage reopened(spec, 0, options);
    bool initialized = true;
    hbm_sim::ByteVector actual =
        reopened.read(address, payload.size(), &initialized);
    require(actual == payload,
            "file-backed memory image did not persist payload across reopen");
    require(initialized,
            "file-backed memory image did not persist initialized mask");
    require(reopened.allocated_lines() == 1 &&
                reopened.storage_stats().unique_written_lines == 1,
            "file-backed metadata header did not persist line counters");
    require(reopened.all_addresses().size() == 1,
            "file-backed memory image presence map is incorrect after reopen");
  }

  remove_backend_files(path);
}

} // namespace

int main() {
  // 第一组：不跑完整 controller，只验证单位换算、timing table 元数据、
  // TimingEngine scope ready time 和 CommandExecutor 单命令副作用。
  test_jedec_conversion_helpers();
  test_unified_spec_factory();
  test_timing_table_validation();
  test_library_spec_rejects_unimplemented_or_invalid_protocol_modes();
  test_timing_engine_table_and_window_state();
  test_command_executor_state_transitions();

  // 第二组：验证 Ramulator plugin 风格 command
  // trace/validator，以及独立命令状态机。
  test_command_trace_plugins();
  test_dfi_trace_generation();
  test_hbm4_edge_pairing_validator_matrix();
  test_command_state_machine();
  test_control_command_state_and_validator();
  test_initialization_control_sequence_execution();

  // 第三组：HBM4 专用路径，包括 scoped timing、edge pairing、refresh、RFM 和
  // timing source。
  test_hbm4_scoped_timing();
  test_hbm4_refresh_manager();
  test_hbm4_all_bank_refresh_policy();
  test_hbm4_rfm_manager();
  test_hbm4_all_bank_rfm_policy();
  test_timing_source_override();
  test_timing_profile_dimensions();

  // 第四组：控制器架构语义，包括多 controller 并行、写合并/转发和 row policy。
  test_multi_controller_parallel_channels();
  test_active_six_stack_memory_system_routing_qos_and_stats();
  test_write_forward_and_coalesce();
  test_closed_page_row_policy();
  test_closed_cap_row_policy();
  test_maintenance_progress_dependencies();

  // 第五组：LPDDR6/LPDDR5 专用路径，包括 REFdb、PRAC/RFM、CAS/WCK 和 efficiency
  // mapping。
  test_lpddr6_refdb_counter_boundaries();
  test_lpddr6_refresh_manager();
  test_lpddr6_dual_bank_refresh_pair();
  test_refdb_does_not_precharge_other_pseudo_channel();
  test_lpddr6_prac_rfm_manager();
  test_lpddr6_cas_read_sequence();
  test_lpddr6_wck_sync_skip();
  test_lpddr6_wck_always_on_mode();
  test_lpddr6_ca_parity_command_overhead();
  test_refresh_credit_and_low_power();
  test_refresh_credit_conservation_and_rank_rotation();
  test_lpddr6_efficiency_mode_mapping();
  test_lpddr6_shared_metadata_lane_overhead();
  test_lpddr6_host_line_transaction_split();
  test_hbm3_lpddr5_host_line_transaction_split();
  test_hbm4_host_line_transaction_split();
  test_async_frontend_response_interface();
  test_async_response_backpressure_holds_completion();
  test_async_frontend_contract_and_host_only_delivery();
  test_streaming_host_response_consumer();
  test_async_ecc_response_status();
  test_address_mapping_templates();
  test_lpddr5_cas_write_sequence();

  // 第六组：真实存储区与数据正确性。第一版采用稀疏 MemoryImage，
  // 覆盖普通写后读、写缓冲转发、masked write 和 trace payload 解析。
  test_real_storage_write_read_correctness();
  test_overlapping_read_before_write_ordering();
  test_real_storage_read_forward_correctness();
  test_overlapping_masked_coalesce_preserves_byte_order();
  test_real_storage_masked_write_correctness();
  test_physical_storage_coordinates_and_stats();
  test_memory_image_cross_line_read_uses_each_line_address();
  test_all_bank_refresh_covers_every_pc_and_sid();
  test_passive_multistack_memory_model_isolation();
  test_floorplan_power_and_thermal_model();
  test_thermal_neighbor_state_and_coordinates();
  test_dramsim3_idd_power_and_grid_thermal();
  test_tsv_thermal_coupling_and_ecc_shadow();
  test_memory_image_row_buffer_writeback();
  test_controller_drives_row_buffer_storage_events();
  test_auto_precharge_storage_timing_matches_phy_modes();
  test_dram_geometry_overflow_is_rejected();
  test_physical_storage_multichannel_memory_system();
  test_memory_image_text_checkpoint_and_mismatch_report();
  test_data_trace_payload_parsing();
  test_data_address_range_overflow_is_rejected();
  test_streaming_traffic_source();
  test_file_backed_memory_backend(hbm_sim::MemoryBackendKind::MmapSparse,
                                  "mmap");
  test_file_backed_memory_backend(hbm_sim::MemoryBackendKind::ChunkFile,
                                  "chunk");
  std::cout << "sequence tests passed\n";
  return 0;
}
