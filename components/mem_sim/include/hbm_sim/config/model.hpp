#pragma once

#include <string>
#include <utility>
#include <vector>
#include "hbm_sim/dram/spec.hpp"

namespace hbm_sim::config {
using ModelOverrides = std::vector<std::pair<std::string, std::string>>;
struct ParameterDerivation {
  std::string key;
  std::string value;
  std::string formula;
};
struct ResolvedModelInputs {
  ModelOverrides overrides;
  std::vector<ParameterDerivation> derived;
};

// The shared resolver derives redundant inputs, but never synthesizes a vendor RL/WL table.
// The caller supplies a protocol baseline; explicit dependent values are checked
// against their determinants. Omission and 'auto' request derivation, not zero.
// HBM SID follows supported heights when height is supplied or SID is 'auto';
// explicit SID remains independent. Unrelated partial updates preserve baseline SID.
ResolvedModelInputs resolve_coupled_inputs(const DramSpec& baseline,
                                         const ModelOverrides& inputs);

// Input keys are canonical (lowercase, underscores); vector order preserves
// layered overrides. timing_source.<key> binds provenance to a timing input;
// a plain timing_source is an order-independent default for library inputs.
bool is_spec_override_key(const std::string& key);
bool is_timing_override_key(const std::string& key);
void apply_spec_overrides(DramSpec& spec, const ModelOverrides& overrides);
DramSpec build_model(const std::string& standard,
                     const ModelOverrides& overrides = {},
                     int schema_version = 3,
                     std::vector<ParameterDerivation>* derived = nullptr);
}  // namespace hbm_sim::config
