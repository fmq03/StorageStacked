#pragma once
#include "hbm_sim/core/system.hpp"
#include "hif2mc/backend.hpp"
#include <memory>

namespace hif2mc {
class MemSimBackend final : public Backend {
public:
  MemSimBackend(hbm_sim::DramSpec, hbm_sim::MemorySystemOptions = {});
  ~MemSimBackend() override;
  bool try_submit(const BackendRequest &) override;
  void tick() override;
  std::optional<BackendEvent> pop_event() override;
  std::size_t transaction_bytes() const override;
  double tick_period_ps() const override;
  std::uint64_t capacity_bytes() const override;
  bool idle() const override;
  void finish() override;
  const hbm_sim::MemorySystem &memory_system() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace hif2mc
