#include "hbm_sim/stats/result.hpp"
#include "hbm_sim/dram/spec.hpp"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <regex>
#include <sstream>
#include <stdexcept>

namespace hbm_sim {
namespace {
std::string trim(const std::string& s) {
  auto a = s.find_first_not_of(" \t\r\n");
  return a == std::string::npos ? "" : s.substr(a, s.find_last_not_of(" \t\r\n") - a + 1);
}
std::string quote(const std::string& value) {
  std::ostringstream out;
  out << '"';
  for (unsigned char c : value) {
    if (c == '"' || c == '\\') out << '\\' << c;
    else if (c < 0x20) out << "\\u" << std::hex << std::setw(4)
                           << std::setfill('0') << static_cast<unsigned>(c);
    else out << c;
  }
  out << '"';
  return out.str();
}
std::string json(const ResultValue& v) {
  if (const auto* s = std::get_if<std::string>(&v)) return quote(*s);
  if (const auto* d = std::get_if<double>(&v); d && !std::isfinite(*d))
    throw std::runtime_error("non-finite result");
  return result_value_text(v);
}
void object(std::ostream& out, const ResultFields& fields) {
  out << '{';
  bool first = true;
  for (const auto& [k, v] : fields) {
    out << (first ? "" : ",") << "\n    " << quote(k) << ": " << json(v);
    first = false;
  }
  out << "\n  }";
}
std::string value(const ResultFields& f, const std::string& k) {
  auto it = f.find(k);
  if (it == f.end() || std::holds_alternative<std::nullptr_t>(it->second)) return "N/A";
  if (const auto* d = std::get_if<double>(&it->second)) {
    std::ostringstream s;
    s << std::setprecision(6) << *d;
    return s.str();
  }
  return result_value_text(it->second);
}
double number(const ResultFields& f, const std::string& k) {
  auto it = f.find(k);
  if (it == f.end()) throw std::runtime_error("missing result field: " + k);
  return std::visit([](const auto& v) -> double {
    using T = std::decay_t<decltype(v)>;
    if constexpr (std::is_arithmetic_v<T>) return static_cast<double>(v);
    else throw std::runtime_error("non-numeric result field");
  }, it->second);
}
bool enabled(const ResultFields& f, const std::string& k) {
  return value(f, k) == "true";
}
void select(ResultFields& target, const ResultFields& source, const char* keys) {
  std::istringstream in(keys);
  std::string key;
  while (in >> key) if (auto it = source.find(key); it != source.end())
    target.emplace(*it);
}
}  // namespace

std::string result_value_text(const ResultValue& v) {
  return std::visit([](const auto& item) -> std::string {
    using T = std::decay_t<decltype(item)>;
    if constexpr (std::is_same_v<T, std::nullptr_t>) return "null";
    else if constexpr (std::is_same_v<T, std::string>) return item;
    else if constexpr (std::is_same_v<T, bool>) return item ? "true" : "false";
    else {
      std::ostringstream out;
      out << std::setprecision(std::numeric_limits<double>::max_digits10) << item;
      return out.str();
    }
  }, v);
}

ResultReport make_result_report(const ResultFields& f) {
  ResultReport r;
  select(r.model, f, "model_name standard model_conformance stack_count channels "
      "pseudo_channels sids ranks bank_groups banks_per_group rows columns "
      "stack_height density_gb capacity_per_instance_bytes aggregate_capacity_bytes "
      "line_size dram_transaction_bytes scheduler row_policy address_mapping "
      "mem_phy_mode memory_backend supports_refresh supports_rfm ecc_shadow "
      "power_model_enabled thermal_model_enabled lpddr_family memory_system channel_mapper stack_mapping");
  select(r.parameters, f, "data_rate_mbps data_bus_bits tCK_ps tick_duration_ps "
      "tick_multiplier lpddr_wck_ratio pattern requests read_ratio inject_interval "
      "seed random_address_space_bytes effective_random_address_space_bytes "
      "addr_stride read_buffer_size write_buffer_size input_kind");
  select(r.metrics, f, "host_requests dram_transactions completed_reads completed_writes "
      "remaining_requests remaining_pending hit_cycle_limit system_cycles "
      "read_bytes write_bytes achieved_bw_GBps peak_bandwidth_GBps bandwidth_util_pct "
      "storage_lines_allocated storage_bytes_allocated power_energy_pJ "
      "thermal_avg_temp_C thermal_peak_temp_C");
  select(r.validation, f, "cmd_validation dfi_validation cmd_validation_checked "
      "dfi_validation_events data_checked_reads data_mismatches "
      "ecc_uncorrectable_errors golden_verified golden_mismatches");
  for (const char* key : {"cmd_validation", "dfi_validation"})
    if (value(r.validation, key) == "off") r.validation[key] = std::string("not_run");
  if (f.contains("system_cycles")) {
    const double ns = number(f, "tick_duration_ps") / 1000;
    r.metrics["simulation_time_ns"] = number(f, "system_cycles") * ns;
    r.metrics["avg_read_latency_ns"] = number(f, "completed_reads") == 0
        ? ResultValue(nullptr) : ResultValue(number(f, "avg_read_latency") * ns);
    const double classified = number(f, "row_hits") + number(f, "row_misses") +
                              number(f, "row_conflicts");
    r.metrics["row_hit_pct"] = classified == 0 ? ResultValue(nullptr)
        : ResultValue(100 * number(f, "row_hits") / classified);
  }
  if (!enabled(f, "power_model_enabled")) r.metrics.erase("power_energy_pJ");
  if (!enabled(f, "thermal_model_enabled")) {
    r.metrics.erase("thermal_avg_temp_C");
    r.metrics.erase("thermal_peak_temp_C");
  }
  if (enabled(f, "lpddr_family")) {
    if (value(f, "sids") == "1") r.model.erase("sids");
  }
  else {
    // A nontrivial experimental rank dimension must never disappear.
    if (value(f, "ranks") == "1") r.model.erase("ranks");
    r.parameters.erase("lpddr_wck_ratio");
  }
  if (value(f, "input_kind") == "trace") {
    for (const char* key : {"pattern", "read_ratio", "seed",
                           "random_address_space_bytes", "effective_random_address_space_bytes",
                           "addr_stride", "requests"})
      r.parameters.erase(key);
  }
  if (f.contains("stack_count")) {
    const auto count = static_cast<std::size_t>(number(f, "stack_count"));
    for (std::size_t i = 0; i < count; ++i) {
      const auto prefix = "stack_" + std::to_string(i) + "_";
      if (!f.contains(prefix + "reads")) continue;
      ResultFields s;
      record_field(s, "stack", i);
      for (auto [old, key] : {std::pair{"reads", "completed_reads"},
                             {"writes", "completed_writes"}, {"bw_GBps", "achieved_bw_GBps"}})
        s[key] = f.at(prefix + old);
      s["avg_read_latency_ns"] = number(f, prefix + "reads") == 0 ? ResultValue(nullptr)
          : ResultValue(number(f, prefix + "avg_read_latency") *
                        number(f, "tick_duration_ps") / 1000);
      r.stacks.push_back(std::move(s));
    }
  }
  if (value(f, "stats_view") == "diagnostic") r.diagnostics = f;
  return r;
}

// Compare executed values, not file locations or just the [override] layer.
// Timing source sections collapse to one numerical timing namespace; provenance
// belongs in the resolved snapshot, not a false "timing changed" notification.
std::vector<ParameterChange> compare_resolved_parameters(
    const std::string& baseline, const std::string& effective) {
  auto parse = [](const std::string& text) {
    std::map<std::string, std::string> result;
    std::istringstream in(text);
    std::string line, section;
    while (std::getline(in, line)) {
      line = trim(line);
      if (line.empty() || line.front() == '#') continue;
      if (line.front() == '[') { section = line.substr(1, line.find(']') - 1); continue; }
      auto eq = line.find('=');
      if (eq == std::string::npos) continue;
      auto k = trim(line.substr(0, eq)), v = trim(line.substr(eq + 1));
      if (section == "meta" || section == "model" || section == "validation" ||
          section == "outputs") continue;
      if (section == "workload" && (k == "pattern" || k == "requests" ||
          k == "read_ratio" || k == "seed" || k == "random_address_space_bytes" ||
          k == "addr_stride" || k == "inject_interval" || k == "stats_view" ||
          k == "progress_interval" || k == "trace")) continue;
      if ((k == "source" && section.starts_with("timing.")) || k.ends_with("_file")) continue;
      auto scope = section.starts_with("timing.") ? "timing" : section;
      result[scope + "." + k] = v;
    }
    return result;
  };
  auto a = parse(baseline), b = parse(effective);
  std::vector<ParameterChange> changes;
  for (const auto& [k, v] : b) {
    auto it = a.find(k);
    if (it == a.end() || it->second != v)
      changes.push_back({k, it == a.end() ? "<unset>" : it->second, v});
  }
  return changes;
}

void print_diagnostics(std::ostream& out, const ResultFields& f) {
  for (const auto& [k, v] : f)
    out << std::left << std::setw(32) << k << ": " << result_value_text(v) << '\n';
}

void print_result(std::ostream& out, const ResultReport& r) {
  auto m = [&](const char* k) { return value(r.model, k); };
  auto p = [&](const char* k) { return value(r.parameters, k); };
  auto s = [&](const char* k) { return value(r.metrics, k); };
  auto v = [&](const char* k) { return value(r.validation, k); };
  const bool lp = enabled(r.model, "lpddr_family");
  out << "# ===== MODEL =====\n"
      << "Model / Standard         : " << m("model_name") << " / " << m("standard") << '\n'
      << "Organization             : " << m("stack_count") << (lp ? " Device x " : " Stack x ")
      << m("channels") << " Channel x " << m("pseudo_channels")
      << (lp ? " Subchannel x " : " PC x ") << (lp ? m("ranks") : m("sids"))
      << (lp ? " Rank\n" : " SID\n");
  if (!lp && r.model.contains("ranks")) out << "Experimental Ranks       : " << m("ranks") << '\n';
  if (lp && r.model.contains("sids")) out << "Experimental SIDs        : " << m("sids") << " (nonstandard LPDDR dimension)\n";
  out << "Bank / Row / Column      : " << m("bank_groups") << " BG x " << m("banks_per_group")
      << " Bank per " << (lp ? "SC/Rank" : "PC/SID")
      << "; " << m("rows") << " Row x " << m("columns") << " transaction columns per Bank\n";
  auto gib = [&](const char* key) {
    std::ostringstream os;
    os << std::setprecision(6) << number(r.model, key) / 1073741824.0;
    return os.str();
  };
  out << "Capacity                 : " << gib("capacity_per_instance_bytes")
      << " GiB/instance; total=" << gib("aggregate_capacity_bytes") << " GiB\n"
      << "Host / Transaction Bytes : " << m("line_size") << " / " << m("dram_transaction_bytes") << " B\n"
      << "Policies                 : " << m("scheduler") << " / " << m("row_policy")
      << " / " << m("address_mapping") << "; Channel=" << m("channel_mapper")
      << "; Stack=" << m("stack_mapping") << '\n'
      << "Features                 : PHY=" << m("mem_phy_mode") << "; Backend=" << m("memory_backend")
      << "; Refresh=" << m("supports_refresh") << "; RFM=" << m("supports_rfm")
      << "; Payload ECC=" << m("ecc_shadow") << '\n'
      << "# ===== PARAMETERS =====\n"
      << "Interface / Clock        : " << p("data_rate_mbps") << " Mb/s/pin; "
      << p("data_bus_bits") << " bit/instance; CK=" << p("tCK_ps")
      << " ps; tick=" << p("tick_duration_ps") << " ps";
  if (lp) out << "; WCK:CK=" << p("lpddr_wck_ratio");
  out << '\n';
  if (p("input_kind") == "trace") out << "Workload                 : Trace; inject_interval=" << p("inject_interval") << " tick\n";
  else out << "Workload                 : " << p("pattern") << "; " << p("requests")
           << " requests; reads=" << p("read_ratio") << "%; inject_interval=" << p("inject_interval") << " tick\n"
           << "Address Space / Seed     : " << p("effective_random_address_space_bytes") << " B / " << p("seed")
           << " (stream uses stride=" << p("addr_stride") << " B)\n";
  out << "Read / Write Queues      : " << p("read_buffer_size") << " / " << p("write_buffer_size") << '\n'
      << "Comparison Baseline      : " << r.baseline << "; " << r.changes.size()
      << " changes (including derived values; not vendor-certified; full list: --stats-json)\n";
  // Printed identity already states these final values. The JSON change list
  // remains complete, including derived effects, with no truncation.
  std::size_t extra = 0;
  const std::regex visible(R"((architecture|organization|geometry)\.(channels|pseudo_channels|sids|ranks|bank_groups|banks_per_group|rows|columns|line_size|dram_transaction_bytes|data_rate_mbps|data_bus_bits|tCK_ps|density_gb)|system\.stack_count|controller\.(scheduler|row_policy|address_mapping|read_buffer_size|write_buffer_size))");
  for (bool timing : {true, false}) {
    for (const auto& c : r.changes) {
      if (c.key.starts_with("timing.") != timing || std::regex_match(c.key, visible)) continue;
      if (extra++ < 2) out << "Parameter Change         : " << c.key << " " << c.baseline << " -> " << c.value << '\n';
    }
  }
  out << "# ===== RESULTS =====\n"
      << "Run Status               : " << v("run_status") << '\n'
      << "Requests / Transactions  : " << s("host_requests") << " Host; completed="
      << s("completed_reads") << " reads / " << s("completed_writes")
      << " writes; submitted=" << s("dram_transactions") << " transactions\n"
      << "Remaining Work           : queued/uninjected=" << s("remaining_requests")
      << "; pending=" << s("remaining_pending") << " (mixed work count, not Host-only)\n"
      << "Simulation Time          : " << s("simulation_time_ns") << " ns\n"
      << "Bandwidth / Utilization  : " << s("achieved_bw_GBps") << " GB/s / " << s("bandwidth_util_pct") << "%\n"
      << "Read Transaction Latency : " << s("avg_read_latency_ns") << " ns\n"
      << "Row Hit Rate             : " << s("row_hit_pct") << "% (first-scheduling classification)\n"
      << "Validation               : Command=" << v("cmd_validation") << "; DFI=" << v("dfi_validation")
      << "; checked_reads=" << v("data_checked_reads") << "; data_errors=" << v("data_mismatches");
  if (v("data_checked_reads") == "0") out << " (no independent data-check evidence)";
  out << '\n';
  if (r.metrics.contains("power_energy_pJ")) out << "Energy                   : " << s("power_energy_pJ") << " pJ\n";
  if (r.metrics.contains("thermal_peak_temp_C")) out << "Mean / Peak Temperature  : " << s("thermal_avg_temp_C") << " / " << s("thermal_peak_temp_C") << " degC\n";
  if (r.metrics.contains("storage_lines_allocated")) out << "Backend Allocation       : " << s("storage_lines_allocated") << " lines; " << s("storage_bytes_allocated") << " B (data-byte accounting)\n";
  if (r.validation.contains("golden_mismatches")) out << "Golden Check             : " << v("golden_verified") << " lines; errors=" << v("golden_mismatches") << '\n';
  if (v("ecc_uncorrectable_errors") != "0" && v("ecc_uncorrectable_errors") != "N/A")
    out << "WARNING: Uncorrectable ECC errors=" << v("ecc_uncorrectable_errors") << '\n';
  const double derived = density_gbit_from_geometry(
      number(r.model, "capacity_per_instance_bytes"), lp,
      static_cast<int>(number(r.model, "stack_height")),
      static_cast<int>(number(r.model, "channels")),
      static_cast<int>(number(r.model, "pseudo_channels")),
      lp ? static_cast<int>(number(r.model, "ranks")) : 1);
  if (std::abs(derived - number(r.model, "density_gb")) > 1e-9 * std::max(1.0, derived))
    out << "WARNING: Nominal density_gb differs from geometry-derived density; simulation capacity uses geometry\n";
  if (m("memory_system") == "single_controller") out << "WARNING: Single-controller validation; organization capacity does not imply all Channels are simulated\n";
  if (r.stacks.size() > 1) {
    out << "Stack  Completed_Reads  Completed_Writes  Bandwidth(GB/s)  Read_Latency(ns)\n";
    for (const auto& st : r.stacks) out << value(st, "stack") << "  "
        << value(st, "completed_reads") << "  " << value(st, "completed_writes") << "  "
        << value(st, "achieved_bw_GBps") << "  " << value(st, "avg_read_latency_ns") << '\n';
  }
}

void write_result_json(const std::string& path, const ResultReport& r,
                       const std::string& status, const std::string& error) {
  if (path.empty()) return;
  if (status != "completed" && status != "truncated" && status != "failed")
    throw std::invalid_argument("invalid run status");
  std::ostringstream out;
  out << "{\n  \"schema_version\": 2,\n  \"run_status\": " << quote(status)
      << ",\n  \"error\": " << quote(error);
  for (auto [name, fields] : {std::pair{"model", &r.model}, {"parameters", &r.parameters},
                             {"metrics", &r.metrics}, {"validation", &r.validation}}) {
    out << ",\n  " << quote(name) << ": ";
    auto clean = *fields;
    clean.erase("run_status"); // only the envelope owns final completion status
    object(out, clean);
  }
  out << ",\n  \"comparison_baseline\": " << quote(r.baseline) << ",\n  \"changes\": [";
  bool first = true;
  for (const auto& c : r.changes) {
    out << (first ? "" : ",") << "\n    {\"key\": " << quote(c.key)
        << ", \"baseline\": " << quote(c.baseline) << ", \"value\": " << quote(c.value) << "}";
    first = false;
  }
  out << "\n  ],\n  \"stacks\": [";
  first = true;
  for (const auto& st : r.stacks) { if (!first) out << ','; object(out, st); first = false; }
  out << "\n  ]";
  if (!r.diagnostics.empty()) {
    auto diagnostic = r.diagnostics;
    diagnostic["run_status"] = status;
    out << ",\n  \"diagnostics\": ";
    object(out, diagnostic);
  }
  out << "\n}\n";
  auto parent = std::filesystem::path(path).parent_path();
  if (!parent.empty()) std::filesystem::create_directories(parent);
  std::ofstream file(path, std::ios::trunc);
  if (!file) throw std::runtime_error("cannot open result JSON: " + path);
  file << out.str();
  file.close();
  if (!file) throw std::runtime_error("cannot write result JSON: " + path);
}
}  // namespace hbm_sim
