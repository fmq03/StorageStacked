#include "hbm_sim/config/document.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <tuple>
#include <unordered_set>

namespace hbm_sim::config {
namespace {

std::string trim(std::string value) {
  auto not_space = [](unsigned char c) { return !std::isspace(c); };
  value.erase(value.begin(), std::find_if(value.begin(), value.end(), not_space));
  value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(), value.end());
  return value;
}

std::vector<std::string> split_section(const std::string& section) {
  std::vector<std::string> parts;
  std::stringstream stream(section);
  std::string part;
  while (std::getline(stream, part, '.')) {
    parts.push_back(part);
  }
  return parts;
}

bool is_common_section(const std::string& section) {
  static const std::unordered_set<std::string> sections{
      "common", "system", "workload", "architecture", "timing", "protocol", "controller", "controller.scheduler",
      "controller.row_policy", "mapping", "phy", "maintenance", "storage",
      "power", "thermal", "reliability", "reliability.payload",
      "fault_injection", "outputs", "validation", "frontend", "memory_system",
      "dram.organization", "dram.timing", "dram.protocol", "controller.refresh",
      "controller.addr_mapper", "audit"};
  return sections.contains(section) || section.starts_with("timing.");
}

int section_layer(const std::string& section,
                  const Selection& selection,
                  bool& active) {
  active = true;
  // v2 配置的覆盖方向与“默认 -> 特化 -> 实验覆盖”一致：
  // common 提供两种协议族都能理解的工程默认，family/standard/preset 逐层
  // 特化，override 最后表达本次实验的偏离。这样继承式多 Stack 用例才能真正
  // 覆盖 [system] 中的单 Stack 默认，而不是被公共段反向覆盖。
  if (section.empty()) return 10;
  if (section == "meta" || section == "model") return 0;
  if (section == "override") return 50;
  if (is_common_section(section)) return 10;

  const auto parts = split_section(section);
  if (parts.size() == 2 && parts[0] == "family") {
    const bool hbm = selection.standard == "hbm3" || selection.standard == "hbm4";
    active = (parts[1] == "hbm" && hbm) || (parts[1] == "lpddr" && !hbm);
    return 20;
  }
  if (parts.size() >= 2 && parts[0] == "standard") {
    active = canonical_standard(parts[1]) == selection.standard;
    return 30;
  }
  if (parts.size() >= 3 && parts[0] == "preset") {
    active = canonical_standard(parts[1]) == selection.standard &&
             normalize_name(parts[2]) == normalize_name(selection.preset);
    return 40;
  }
  throw std::runtime_error("unknown config section [" + section + "]");
}

bool is_selector_entry(const ConfigEntry& entry) {
  if (!entry.section.empty() && entry.section != "model") return false;
  const std::string key = normalize_name(entry.key);
  return key == "standard" || key == "base_standard" || key == "preset";
}

std::string canonical_entry_key(const ConfigEntry& entry) {
  const std::string& section = entry.section;
  const std::string& key = entry.key;
  if (key == "timing_source") return "timing_override_source";
  if (section == "model") {
    if (key == "name") return "model_name";
    if (key == "base_standard") return "standard";
  }
  if (section == "validation" && key == "mode") return "validation_mode";
  if (section == "controller.scheduler" && key == "type") return "scheduler";
  if (section == "controller.row_policy") {
    if (key == "type") return "row_policy";
    if (key == "cap") return "row_policy_cap";
  }
  if (section == "phy") {
    if (key == "mode") return "mem_phy_mode";
    if (key == "protocol") return "phy_protocol";
    if (key == "command_fifo_depth") return "phy_command_fifo_depth";
    if (key == "read_fifo_depth") return "phy_read_fifo_depth";
    if (key == "write_fifo_depth") return "phy_write_fifo_depth";
    if (key == "command_pipeline_cycles") return "phy_command_pipeline_cycles";
    if (key == "read_return_pipeline_cycles") return "phy_read_return_pipeline_cycles";
    if (key == "write_data_pipeline_cycles") return "phy_write_data_pipeline_cycles";
    if (key == "reset_cycles") return "phy_reset_cycles";
    if (key == "initialization_cycles") return "phy_initialization_cycles";
    if (key == "training_cycles") return "phy_training_cycles";
    if (key == "auto_train") return "phy_auto_train";
  }
  if (section == "storage") {
    if (key == "backend") return "memory_backend";
    if (key == "capacity_bytes") return "memory_capacity_bytes";
    if (key == "data_file") return "memory_data_file";
    if (key == "init_file") return "memory_init_file";
    if (key == "meta_file") return "memory_meta_file";
    if (key == "presence_file") return "memory_presence_file";
    if (key == "chunk_size") return "memory_chunk_size";
    if (key == "chunk_cache_entries") return "memory_chunk_cache_entries";
  }
  if (section == "power") {
    if (key == "enabled") return "power_model";
    if (key == "source") return "power_source";
    if (key == "scale") return "power_scale";
  }
  if (section == "thermal") {
    if (key == "enabled") return "thermal_model";
    if (key == "ambient_c") return "thermal_ambient_c";
    if (key == "cooling_per_cycle") return "thermal_cooling_per_cycle";
    if (key == "rise_c_per_pj") return "thermal_rise_c_per_pj";
    if (key == "lateral_coupling") return "thermal_lateral_coupling";
    if (key == "vertical_coupling") return "thermal_vertical_coupling";
  }
  const auto parts = split_section(section);
  if (section == "timing" || section.starts_with("timing.") ||
      (!parts.empty() && parts.back().starts_with("timing"))) {
    if (key == "source") return "timing_override_source";
    if (key == "reference") return "parameter_reference";
  }
  return key;
}

}  // namespace

