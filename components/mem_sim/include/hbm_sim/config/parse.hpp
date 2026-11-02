#pragma once

// Strict scalar parsing shared by CLI and library model construction.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace hbm_sim::config {
inline std::uint64_t parse_u64(const std::string& value) {
  // stoull("-1") 会按无符号规则返回 UINT64_MAX，而且 stoi/stod 默认允许
  // 未解析的尾部字符。配置错误不能静默变成一个看似合法的巨大实验。
  if (value.empty() || value.front() == '-' ||
      std::any_of(value.begin(), value.end(), [](unsigned char c) {
        return std::isspace(c);
      })) {
    throw std::invalid_argument("invalid non-negative integer: " + value);
  }
  std::size_t parsed = 0;
  const auto result = std::stoull(value, &parsed, 10);
  if (parsed != value.size()) {
    throw std::invalid_argument("invalid non-negative integer: " + value);
  }
  return static_cast<std::uint64_t>(result);
}

inline int parse_int(const std::string& value) {
  if (value.empty() ||
      std::any_of(value.begin(), value.end(), [](unsigned char c) {
        return std::isspace(c);
      })) {
    throw std::invalid_argument("invalid integer: " + value);
  }
  std::size_t parsed = 0;
  const int result = std::stoi(value, &parsed, 10);
  if (parsed != value.size()) {
    throw std::invalid_argument("invalid integer: " + value);
  }
  return result;
}

inline double parse_double(const std::string& value) {
  if (value.empty() ||
      std::any_of(value.begin(), value.end(), [](unsigned char c) {
        return std::isspace(c);
      })) {
    throw std::invalid_argument("invalid finite number: " + value);
  }
  std::size_t parsed = 0;
  const double result = std::stod(value, &parsed);
  if (parsed != value.size() || !std::isfinite(result)) {
    throw std::invalid_argument("invalid finite number: " + value);
  }
  return result;
}

inline bool parse_bool(const std::string& value) {
  std::string normalized = value;
  std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  if (normalized == "1" || normalized == "true" || normalized == "yes" || normalized == "on") {
    return true;
  }
  if (normalized == "0" || normalized == "false" || normalized == "no" || normalized == "off") {
    return false;
  }
  throw std::invalid_argument("invalid bool value: " + value);
}

inline std::string lower_value(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
    if (c == '-') return '_';
    return static_cast<char>(std::tolower(c));
  });
  return value;
}
}  // namespace hbm_sim::config
