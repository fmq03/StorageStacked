#pragma once

#include <cstdint>
#include <map>
#include <ostream>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

namespace hbm_sim {
struct Stats;
using ResultValue = std::variant<std::nullptr_t, bool, std::int64_t,
                                 std::uint64_t, double, std::string>;
using ResultFields = std::map<std::string, ResultValue>;

// Typed values precede formatting; integers never pass through double.
template<class T>
void record_field(ResultFields& fields, const std::string& key, const T& value) {
  if constexpr (std::is_same_v<T, std::nullptr_t>) fields[key] = nullptr;
  else if constexpr (std::is_same_v<T, bool>) fields[key] = value;
  else if constexpr (std::is_integral_v<T> && std::is_signed_v<T>)
    fields[key] = static_cast<std::int64_t>(value);
  else if constexpr (std::is_integral_v<T>) fields[key] = static_cast<std::uint64_t>(value);
  else if constexpr (std::is_floating_point_v<T>) fields[key] = static_cast<double>(value);
  else fields[key] = std::string(value);
}

struct ParameterChange { std::string key, baseline, value; };
struct ResultReport {
  ResultFields model, parameters, metrics, validation;
  std::vector<ResultFields> stacks;
  std::vector<ParameterChange> changes;
  std::string baseline;
  // Explicit diagnostics only, outside the public metric contract.
  ResultFields diagnostics;
};

ResultFields collect_stats(const Stats& stats);
ResultReport make_result_report(const ResultFields& fields);
std::vector<ParameterChange> compare_resolved_parameters(
    const std::string& baseline, const std::string& effective);
std::string result_value_text(const ResultValue& value);
void print_diagnostics(std::ostream& out, const ResultFields& fields);
void print_result(std::ostream& out, const ResultReport& report);
void write_result_json(const std::string& path, const ResultReport& report,
                       const std::string& run_status,
                       const std::string& error = {});
}  // namespace hbm_sim