const char* to_string(ValidationMode mode) {
  switch (mode) {
    case ValidationMode::Exploratory: return "exploratory";
    case ValidationMode::Standard: return "standard";
    case ValidationMode::Device: return "device";
  }
  return "exploratory";
}

ValidationMode parse_validation_mode(const std::string& value) {
  const std::string normalized = normalize_name(value);
  if (normalized == "exploratory" || normalized == "research") {
    return ValidationMode::Exploratory;
  }
  if (normalized == "standard" || normalized == "jedec") {
    return ValidationMode::Standard;
  }
  if (normalized == "device" || normalized == "vendor" || normalized == "calibrated") {
    return ValidationMode::Device;
  }
  throw std::invalid_argument(
      "invalid validation_mode: " + value +
      " (implemented: exploratory, standard, device)");
}

std::string normalize_name(std::string value) {
  value = trim(std::move(value));
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
    if (c == '-') return '_';
    return static_cast<char>(std::tolower(c));
  });
  return value;
}

std::string canonical_standard(std::string value) {
  value = normalize_name(std::move(value));
  if (value == "hbm") return "hbm4";
  if (value == "lpddr" || value == "ldppr") return "lpddr6";
  return value;
}

ConfigDocument load_document(const std::string& path) {
  std::ifstream input(path);
  if (!input) throw std::runtime_error("failed to open config: " + path);

  ConfigDocument document;
  document.path = path;
  std::string section;
  std::set<std::pair<std::string, std::string>> seen_section_keys;
  std::string line;
  std::size_t lineno = 0;
  while (std::getline(input, line)) {
    ++lineno;
    const std::size_t comment = line.find('#');
    if (comment != std::string::npos) line.resize(comment);
    line = trim(std::move(line));
    if (line.empty()) continue;

    if (line.front() == '[') {
      if (line.size() < 3 || line.back() != ']') {
        throw std::runtime_error(path + ":" + std::to_string(lineno) +
                                 ": malformed config section");
      }
      section = normalize_name(trim(line.substr(1, line.size() - 2)));
      if (section.empty()) {
        throw std::runtime_error(path + ":" + std::to_string(lineno) +
                                 ": empty config section");
      }
      document.sectioned = true;
      continue;
    }

    const std::size_t eq = line.find('=');
    if (eq == std::string::npos) {
      throw std::runtime_error(path + ":" + std::to_string(lineno) +
                               ": config entry is missing '='");
    }
    ConfigEntry entry;
    entry.key = normalize_name(trim(line.substr(0, eq)));
    entry.value = trim(line.substr(eq + 1));
    entry.section = section;
    entry.path = path;
    entry.line = lineno;
    if (entry.key.empty() || entry.value.empty()) {
      throw std::runtime_error(path + ":" + std::to_string(lineno) +
                               ": config key and value must not be empty");
    }
    const auto identity = std::make_pair(entry.section, entry.key);
    if (!seen_section_keys.insert(identity).second) {
      throw std::runtime_error(path + ":" + std::to_string(lineno) +
                               ": duplicate key '" + entry.key + "' in section [" +
                               entry.section + "]");
    }
    if (entry.section == "meta" && entry.key == "extends") {
      document.extends.push_back(entry.value);
      continue;
    }
    document.entries.push_back(std::move(entry));
  }
  return document;
}

std::vector<ConfigDocument> load_document_tree(const std::string& path) {
  namespace fs = std::filesystem;
  std::vector<ConfigDocument> documents;
  std::set<std::string> active;

  auto visit = [&](auto&& self, const fs::path& input_path) -> void {
    std::error_code error;
    fs::path normalized = fs::weakly_canonical(input_path, error);
    if (error) normalized = fs::absolute(input_path).lexically_normal();
    const std::string identity = normalized.string();
    if (!active.insert(identity).second) {
      throw std::runtime_error("config inheritance cycle detected at: " + identity);
    }

    ConfigDocument document = load_document(identity);
    for (const auto& base : document.extends) {
      fs::path base_path(base);
      if (base_path.is_relative()) base_path = normalized.parent_path() / base_path;
      self(self, base_path);
    }
    documents.push_back(std::move(document));
    active.erase(identity);
  };

  visit(visit, fs::path(path));
  return documents;
}

