#include "hif2mc/memsim_backend.hpp"
#include <deque>
#include <limits>
#include <map>
#include <stdexcept>
#include <unordered_map>

namespace hif2mc {
namespace {
Status status_of(hbm_sim::ResponseStatus s) {
  using S = hbm_sim::ResponseStatus;
  switch (s) {
  case S::Ok:
    return Status::Ok;
  case S::EccCorrected:
    return Status::EccCorrected;
  case S::UninitializedData:
    return Status::Uninitialized;
  case S::DataMismatch:
    return Status::DataMismatch;
  case S::Retry:
    return Status::Retry;
  case S::Timeout:
    return Status::Timeout;
  case S::EccUncorrectable:
    return Status::EccUncorrectable;
  case S::InvalidTransaction:
    return Status::Invalid;
  }
  throw std::logic_error("unknown mem_sim status");
}
} // namespace
struct MemSimBackend::Impl {
  hbm_sim::DramSpec spec;
  std::unique_ptr<hbm_sim::MemorySystem> system;
  std::uint64_t capacity;
  struct Child {
    std::uint64_t host;
    std::uint32_t index;
    bool write;
  };
  std::unordered_map<std::uint64_t, Child> children;
  std::map<std::uint64_t, std::vector<std::uint64_t>> hosts;
  std::vector<std::size_t> trace_cursors;
  std::deque<BackendEvent> events;
  Impl(hbm_sim::DramSpec s, hbm_sim::MemorySystemOptions options)
      : spec(std::move(s)) {
    if (options.stack_count <= 0 ||
        spec.addressable_capacity_bytes() >
            std::numeric_limits<std::uint64_t>::max() /
                static_cast<unsigned>(options.stack_count))
      throw std::invalid_argument("invalid backend capacity");
    capacity = spec.addressable_capacity_bytes() *
               static_cast<unsigned>(options.stack_count);
    // CONTRACT: v1 observes public per-controller trace records for MC issue
    // events. This retains history and is NOT a DFI pin-consumption observer.
    options.controller.retain_command_trace = true;
    options.response_delivery_mode = hbm_sim::ResponseDeliveryMode::HostOnly;
    system = std::make_unique<hbm_sim::MemorySystem>(spec, std::move(options));
    trace_cursors.resize(system->controller_count());
  }
};
MemSimBackend::MemSimBackend(hbm_sim::DramSpec s,
                             hbm_sim::MemorySystemOptions o)
    : impl_(std::make_unique<Impl>(std::move(s), std::move(o))) {}
MemSimBackend::~MemSimBackend() = default;
bool MemSimBackend::try_submit(const BackendRequest &r) {
  auto &p = *impl_;
  const auto tx = transaction_bytes();
  if (!r.bytes || r.bytes > tx || r.address >= p.capacity ||
      r.bytes > p.capacity - r.address || r.address % tx + r.bytes > tx)
    throw std::invalid_argument(
        "backend child outside capacity/transaction boundary");
  if (p.children.contains(r.id))
    throw std::logic_error("duplicate backend child ID");
  if (r.write && (r.data.size() != r.bytes || r.mask.size() != r.bytes))
    throw std::invalid_argument("backend write payload/mask length mismatch");
  hbm_sim::Request q;
  q.id = r.id;
  q.host_request_id = r.host_id;
  q.address = r.address;
  q.type = r.write ? hbm_sim::RequestType::Write : hbm_sim::RequestType::Read;
  q.transaction_index = r.index;
  q.transaction_count = r.count;
  q.transfer_bytes = r.bytes;
  q.qos_class = r.qos;
  // Single-stack MemorySystem trusts decoded. Multi-stack MemorySystem
  // re-decodes the routed local address itself.
  if (p.system->stack_count() == 1)
    q.decoded = hbm_sim::AddressMapper(p.spec).decode(r.address);
  if (r.write) {
    q.payload = r.data;
    q.byte_mask = r.mask;
    q.has_payload = true;
    q.has_byte_mask = true;
  }
  if (!p.system->try_submit(std::move(q)))
    return false;
  p.children.emplace(r.id, Impl::Child{r.host_id, r.index, r.write});
  p.hosts[r.host_id].push_back(r.id);
  return true;
}
void MemSimBackend::tick() {
  auto &p = *impl_;
  p.system->step();
  for (std::size_t i = 0; i < p.trace_cursors.size(); ++i) {
    const auto &trace = p.system->controllers()[i].issued_commands();
    for (auto &cursor = p.trace_cursors[i]; cursor < trace.size(); ++cursor) {
      const auto &cmd = trace[cursor];
      using C = hbm_sim::Command;
      if (cmd.command != C::RD && cmd.command != C::RDA &&
          cmd.command != C::WR && cmd.command != C::WRA)
        continue;
      auto it = p.children.find(cmd.request_id);
      if (it == p.children.end())
        continue; // maintenance, not our data request
      p.events.push_back({BackendEvent::Kind::Issued,
                          it->second.host,
                          it->second.index,
                          it->second.write,
                          Status::Ok,
                          {},
                          {}});
    }
  }
  while (p.system->has_response()) {
    auto response = p.system->pop_response();
    auto host = p.hosts.find(response.host_request_id);
    if (host == p.hosts.end())
      throw std::logic_error("unmatched mem_sim host response");
    p.events.push_back({BackendEvent::Kind::Completed, response.host_request_id,
                        0, response.type == hbm_sim::RequestType::Write,
                        status_of(response.status), std::move(response.data),
                        std::move(response.initialized_mask)});
    for (auto id : host->second)
      p.children.erase(id);
    p.hosts.erase(host);
  }
}
std::optional<BackendEvent> MemSimBackend::pop_event() {
  if (impl_->events.empty())
    return {};
  auto result = std::move(impl_->events.front());
  impl_->events.pop_front();
  return result;
}
std::size_t MemSimBackend::transaction_bytes() const {
  return static_cast<std::size_t>(impl_->spec.transaction_bytes());
}
double MemSimBackend::tick_period_ps() const {
  return impl_->spec.timing.tCK_ps / impl_->spec.tick_multiplier;
}
std::uint64_t MemSimBackend::capacity_bytes() const { return impl_->capacity; }
bool MemSimBackend::idle() const {
  return impl_->hosts.empty() && impl_->events.empty() &&
         impl_->system->quiescent();
}
void MemSimBackend::finish() {
  if (!idle())
    throw std::logic_error("finish requires a drained backend");
  impl_->system->finish();
}
const hbm_sim::MemorySystem &MemSimBackend::memory_system() const {
  return *impl_->system;
}
} // namespace hif2mc
