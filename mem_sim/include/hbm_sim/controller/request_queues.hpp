#pragma once

#include <array>
#include <deque>
#include <stdexcept>
#include "hbm_sim/core/request.hpp"

namespace hbm_sim {

enum class RequestQueueKind { Active, Priority, Read, Write };

// Queue storage/lifecycle only. Scheduler chooses candidates; protocol code
// decides prerequisites and completion. No timing or row-policy rules live here.
class RequestQueues {
 public:
  using Queue = std::deque<Request>;
  RequestQueues(std::size_t banks, std::size_t priority, std::size_t reads,
                std::size_t writes)
      : limits_{banks, priority, reads, writes}, active_counts_(banks, 0) {
    for (auto limit : limits_)
      if (limit == 0) throw std::invalid_argument("request queue capacity must be positive");
  }
  Queue& operator[](RequestQueueKind kind) { return queues_.at(slot(kind)); }
  const Queue& operator[](RequestQueueKind kind) const { return queues_.at(slot(kind)); }
  bool has_space(RequestQueueKind kind) const { return (*this)[kind].size() < limits_.at(slot(kind)); }
  std::size_t size() const {
    std::size_t total = 0;
    for (const auto& queue : queues_) total += queue.size();
    return total;
  }
  const std::vector<int>& active_counts() const { return active_counts_; }

  void erase(RequestQueueKind kind, std::size_t index, const DramSpec& spec) {
    auto& queue = (*this)[kind];
    if (index >= queue.size()) return;
    if (kind == RequestQueueKind::Active) {
      auto& count = active_counts_.at(queue[index].decoded.flat_bank(spec));
      if (count <= 0) throw std::logic_error("active queue ownership underflow");
      --count;
    }
    queue.erase(queue.begin() + static_cast<std::ptrdiff_t>(index));
  }
  bool promote(RequestQueueKind kind, std::size_t index, const DramSpec& spec) {
    auto& source = (*this)[kind];
    if (kind == RequestQueueKind::Active || index >= source.size() ||
        !has_space(RequestQueueKind::Active)) return false;
    const auto bank = source[index].decoded.flat_bank(spec);
    auto& count = active_counts_.at(bank);
    (*this)[RequestQueueKind::Active].push_back(source[index]);
    ++count;
    source.erase(source.begin() + static_cast<std::ptrdiff_t>(index));
    return true;
  }

 private:
  static std::size_t slot(RequestQueueKind kind) { return static_cast<std::size_t>(kind); }
  std::array<Queue, 4> queues_;
  std::array<std::size_t, 4> limits_;
  std::vector<int> active_counts_;
};

}  // namespace hbm_sim