Selection discover_selection(const std::vector<ConfigDocument>& documents,
                             const std::string& cli_standard,
                             const std::string& cli_preset) {
  Selection selection;
  for (const auto& document : documents) {
    for (const auto& entry : document.entries) {
      if (!is_selector_entry(entry)) continue;
      const std::string key = normalize_name(entry.key);
      if (key == "standard" || key == "base_standard") {
        selection.standard = canonical_standard(entry.value);
      } else if (key == "preset") {
        selection.preset = normalize_name(entry.value);
      }
    }
  }
  if (!cli_standard.empty()) selection.standard = canonical_standard(cli_standard);
  if (!cli_preset.empty()) selection.preset = normalize_name(cli_preset);
  return selection;
}

std::vector<ConfigEntry> resolve_document(const ConfigDocument& document,
                                          const Selection& selection) {
  std::map<std::string, std::string> section_sources;
  for (const auto& entry : document.entries) {
    const auto key = canonical_entry_key(entry);
    if (key == "timing_source" || key == "timing_override_source")
      section_sources[entry.section] = entry.value;
  }
  auto bind_source = [&](ConfigEntry& entry) {
    if (const auto it = section_sources.find(entry.section); it != section_sources.end())
      entry.timing_source = it->second;
  };
  if (!document.sectioned) {
    std::vector<ConfigEntry> entries = document.entries;
    for (auto& entry : entries) { entry.layer = 40; bind_source(entry); }
    return entries;
  }

  std::vector<std::pair<std::size_t, ConfigEntry>> staged;
  staged.reserve(document.entries.size());
  for (std::size_t index = 0; index < document.entries.size(); ++index) {
    ConfigEntry entry = document.entries[index];
    bind_source(entry);
    bool active = true;
    entry.layer = section_layer(entry.section, selection, active);
    if (active) staged.emplace_back(index, std::move(entry));
  }
  std::stable_sort(staged.begin(), staged.end(), [](const auto& lhs, const auto& rhs) {
    return std::tie(lhs.second.layer, lhs.first) < std::tie(rhs.second.layer, rhs.first);
  });

  std::vector<ConfigEntry> resolved;
  resolved.reserve(staged.size());
  std::map<std::pair<int, std::string>, ConfigEntry> seen_semantic_keys;
  for (auto& [index, entry] : staged) {
    (void)index;
    entry.key = canonical_entry_key(entry);
    // timing source/reference 是 subsection-local provenance：同一 standard 的
    // timing_jedec 与 timing_research 可以各自声明 source，而数值键仍必须在
    // 相同 layer 唯一。其他 section alias（例如 type/scheduler）则按 canonical
    // key 检测语义重复。
    std::string semantic_key = entry.key;
    if (entry.key == "timing_override_source" ||
        entry.key == "parameter_reference") {
      semantic_key = entry.section + ":" + entry.key;
    }
    const auto identity = std::make_pair(entry.layer, semantic_key);
    const auto [it, inserted] = seen_semantic_keys.emplace(identity, entry);
    if (!inserted) {
      const ConfigEntry& previous = it->second;
      throw std::runtime_error(
          entry.path + ":" + std::to_string(entry.line) +
          ": semantic duplicate key '" + entry.key + "' at layer " +
          std::to_string(entry.layer) + " (already set by " +
          previous.path + ":" + std::to_string(previous.line) + ")");
    }
    resolved.push_back(std::move(entry));
  }
  return resolved;
}

std::vector<ConfigEntry> resolve_documents(const std::vector<ConfigDocument>& documents,
                                           const Selection& selection) {
  std::vector<std::pair<std::size_t, ConfigEntry>> staged;
  std::size_t sequence = 0;
  for (const auto& document : documents) {
    for (auto& entry : resolve_document(document, selection)) {
      staged.emplace_back(sequence++, std::move(entry));
    }
  }
  std::stable_sort(staged.begin(), staged.end(), [](const auto& lhs, const auto& rhs) {
    return std::tie(lhs.second.layer, lhs.first) < std::tie(rhs.second.layer, rhs.first);
  });

  std::vector<ConfigEntry> resolved;
  resolved.reserve(staged.size());
  for (auto& [index, entry] : staged) {
    (void)index;
    resolved.push_back(std::move(entry));
  }
  return resolved;
}

std::vector<std::string> list_presets(const ConfigDocument& document,
                                      const std::string& standard) {
  const std::string canonical = canonical_standard(standard);
  std::set<std::string> presets;
  for (const auto& entry : document.entries) {
    const auto parts = split_section(entry.section);
    if (parts.size() < 3 || parts[0] != "preset" ||
        canonical_standard(parts[1]) != canonical) {
      continue;
    }
    presets.insert(parts[2]);
  }
  return {presets.begin(), presets.end()};
}

}  // namespace hbm_sim::config
