#include "hbm_sim/stats/result.hpp"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace {
void require(bool ok, const char* message) {
  if (!ok) throw std::runtime_error(message);
}
std::string read(const std::filesystem::path& path) {
  std::ifstream in(path);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}
}

int main() {
  try {
    char pattern[] = "/tmp/hbm_result_values_XXXXXX";
    const char* created = ::mkdtemp(pattern);
    if (!created) throw std::runtime_error("mkdtemp failed");
    const auto directory = std::filesystem::path(created);
    const auto path = directory / "result.json";
    hbm_sim::ResultReport r;
    hbm_sim::record_field(r.metrics, "counter", std::uint64_t{9007199254740993ULL});
    hbm_sim::record_field(r.metrics, "precise", 1.2345678901234567);
    hbm_sim::record_field(r.model, "name", std::string("a\"b\n汉字"));
    r.metrics["unavailable"] = nullptr;
    r.validation["run_status"] = std::string("completed");
    r.diagnostics["run_status"] = std::string("completed");
    hbm_sim::write_result_json(path.string(), r, "failed", "late output failure");
    const auto text = read(path);
    require(text.find("9007199254740993") != std::string::npos, "64-bit integer rounded");
    require(text.find("1.2345678901234567") != std::string::npos, "double rounded");
    require(text.find("a\\\"b\\u000a汉字") != std::string::npos, "JSON escaping failed");
    require(text.find("\"unavailable\": null") != std::string::npos, "null became zero");
    require(text.find("completed") == std::string::npos, "late error left success status");
    hbm_sim::record_field(r.metrics, "bad", std::numeric_limits<double>::infinity());
    bool rejected = false;
    try { hbm_sim::write_result_json(path.string(), r, "completed"); }
    catch (const std::runtime_error&) { rejected = true; }
    require(rejected && read(path) == text, "invalid value damaged previous result");
    const auto changes = hbm_sim::compare_resolved_parameters(
        "[architecture]\ncolumns=32\n[timing.jedec]\nsource=jedec\nnCL=10\n",
        "[architecture]\ncolumns=16\n[timing.vendor]\nsource=vendor\nnCL=10\n");
    require(changes.size() == 1 && changes.front().key == "architecture.columns",
            "source relabel treated as numerical change");
    const auto power_changes = hbm_sim::compare_resolved_parameters(
        "[power]\nsource=configured_pj\n", "[power]\nsource=dramsim3_idd\n");
    require(power_changes.size() == 1 && power_changes.front().key == "power.source",
            "power formula selection mistaken for an audit-only label");
    std::filesystem::remove(path);
    std::filesystem::remove(directory);
    std::cout << "typed result tests passed\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
