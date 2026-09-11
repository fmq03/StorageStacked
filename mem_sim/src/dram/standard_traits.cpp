// 标准 traits 目录：只保存协议身份、能力和默认 profile 选择。
// 组织结构和 timing 数值由 profiles.cpp 唯一负责。
#include "hbm_sim/dram/spec.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <stdexcept>

namespace hbm_sim {
namespace {

constexpr std::array<StandardTraits, 4> kStandardTraits{{
    {
        .standard = DramStandard::Hbm4,
        .family = DramFamily::Hbm,
        .canonical_key = "hbm4",
        .display_name = "HBM4",
        .aliases = {"hbm4", "hbm", ""},
        .alias_count = 2,
        .default_timing_profile = "hbm4_jedec_8000_32gb_8hi",
        .default_mode_profile = "full_stack_crc_off",
        .default_speed_bin_mbps = 8000,
        .default_density_gb = 32,
        .default_stack_height = 8,
        .supports_rfm = true,
        .supports_ecc = true,
        .hbm_edge_pairing_matrix = "hbm4_jedec270_4a_row_col_pre_matrix",
        .hbm_sid_mapping = "sid_pair_8hi",
        .hbm_ecc_scheme = "metadata_16b_per_32B_transaction",
        .hbm_ras_policy = "counter_only",
    },
    {
        .standard = DramStandard::Hbm3,
        .family = DramFamily::Hbm,
        .canonical_key = "hbm3",
        .display_name = "HBM3",
        .aliases = {"hbm3", "", ""},
        .alias_count = 1,
        .default_timing_profile = "hbm3_ramulator2_6400_16gb_8hi",
        .default_speed_bin_mbps = 6400,
        .default_density_gb = 16,
        .default_stack_height = 8,
        .hbm_edge_pairing_matrix = "hbm3_row_col_pre_pairing",
        .hbm_sid_mapping = "sid_pair_8hi",
        .hbm_ras_policy = "counter_only",
    },
    {
        .standard = DramStandard::Lpddr6,
        .family = DramFamily::Lpddr,
        .canonical_key = "lpddr6",
        .display_name = "LPDDR6",
        .aliases = {"lpddr6", "lpddr", "ldppr"},
        .alias_count = 3,
        .default_timing_profile = "lpddr6_jedec_10667_16gb",
        .default_mode_profile = "dvfsl_linkprot_off_bl24",
        .default_speed_bin_mbps = 10667,
        .default_density_gb = 16,
        .default_stack_height = 0,
        .supports_rfm = true,
        .lpddr_dual_bank_refresh = true,
        .lpddr_mode_register_profile = "RLSet1_WLSetA_BL24",
        .activation_scope = TimingScope::PseudoChannel,
    },
    {
        .standard = DramStandard::Lpddr5,
        .family = DramFamily::Lpddr,
        .canonical_key = "lpddr5",
        .display_name = "LPDDR5",
        .aliases = {"lpddr5", "", ""},
        .alias_count = 1,
        .default_timing_profile = "lpddr5_ramulator2_6400_16gb",
        .default_speed_bin_mbps = 6400,
        .default_density_gb = 16,
        .lpddr_wck_training_required = false,
        .row_bus_scope = TimingScope::Channel,
        .column_bus_scope = TimingScope::Channel,
        .wck_scope = TimingScope::Channel,
    },
}};

std::string normalized_name(std::string name) {
  std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return name;
}

}  // namespace

const StandardTraits& find_standard_traits(const std::string& name) {
  const std::string key = normalized_name(name);
  for (const auto& traits : kStandardTraits) {
    for (std::size_t i = 0; i < traits.alias_count; ++i) {
      if (traits.aliases[i] == key) {
        return traits;
      }
    }
  }
  throw std::invalid_argument("unsupported standard: " + name);
}

void apply_standard_traits(DramSpec& spec, const StandardTraits& traits) {
  spec = DramSpec{};
  spec.standard = traits.standard;
  spec.family = traits.family;
  spec.name = std::string(traits.display_name);
  spec.timing_profile = std::string(traits.default_timing_profile);
  spec.vendor_profile = "generic";
  spec.mode_profile = std::string(traits.default_mode_profile);
  spec.speed_bin_mbps = traits.default_speed_bin_mbps;
  spec.density_gb = traits.default_density_gb;
  spec.stack_height = traits.default_stack_height;

  spec.lpddr_family = traits.family == DramFamily::Lpddr;
  spec.dual_command_bus = traits.family == DramFamily::Hbm;
  spec.split_activate = traits.family == DramFamily::Lpddr;
  spec.supports_refresh = true;
  spec.supports_rfm = traits.supports_rfm;
  spec.supports_ecc = traits.supports_ecc;
  spec.lpddr_dual_bank_refresh = traits.lpddr_dual_bank_refresh;
  spec.lpddr_wck_training_required = traits.lpddr_wck_training_required;

  spec.hbm_edge_pairing = traits.family == DramFamily::Hbm;
  spec.hbm_strict_edge_pairing = true;
  spec.hbm_edge_pairing_matrix = std::string(traits.hbm_edge_pairing_matrix);
  spec.hbm_sid_mapping = std::string(traits.hbm_sid_mapping);
  spec.hbm_ecc_scheme = std::string(traits.hbm_ecc_scheme);
  spec.hbm_ras_policy = std::string(traits.hbm_ras_policy);
  spec.hbm_link_crc_mode = "off";
  spec.hbm_link_retry_enabled = false;
  spec.hbm_ecc_bits_per_request = traits.standard == DramStandard::Hbm4 ? 16 : 0;

  spec.lpddr_mode_register_profile = std::string(traits.lpddr_mode_register_profile);
  if (traits.standard == DramStandard::Lpddr6) {
    spec.lpddr_low_data_rate_mbps = 4267;
    spec.lpddr_dvfs_mode = LpddrDvfsMode::Nominal;
    spec.lpddr_wck_mode = LpddrWckMode::CasSync;
    spec.lpddr_wck_ratio = 2;
    spec.lpddr_wck_training_mode = "startup_and_dvfs_retrain";
    spec.lpddr_dvfs_transition_policy = "idle_channel_nacu_guarded";
  } else if (traits.standard == DramStandard::Lpddr5) {
    spec.lpddr_wck_training_mode = "cas_sync_only";
    spec.lpddr_dvfs_transition_policy = "disabled";
  }
  spec.lpddr_link_protection_mode = "off";
  spec.lpddr_low_power_state_policy = "controller_idle";

  spec.activation_scope = traits.activation_scope;
  spec.row_bus_scope = traits.row_bus_scope;
  spec.column_bus_scope = traits.column_bus_scope;
  spec.wck_scope = traits.wck_scope;
}

DramSpec make_spec_draft(const std::string& name) {
  DramSpec spec;
  apply_standard_traits(spec, find_standard_traits(name));
  return spec;
}

std::vector<std::string> supported_specs() {
  std::vector<std::string> names;
  names.reserve(kStandardTraits.size());
  for (const auto& traits : kStandardTraits) {
    names.emplace_back(traits.canonical_key);
  }
  return names;
}

}  // namespace hbm_sim
