#pragma once

#include "hbm_sim/dram/spec.hpp"

// Small controller/PHY fixtures intentionally resize address geometry while
// retaining fixed command timings. Update only their density bookkeeping;
// do not reapply a profile or hide timing/clock errors in validate_spec().
inline void set_fixture_density_from_geometry(hbm_sim::DramSpec& spec) {
  spec.density_gb = hbm_sim::density_gbit_from_geometry(
      static_cast<double>(spec.addressable_capacity_bytes()), spec.lpddr_family,
      spec.stack_height, spec.org.channels, spec.org.pseudo_channels, spec.org.ranks);
}
