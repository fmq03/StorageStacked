#pragma once

// Controller -> MemorySystem -> Frontend 的异步完成接口。
// 这些类型只描述内存语义，不绑定 CPU、NoC、UCIe、AXI 或 RTL 的具体信号。

#include <cstdint>
#include <vector>

#include "hbm_sim/core/common.hpp"
#include "hbm_sim/core/data.hpp"

namespace hbm_sim {

enum class ResponseStatus {
  Ok,
  EccCorrected,
  UninitializedData,
  DataMismatch,
  Retry,
  Timeout,
  EccUncorrectable,
  InvalidTransaction,
};

// 数值顺序就是聚合严重度；HostResponse 取所有子事务中的最高严重度。
inline ResponseStatus merge_response_status(ResponseStatus lhs,
                                             ResponseStatus rhs) {
  return static_cast<int>(lhs) >= static_cast<int>(rhs) ? lhs : rhs;
}

inline const char* response_status_name(ResponseStatus status) {
  switch (status) {
    case ResponseStatus::Ok:
      return "ok";
    case ResponseStatus::EccCorrected:
      return "ecc_corrected";
    case ResponseStatus::UninitializedData:
      return "uninitialized_data";
    case ResponseStatus::DataMismatch:
      return "data_mismatch";
    case ResponseStatus::Retry:
      return "retry";
    case ResponseStatus::Timeout:
      return "timeout";
    case ResponseStatus::EccUncorrectable:
      return "ecc_uncorrectable";
    case ResponseStatus::InvalidTransaction:
      return "invalid_transaction";
  }
  return "invalid_transaction";
}

// 一个 Controller 完成的 DRAM 子事务。读响应的 data/initialized_mask 有效；
// 写响应只表示当前模型定义的写完成点，不回传 payload。
struct TransactionResponse {
  std::uint64_t request_id = 0;
  std::uint64_t host_request_id = 0;
  std::uint32_t transaction_index = 0;
  std::uint32_t transaction_count = 1;
  RequestType type = RequestType::Read;
  Address system_address = 0;
  Address local_address = 0;
  int stack = -1;
  int channel = -1;
  Cycle arrival_cycle = 0;
  Cycle issued_cycle = 0;
  Cycle completion_cycle = 0;
  ByteVector data;
  ByteVector initialized_mask;
  bool initialized = true;
  bool ecc_corrected = false;
  bool ecc_uncorrectable = false;
  bool forwarded = false;
  bool coalesced = false;
  ResponseStatus status = ResponseStatus::Ok;
};

// Frontend 原始请求的聚合响应。transactions 始终按 transaction_index 排序；
// 对读请求，data 和 initialized_mask 是各子事务按该顺序拼接后的结果。
struct HostResponse {
  std::uint64_t host_request_id = 0;
  RequestType type = RequestType::Read;
  Address system_address = 0;
  std::uint32_t transaction_count = 1;
  int stack = -1;
  int channel = -1;
  Cycle arrival_cycle = 0;
  Cycle first_issued_cycle = 0;
  Cycle completion_cycle = 0;
  ByteVector data;
  ByteVector initialized_mask;
  std::vector<TransactionResponse> transactions;
  bool initialized = true;
  bool ecc_corrected = false;
  bool ecc_uncorrectable = false;
  bool forwarded = false;
  bool coalesced = false;
  ResponseStatus status = ResponseStatus::Ok;
};

}  // namespace hbm_sim
