#pragma once

#include <cstddef>

namespace hbm_sim {

struct StorageKey {
  int stack = 0;
  int channel = 0;
  int pseudo_channel = 0;
  int sid = 0;
  int rank = 0;
  int bank_group = 0;
  int bank = 0;
  int row = 0;
  int column = 0;

  bool operator==(const StorageKey& other) const {
    return stack == other.stack &&
           channel == other.channel &&
           pseudo_channel == other.pseudo_channel &&
           sid == other.sid &&
           rank == other.rank &&
           bank_group == other.bank_group &&
           bank == other.bank &&
           row == other.row &&
           column == other.column;
  }
};

struct StorageKeyHash {
  std::size_t operator()(const StorageKey& key) const;
};

}  // namespace hbm_sim
