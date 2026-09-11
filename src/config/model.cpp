// Auditable model construction shared by CLI and external library frontends.
// Document loading/layering is separate from DRAM construction and execution.
#include "hbm_sim/config/model.hpp"
#include "hbm_sim/config/parse.hpp"
#include "hbm_sim/config/fields.hpp"
#include "hbm_sim/dram/profiles.hpp"
#include "hbm_sim/dram/jedec.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <stdexcept>
#include <unordered_set>
#include <set>
#include <map>
#include <limits>
#include <iomanip>
#include <sstream>

namespace hbm_sim::config {
namespace {
// 配置文件中的 timing 覆盖值不总是 vendor 数据：校准模板经常会显式写出
// research_default 数字，方便审计和替换。来源绑定到具体 timing 字段，
// 不依赖 source 在 section 内的书写位置。
hbm_sim::TimingValueSource parse_timing_value_source(std::string value) {
  value = lower_value(std::move(value));
  if (value == "jedec" || value == "standard") return hbm_sim::TimingValueSource::JEDEC;
  if (value == "vendor" || value == "datasheet") return hbm_sim::TimingValueSource::Vendor;
  if (value == "derived") return hbm_sim::TimingValueSource::Derived;
  if (value == "external_reference" || value == "reference" || value == "ramulator2" ||
      value == "dramsim3") return hbm_sim::TimingValueSource::ExternalReference;
  if (value == "research" || value == "research_default") return hbm_sim::TimingValueSource::ResearchDefault;
  throw std::invalid_argument("invalid timing source: " + value);
}

hbm_sim::AddressMappingKind parse_address_mapping(std::string value) {
  value = lower_value(std::move(value));
  if (value == "default" || value == "legacy") return hbm_sim::AddressMappingKind::Default;
  if (value == "robaracoch" || value == "ro_ba_ra_co_ch") return hbm_sim::AddressMappingKind::RoBaRaCoCh;
  if (value == "chrabaroco" || value == "ch_ra_ba_ro_co") return hbm_sim::AddressMappingKind::ChRaBaRoCo;
  if (value == "rocorabach" || value == "ro_co_ra_ba_ch") return hbm_sim::AddressMappingKind::RoCoRaBaCh;
  throw std::invalid_argument("invalid address_mapping: " + value);
}

hbm_sim::LpddrEfficiencyMode parse_lpddr_efficiency(std::string value) {
  value = lower_value(std::move(value));
  if (value == "normal" || value == "off") return hbm_sim::LpddrEfficiencyMode::Normal;
  if (value == "static" || value == "seff") return hbm_sim::LpddrEfficiencyMode::Static;
  if (value == "dynamic" || value == "deff") return hbm_sim::LpddrEfficiencyMode::Dynamic;
  throw std::invalid_argument("invalid lpddr_efficiency_mode: " + value);
}

hbm_sim::MaintenancePolicyKind parse_maintenance_policy(std::string value) {
  value = lower_value(std::move(value));
  if (value == "per_bank" || value == "perbank" || value == "pb") {
    return hbm_sim::MaintenancePolicyKind::PerBank;
  }
  if (value == "all_bank" || value == "allbank" || value == "ab") {
    return hbm_sim::MaintenancePolicyKind::AllBank;
  }
  throw std::invalid_argument("invalid maintenance policy: " + value);
}

hbm_sim::LpddrDvfsMode parse_lpddr_dvfs_mode(std::string value) {
  value = lower_value(std::move(value));
  if (value == "nominal" || value == "on") return hbm_sim::LpddrDvfsMode::Nominal;
  if (value == "low" || value == "low_power") return hbm_sim::LpddrDvfsMode::Low;
  if (value == "disabled" || value == "off") return hbm_sim::LpddrDvfsMode::Disabled;
  throw std::invalid_argument("invalid lpddr_dvfs_mode: " + value);
}

hbm_sim::LpddrWckMode parse_lpddr_wck_mode(std::string value) {
  value = lower_value(std::move(value));
  if (value == "cas_sync" || value == "cas") return hbm_sim::LpddrWckMode::CasSync;
  if (value == "always_on" || value == "alwayson") return hbm_sim::LpddrWckMode::AlwaysOn;
  throw std::invalid_argument(
      "invalid or unimplemented lpddr_wck_mode: " + value +
      " (implemented: cas_sync, always_on)");
}

hbm_sim::LowPowerMode parse_low_power_mode(std::string value) {
  value = lower_value(std::move(value));
  if (value == "off" || value == "none") return hbm_sim::LowPowerMode::Off;
  if (value == "power_down" || value == "powerdown" || value == "pd") return hbm_sim::LowPowerMode::PowerDown;
  if (value == "self_refresh" || value == "selfrefresh" || value == "sr") return hbm_sim::LowPowerMode::SelfRefresh;
  throw std::invalid_argument("invalid low_power_mode: " + value);
}

hbm_sim::RefreshTemperatureMode parse_refresh_temperature_mode(std::string value) {
  value = lower_value(std::move(value));
  if (value == "normal" || value == "1x") return hbm_sim::RefreshTemperatureMode::Normal;
  if (value == "high" || value == "2x") return hbm_sim::RefreshTemperatureMode::High;
  if (value == "extended" || value == "ext") return hbm_sim::RefreshTemperatureMode::Extended;
  throw std::invalid_argument("invalid refresh_temperature_mode: " + value);
}



}  // namespace
bool is_spec_override_key(const std::string& key) {
  if (key.starts_with("timing_source."))
    return is_timing_override_key(key.substr(14));
  // 配置文件允许把 DramSpec 中的组织结构、开关和 timing 直接覆盖。
  // 这里显式列白名单，而不是把未知 key 静默塞进 spec_overrides，原因有两个：
  // 1. JEDEC/vendor timing 名称很多，拼错一个字符就会让数值对比失真；
  // 2. CLI 仍处在小型研究工具阶段，明确报错比“容忍但忽略”更容易定位配置问题。
  if (find_model_field(key) || find_timing_field(key)) return true;
  static const char* keys[] = {
      "timing_source",
      "timing_override_source",
      "dfi_read_latency_ns",
      "dfi_write_latency_ns",
      "refresh_policy",
      "rfm_policy",
      "hbm_sid_interleave",
      "lpddr_dynamic_efficiency",
      "lpddr_efficiency_mode",
      "lpddr_dvfs_mode",
      "lpddr_wck_mode",
      "low_power_mode",
      "refresh_temperature_mode",
      "address_mapping",
      "addr_mapping"};
  return std::find(std::begin(keys), std::end(keys), key) != std::end(keys);
}

bool is_timing_override_key(const std::string& key) {
  return !canonical_timing_name_for_key(key).empty();
}

static void apply_resolved_overrides(hbm_sim::DramSpec& spec,
                                    const ModelOverrides& overrides) {
  // 第一遍只收集会影响 profile 展开的选择项。组织/timing 的显式覆盖留到
  // profile 之后，保证最终优先级固定为 traits < profile < config/CLI。
  for (const auto& [key, value] : overrides) {
    if (const auto* field = find_model_field(key); field && field->profile_selector) {
      field->set(spec, value);
    } else if (key == "lpddr_dvfs_mode") {
      spec.lpddr_dvfs_mode = parse_lpddr_dvfs_mode(value);
    } else if (key == "lpddr_efficiency_mode") {
      spec.lpddr_efficiency_mode = parse_lpddr_efficiency(value);
    } else if (key == "lpddr_dynamic_efficiency") {
      spec.lpddr_dynamic_efficiency = parse_bool(value);
      if (spec.lpddr_dynamic_efficiency) {
        spec.lpddr_efficiency_mode = hbm_sim::LpddrEfficiencyMode::Dynamic;
      }
    } else if (key == "lpddr_wck_mode") {
      spec.lpddr_wck_mode = parse_lpddr_wck_mode(value);
    } else if (key == "refresh_temperature_mode") {
      spec.refresh_temperature_mode = parse_refresh_temperature_mode(value);
    } else if (key == "low_power_mode") {
      spec.low_power_mode = parse_low_power_mode(value);
    } else if (key == "hbm_sid_interleave") {
      spec.hbm_sid_interleave = parse_bool(value);
    }
  }

  double profile_tck = 0;
  for (const auto& [key, value] : overrides)
    if (key == "tck_ps") profile_tck = parse_double(value);
  hbm_sim::apply_standard_timing_profile(spec, profile_tck);

  // ns/us 覆盖项依赖最终 tCK。显式 tCK 的优先级高于 profile，并且与配置
  // 文件中的书写顺序无关。
  for (const auto& [key, value] : overrides) {
    if (key == "tck_ps") {
      spec.timing.tCK_ps = parse_double(value);
    }
  }

  hbm_sim::TimingValueSource default_source = hbm_sim::TimingValueSource::ResearchDefault;
  std::map<std::string, hbm_sim::TimingValueSource> field_sources;
  for (const auto& [key, value] : overrides) {
    if (key == "timing_source" || key == "timing_override_source")
      default_source = parse_timing_value_source(value);
    else if (key.starts_with("timing_source.")) {
      const auto name = canonical_timing_name_for_key(key.substr(14));
      if (name.empty()) throw std::invalid_argument("unknown timing source target: " + key);
      field_sources[name] = parse_timing_value_source(value);
    }
  }
  for (const auto& [key, value] : overrides) {
    const std::string timing_name = canonical_timing_name_for_key(key);
    if (key == "timing_source" || key == "timing_override_source" || key.starts_with("timing_source.")) {
      continue;
    }
    const auto timing_override_source = field_sources.contains(timing_name)
        ? field_sources.at(timing_name) : default_source;
    if (assign_model_field(spec, key, value) || assign_timing_field(spec, key, value)) {
      // Field registry supplies typed assignment; provenance is applied below.
    }
    else if (key == "dfi_read_latency_ns") spec.dfi_read_latency_nck = hbm_sim::jedec::ns_to_nck(parse_double(value), spec.timing.tCK_ps);
    else if (key == "dfi_write_latency_ns") spec.dfi_write_latency_nck = hbm_sim::jedec::ns_to_nck(parse_double(value), spec.timing.tCK_ps);
    else if (key == "refresh_policy") spec.refresh_policy = parse_maintenance_policy(value);
    else if (key == "rfm_policy") spec.rfm_policy = parse_maintenance_policy(value);
    else if (key == "hbm_sid_interleave") {
      spec.hbm_sid_interleave = parse_bool(value);
      spec.column_bus_scope = spec.hbm_sid_interleave ? hbm_sim::TimingScope::Sid
                                                      : hbm_sim::TimingScope::PseudoChannel;
    }
    else if (key == "lpddr_dynamic_efficiency") {
      spec.lpddr_dynamic_efficiency = parse_bool(value);
      if (spec.lpddr_dynamic_efficiency) spec.lpddr_efficiency_mode = hbm_sim::LpddrEfficiencyMode::Dynamic;
    }
    else if (key == "lpddr_efficiency_mode") spec.lpddr_efficiency_mode = parse_lpddr_efficiency(value);
    else if (key == "lpddr_dvfs_mode") spec.lpddr_dvfs_mode = parse_lpddr_dvfs_mode(value);
    else if (key == "lpddr_wck_mode") spec.lpddr_wck_mode = parse_lpddr_wck_mode(value);
    else if (key == "low_power_mode") spec.low_power_mode = parse_low_power_mode(value);
    else if (key == "refresh_temperature_mode") spec.refresh_temperature_mode = parse_refresh_temperature_mode(value);
    else if (key == "address_mapping" || key == "addr_mapping") spec.address_mapping = parse_address_mapping(value);
    else throw std::invalid_argument("unknown spec override key: " + key);

    if (!timing_name.empty()) {
      hbm_sim::set_timing_source(
          spec,
          timing_name,
          timing_override_source,
          "Overridden by config/CLI as " + std::string(hbm_sim::to_string(timing_override_source)) + ".");
    }
  }
  if (spec.supports_ecc && spec.hbm_ecc_bits_per_request == 0 && spec.ecc_bits_per_request == 0 &&
      !spec.lpddr_family) {
    spec.hbm_ecc_bits_per_request = spec.standard == hbm_sim::DramStandard::Hbm4 ? 16 : 64;
  }
  if (spec.lpddr_link_protection && spec.lpddr_link_ecc_bits_per_request == 0) {
    spec.lpddr_link_ecc_bits_per_request = 16;
  }
  if (spec.lpddr_dbi_enabled && spec.lpddr_dbi_bits_per_request == 0) {
    spec.lpddr_dbi_bits_per_request = 8;
  }
}


ResolvedModelInputs resolve_coupled_inputs(const DramSpec& baseline,
                                         const ModelOverrides& inputs) {
  std::map<std::string, std::string> explicit_values;
  for (const auto& [key, value] : inputs) explicit_values[key] = value;
  std::map<std::string, std::string> timing_spellings;
  for (const auto& [key, value] : explicit_values) {
    const auto timing = canonical_timing_name_for_key(key);
    if (timing.empty()) continue;
    auto [position, inserted] = timing_spellings.emplace(timing, key);
    if (!inserted && position->second != key)
      throw std::invalid_argument("ambiguous timing inputs for " + timing + ": " +
          position->second + " and " + key + "; use one nCK/ns spelling");
  }
  auto automatic = [&](const std::string& key) {
    const auto it = explicit_values.find(key);
    return it == explicit_values.end() || lower_value(it->second) == "auto";
  };
  auto positive_int = [&](const std::string& key, int fallback) {
    const int value = automatic(key) ? fallback : parse_int(explicit_values.at(key));
    if (value <= 0) throw std::invalid_argument(key + " must be > 0");
    return value;
  };
  auto format = [](double value) {
    std::ostringstream out;
    out << std::setprecision(17) << value;
    return out.str();
  };
  ResolvedModelInputs result;
  // Only documented dependent fields accept auto. A typo or a meaningless auto
  // on a protocol/algorithm field must reach the normal strict parser and fail.
  const std::set<std::string> dependent{
      "speed_bin_mbps", "data_rate_mbps", "data_bus_bits", "density_gb", "tck_ps", "sids"};
  for (const auto& [key, value] : inputs) {
    if (!dependent.contains(key)) result.overrides.emplace_back(key, value);
  }
  auto derive = [&](const std::string& key, double value, const std::string& formula,
                    double tolerance = 1e-9) {
    if (!std::isfinite(value) || value <= 0)
      throw std::invalid_argument("invalid derived " + key);
    if (!automatic(key)) {
      const auto given = parse_double(explicit_values.at(key));
      if (std::abs(given - value) > tolerance * std::max(1.0, std::abs(value)))
        throw std::invalid_argument(key + "=" + explicit_values.at(key) +
                                    " conflicts with derived " + format(value) +
                                    " (" + formula + "); omit it or use auto");
      result.overrides.emplace_back(key, explicit_values.at(key));
    } else {
      result.overrides.emplace_back(key, format(value));
      result.derived.push_back({key, format(value), formula});
    }
  };

  const bool low_dvfs = baseline.standard == DramStandard::Lpddr6 &&
      explicit_values.contains("lpddr_dvfs_mode") &&
      parse_lpddr_dvfs_mode(explicit_values.at("lpddr_dvfs_mode")) == LpddrDvfsMode::Low;
  const int default_rate = low_dvfs
      ? positive_int("lpddr_low_data_rate_mbps", baseline.lpddr_low_data_rate_mbps)
      : baseline.data_rate_mbps;
  const int rate = !automatic("data_rate_mbps")
                       ? positive_int("data_rate_mbps", baseline.data_rate_mbps)
                       : positive_int("speed_bin_mbps", default_rate);
  if (low_dvfs && rate != default_rate)
    throw std::invalid_argument("low DVFS data_rate_mbps must match lpddr_low_data_rate_mbps");
  derive("data_rate_mbps", rate, "selected data rate in Mb/s/pin");
  derive("speed_bin_mbps", rate, "data_rate_mbps (timing speed selector)");
  const int ratio = baseline.standard == DramStandard::Lpddr5
                        ? positive_int("lpddr_wck_ratio", 4) : 2;
  if (baseline.standard == DramStandard::Lpddr6 &&
      !automatic("lpddr_wck_ratio") && positive_int("lpddr_wck_ratio", 2) != 2)
    throw std::invalid_argument("LPDDR6 implements WCK:CK=2:1, not this ratio");
  if (baseline.standard == DramStandard::Lpddr5 && ratio != 2 && ratio != 4)
    throw std::invalid_argument("LPDDR5 WCK:CK must be 2:1 or 4:1");
  // HBM data transfer has four edges per CK; LPDDR uses DDR WCK.
  const double tck_ps = 2000000.0 * ratio / rate;
  // Legacy tables round tCK to integer ps. Explicit values may use that rounding;
  // larger contradictions are rejected instead of creating different time bases.
  derive("tck_ps", tck_ps, "2e6 * WCK_ratio / data_rate (HBM: 4e6/data_rate)",
         0.5 / std::max(1.0, tck_ps));

  DramSpec geometry = baseline;
  auto& org = geometry.org;
  org.channels = positive_int("channels", org.channels);
  org.pseudo_channels = positive_int("pseudo_channels", org.pseudo_channels);
  if (!automatic("sids")) {
    // An explicit SID is an independent research-geometry input, not a value
    // to overwrite with the default height mapping.
    org.sids = positive_int("sids", org.sids);
  } else if (explicit_values.contains("sids") || explicit_values.contains("stack_height")) {
    if (baseline.lpddr_family) {
      org.sids = 1;
    } else {
      const int height = positive_int("stack_height", baseline.stack_height);
      // Supported HBM organization convention, not arbitrary-height rounding
      // or a claim that every corresponding device/timing table is available.
      if (height != 4 && height != 8 && height != 12 && height != 16)
        throw std::invalid_argument("automatic HBM sids requires stack_height 4, 8, 12 or 16; "
                                    "set explicit sids for a research organization");
      org.sids = height / 4;
    }
    result.derived.push_back({"sids", std::to_string(org.sids),
        baseline.lpddr_family ? "LPDDR neutral SID = 1"
                              : "supported HBM organization: stack_height / 4"});
  }
  // Without a height/SID input, preserve an already-configured baseline's
  // geometry (e.g. a library caller applying only a timing override).
  org.ranks = positive_int("ranks", org.ranks);
  org.bank_groups = positive_int("bank_groups", org.bank_groups);
  org.banks_per_group = positive_int("banks_per_group", org.banks_per_group);
  org.rows = positive_int("rows", org.rows);
  org.columns = positive_int("columns", org.columns);
  org.line_size = positive_int("line_size", org.line_size);
  if (!automatic("dram_transaction_bytes")) {
    org.dram_transaction_bytes = parse_int(explicit_values.at("dram_transaction_bytes"));
    if (org.dram_transaction_bytes < 0)
      throw std::invalid_argument("dram_transaction_bytes must be >= 0");
  }
  const auto capacity = geometry.addressable_capacity_bytes();
  if (capacity == 0) throw std::invalid_argument("DRAM geometry capacity overflow");
  // Freeze the very geometry used above. Profile expansion must not silently
  // replace the resolved SID/row dimensions when stack_height or speed changes.
  for (const auto& [key, value] : std::initializer_list<std::pair<const char*, int>>{
           {"channels", org.channels}, {"pseudo_channels", org.pseudo_channels},
           {"sids", org.sids}, {"ranks", org.ranks}, {"bank_groups", org.bank_groups},
           {"banks_per_group", org.banks_per_group}, {"rows", org.rows},
           {"columns", org.columns}, {"line_size", org.line_size},
           {"dram_transaction_bytes", org.dram_transaction_bytes}})
    result.overrides.emplace_back(key, std::to_string(value));
  const double density = density_gbit_from_geometry(
      static_cast<double>(capacity), baseline.lpddr_family,
      baseline.lpddr_family ? 0 : positive_int("stack_height", baseline.stack_height),
      org.channels, org.pseudo_channels, org.ranks);
  derive("density_gb", density,
         baseline.lpddr_family
             ? "geometry_bytes * 8 / 2^30 / (channels * subchannels * ranks)"
             : "geometry_bytes * 8 / 2^30 / stack_height");
  const auto base_lanes = static_cast<std::int64_t>(baseline.org.channels) *
                          baseline.org.pseudo_channels;
  if (base_lanes <= 0 || baseline.data_bus_bits % base_lanes != 0)
    throw std::invalid_argument("baseline interface width is not divisible by channels * PC");
  const auto lanes = static_cast<std::uint64_t>(org.channels) * org.pseudo_channels;
  const auto dq_bits = baseline.data_bus_bits / base_lanes;
  if (dq_bits <= 0 || lanes > static_cast<std::uint64_t>(std::numeric_limits<int>::max() / dq_bits))
    throw std::invalid_argument("derived data_bus_bits overflow");
  const auto width = lanes * dq_bits;
  // Width is an independently configurable research input; only omission/auto
  // follows the selected baseline's per-subchannel DQ width.
  if (automatic("data_bus_bits")) derive("data_bus_bits", static_cast<double>(width),
      "channels * pseudo_channels * baseline DQ bits per subchannel");
  else result.overrides.emplace_back("data_bus_bits", std::to_string(
      positive_int("data_bus_bits", baseline.data_bus_bits)));
  return result;
}

static void apply_coupled_model(DramSpec& spec, const ModelOverrides& overrides,
                                std::vector<ParameterDerivation>* derived) {
    if (spec.density_reference_channels != 0)
      throw std::invalid_argument("configure the full model before creating channel-local views");
    const auto resolved = resolve_coupled_inputs(spec, overrides);
    apply_resolved_overrides(spec, resolved.overrides);
    if (derived) *derived = resolved.derived;
    std::set<std::string> explicit_timings;
    for (const auto& [key, value] : overrides) {
      const auto name = canonical_timing_name_for_key(key);
      if (!name.empty()) explicit_timings.insert(name);
    }
    // Recompute only documented dependencies, not every timing with a plausible
    // algebraic relationship. Explicit secondary constraints remain independent.
    auto derive_timing = [&](const char* name, int& field, int value,
                             std::initializer_list<const char*> dependencies,
                             const char* formula) {
      if (explicit_timings.contains(name)) return;
      if (std::none_of(dependencies.begin(), dependencies.end(),
          [&](const char* dep) { return explicit_timings.contains(dep); })) return;
      field = value;
      TimingValueSource source = TimingValueSource::Derived;
      for (const auto& entry : spec.timing_table.entries) {
        if (std::none_of(dependencies.begin(), dependencies.end(),
                        [&](const char* dep) { return entry.name == dep; })) continue;
        if (entry.source == TimingValueSource::ResearchDefault || entry.vendor_required_for_numeric)
          source = TimingValueSource::ResearchDefault;
        else if (entry.source == TimingValueSource::ExternalReference && source != TimingValueSource::ResearchDefault)
          source = TimingValueSource::ExternalReference;
      }
      set_timing_source(spec, name, source, std::string("Calculated: ") + formula +
                        "; inherits unresolved dependency provenance.");
      if (derived) derived->push_back({name, std::to_string(value), formula});
    };
    refresh_timing_table(spec);
    const auto rc = static_cast<std::int64_t>(spec.timing.nRAS) + spec.timing.nRP;
    if (rc > std::numeric_limits<int>::max())
      throw std::invalid_argument("derived nRC overflow");
    derive_timing("nRC", spec.timing.nRC, static_cast<int>(rc), {"nRAS", "nRP"}, "nRAS + nRP");
    // LPDDR6 has an independent JEDEC RFM duration (Tables 366/367).
    if (spec.standard != DramStandard::Lpddr6) {
      derive_timing("nRFMab", spec.timing.nRFMab, spec.timing.nRFC, {"nRFC"}, "project default: nRFMab = nRFC");
      derive_timing("nRFMpb", spec.timing.nRFMpb, spec.timing.nRFCpb, {"nRFCpb"}, "project default: nRFMpb = nRFCpb");
    }
    finalize_spec(spec);
}

void apply_spec_overrides(DramSpec& spec, const ModelOverrides& overrides) {
  // Apply atomically: a rejected combination must not damage the caller's model.
  auto updated = spec;
  apply_coupled_model(updated, overrides, nullptr);
  spec = std::move(updated);
}

DramSpec build_model(const std::string& standard, const ModelOverrides& overrides,
                     int schema_version, std::vector<ParameterDerivation>* derived) {
  if (schema_version < 1 || schema_version > 3)
    throw std::invalid_argument("unsupported model config schema");
  if (derived) derived->clear();
  // Schema denotes accepted syntax only. All model inputs use the latest resolver.
  DramSpec spec = make_spec(standard);
  apply_coupled_model(spec, overrides, derived);
  return spec;
}
}  // namespace hbm_sim::config
