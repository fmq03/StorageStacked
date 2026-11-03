#pragma once
// Example/test master. This is a HIF traffic driver, NOT Bridge RTL.
#include "hif2mc/adapter.hpp"
#include <algorithm>
#include <deque>
#include <map>
#include <stdexcept>
#include <vector>

namespace hif2mc::example {
struct Sample {
  std::uint64_t cycle;
  Inputs in;
  Outputs out;
};
class Master {
public:
  Adapter &adapter;
  Config config;
  std::deque<Inputs> commands;
  struct Write {
    Bytes data, mask;
    std::size_t beat = 0;
    std::uint64_t eligible = 0;
    std::uint32_t pointer = 0;
  };
  std::map<std::uint32_t, Write> writes;
  std::deque<Write> sending;
  std::map<std::uint64_t, Bytes> reads;
  std::map<std::uint64_t, unsigned> read_ends;
  std::map<std::uint32_t, std::uint64_t> accepted_at;
  std::vector<Sample> samples;
  std::array<std::size_t, 3> credits;
  explicit Master(Adapter &a, Config c)
      : adapter(a), config(c),
        credits{c.lpr_credits, c.hpr_credits, c.write_credits} {}
  void write(std::uint64_t byte_addr, std::uint32_t ptr, Bytes data, Bytes mask,
             bool rmw = false) {
    if (data.size() != config.command_bytes || mask.size() != data.size())
      throw std::invalid_argument("master data/mask size");
    Inputs i;
    i.hif_cmd_valid = true;
    i.hif_cmd_type = rmw ? 2 : 0;
    i.hif_cmd_addr = byte_addr / config.address_unit_bytes;
    i.hif_cmd_wdata_ptr = ptr;
    commands.push_back(i);
    writes[ptr] = {std::move(data), std::move(mask), 0, 0, ptr};
  }
  void read(std::uint64_t byte_addr, std::uint64_t token, bool high = false) {
    Inputs i;
    i.hif_cmd_valid = true;
    i.hif_cmd_type = 1;
    i.hif_cmd_addr = byte_addr / config.address_unit_bytes;
    i.hif_cmd_token = token;
    i.hif_cmd_pri = high ? 1 : 0;
    commands.push_back(i);
  }
  void step() {
    Inputs in;
    const auto peek = adapter.evaluate(in);
    auto effective = credits;
    if (config.reuse_credit_same_edge) {
      effective[0] += peek.hif_lpr_credit;
      effective[1] += peek.hif_hpr_credit;
      effective[2] += peek.hif_wr_credit;
    }
    if (!commands.empty()) {
      const auto &c = commands.front();
      const bool allowed =
          c.hif_cmd_type == 1
              ? effective[c.hif_cmd_pri] > 0
              : effective[2] > 0 && (c.hif_cmd_type != 2 || effective[0] > 0);
      if (allowed)
        in = c;
    }
    if (!sending.empty() && sending.front().eligible <= adapter.cycle()) {
      auto &w = sending.front();
      in.hif_wdata_valid = true;
      std::copy_n(w.data.begin() + static_cast<std::ptrdiff_t>(32 * w.beat), 32,
                  in.hif_wdata.begin());
      for (std::size_t j = 0; j < 32; ++j)
        if (w.mask[32 * w.beat + j])
          in.hif_wdata_mask |= std::uint32_t(1) << j;
      in.hif_wdata_end = w.beat + 1 == config.command_bytes / 32;
    }
    const auto out = adapter.evaluate(in);
    samples.push_back({adapter.cycle(), in, out});
    credits[0] += out.hif_lpr_credit;
    credits[1] += out.hif_hpr_credit;
    credits[2] += out.hif_wr_credit;
    if (in.hif_cmd_valid && !out.hif_cmd_stall) {
      if (in.hif_cmd_type == 1)
        --credits[in.hif_cmd_pri];
      else {
        --credits[2];
        if (in.hif_cmd_type == 2)
          --credits[0];
        accepted_at[in.hif_cmd_wdata_ptr] = adapter.cycle();
      }
      commands.pop_front();
    }
    if (in.hif_wdata_valid && !out.hif_wdata_stall) {
      ++sending.front().beat;
      if (in.hif_wdata_end)
        sending.pop_front();
    }
    if (out.hif_wdata_ptr_valid) {
      auto it = writes.find(out.hif_wdata_ptr);
      if (it == writes.end())
        throw std::logic_error("unknown pointer from model");
      auto w = it->second;
      w.eligible = adapter.cycle() + 1;
      sending.push_back(std::move(w));
    }
    if (out.hif_rdata_valid) {
      auto &data = reads[out.hif_rdata_token];
      data.insert(data.end(), out.hif_rdata.begin(), out.hif_rdata.end());
      if (out.hif_rdata_end)
        ++read_ends[out.hif_rdata_token];
    }
    adapter.tick(in);
  }
  void run(std::size_t limit = 20000) {
    for (std::size_t n = 0; n < limit; ++n) {
      if (commands.empty() && sending.empty() && adapter.idle())
        return;
      step();
    }
    throw std::runtime_error("HIF master timed out before quiescence");
  }
};
inline Bytes pattern(std::size_t n, unsigned seed) {
  Bytes b(n);
  for (std::size_t j = 0; j < n; ++j)
    b[j] = static_cast<std::uint8_t>(seed + 3 * j);
  return b;
}
} // namespace hif2mc::example
