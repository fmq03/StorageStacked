#include "hbm_sim/controller/scheduler.hpp"
#include <iostream>
#include <random>
#include <stdexcept>

using namespace hbm_sim;
namespace {
// Independent pairwise oracle for local Ramulator FRFCFS + the controller's
// final timing check. Ramulator may nominate a not-ready request; that request
// still cannot issue. We compare issuance, not intermediate iterator values.
// Reference: b30320bc, scheduler/impl/frfcfs.cpp SHA256
// d822738aa625c45e6026b44401e185637352e710b0d54dbdf575f98a2541959d.
std::optional<SchedulerCandidateView> reference_issue(
    const std::vector<SchedulerCandidateView>& candidates) {
  std::optional<SchedulerCandidateView> incumbent;
  for (const auto& request : candidates) {
    if (!request.eligible) continue;
    if (!incumbent ||
        (request.timing_ready && !incumbent->timing_ready) ||
        (request.timing_ready == incumbent->timing_ready && request.arrival < incumbent->arrival))
      incumbent = request;
  }
  if (incumbent && !incumbent->timing_ready) return std::nullopt;
  return incumbent;
}
void check(const std::vector<SchedulerCandidateView>& candidates) {
  auto actual = select_scheduled_request(SchedulerKind::FRFCFS, candidates);
  auto expected = reference_issue(candidates);
  if (actual.has_value() != expected.has_value() ||
      (actual && actual->request_index != expected->request_index))
    throw std::runtime_error("FRFCFS issue decision differs from reference contract");
}
}

int main() {
  try {
    check({});
    check({{0, 0, Command::RD, true, false}, {1, 1, Command::ACT, true, true}});
    // Generic FRFCFS is not FRFCFS-RowHit: a younger RD must not bypass an
    // equally-ready older ACT. Ties retain buffer order, not command kind/id.
    check({{7, 1, Command::ACT, true, true}, {2, 1, Command::RD, true, true}});
    std::mt19937 generator(20260909);
    for (int sample = 0; sample < 5000; ++sample) {
      std::vector<SchedulerCandidateView> candidates;
      for (unsigned i = 0, n = generator() % 33; i < n; ++i)
        candidates.push_back({i, generator() % 8,
                              i % 2 ? Command::RD : Command::ACT,
                              (generator() & 1) != 0, (generator() & 1) != 0});
      check(candidates);
    }
    if (select_scheduled_request(SchedulerKind::FCFS,
        {{0, 0, Command::ACT, true, false}, {1, 1, Command::RD, true, true}}))
      throw std::runtime_error("FCFS bypassed blocked oldest request");
    bool rejected = false;
    try { select_scheduled_request(static_cast<SchedulerKind>(99), {}); }
    catch (const std::invalid_argument&) { rejected = true; }
    if (!rejected) throw std::runtime_error("unknown scheduler was silently accepted");
    std::cout << "scheduler reference contract passed (5000 deterministic candidate sets)\n";
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
