// Frontend 流量层：生成 synthetic workload 或读取 trace，并在进入控制器前完成地址解码。
// Controller 热路径只看到 Request::decoded，不需要每个 cycle 重复调用 AddressMapper。
#include "hbm_sim/frontend/traffic.hpp"

#include <algorithm>
#include <cctype>
#include <deque>
#include <fstream>
#include <limits>
#include <random>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

namespace hbm_sim {
namespace {

std::string lower_token(std::string token);

void validate_address_span(Address address, std::uintmax_t size,
                           const std::string& context) {
  if (size != 0 && size - 1 >
                       std::numeric_limits<Address>::max() - address) {
    throw std::overflow_error(context + " exceeds Address space");
  }
}

RequestType parse_type(const std::string& token) {
  // trace 兼容单字母和完整单词，方便复用不同工具生成的简单访存轨迹。
  // BW/BR 语法也会被流式 trace 解析器识别，并在送入控制器前展开成事务。
  if (token == "R" || token == "r" || token == "READ" || token == "Read" || token == "read" ||
      token == "BR" || token == "br") {
    return RequestType::Read;
  }
  if (token == "W" || token == "w" || token == "WRITE" || token == "Write" || token == "write" ||
      token == "BW" || token == "bw") {
    return RequestType::Write;
  }
  throw std::invalid_argument("unknown trace request type: " + token);
}

bool is_maintenance_type(const std::string& token) {
  const std::string lowered = lower_token(token);
  return lowered == "m" || lowered == "maintenance";
}

Command parse_maintenance_command(const std::string& token) {
  const std::string lowered = lower_token(token);
  if (lowered == "refab") return Command::REFAB;
  if (lowered == "refpb") return Command::REFPB;
  if (lowered == "refdb") return Command::REFDB;
  if (lowered == "rfmab") return Command::RFMAB;
  if (lowered == "rfmpb") return Command::RFMPB;
  if (lowered == "preab") return Command::PREAB;
  if (lowered == "prepb") return Command::PREPB;
  if (lowered == "mrw") return Command::MRW;
  if (lowered == "mrr") return Command::MRR;
  if (lowered == "wcksync") return Command::WCKSYNC;
  if (lowered == "wcktrain") return Command::WCKTRAIN;
  if (lowered == "dvfs") return Command::DVFS;
  if (lowered == "eccscrub") return Command::ECCSCRUB;
  if (lowered == "raserr") return Command::RASERR;
  throw std::invalid_argument("unsupported maintenance trace command: " + token);
}

bool is_burst_type(const std::string& token) {
  return token == "BW" || token == "bw" || token == "BR" || token == "br";
}

std::string lower_token(std::string token) {
  std::transform(token.begin(), token.end(), token.begin(), [](unsigned char c) {
    if (c == '-') return '_';
    return static_cast<char>(std::tolower(c));
  });
  return token;
}

Address parse_address(const std::string& token) {
  if (token.empty() || token.front() == '-') {
    throw std::invalid_argument("invalid address token: " + token);
  }
  std::size_t pos = 0;
  // base=0 允许同时解析十进制、0x 十六进制和 0 前缀八进制。
  // pos 检查用于拒绝 "0x100abcXYZ" 这类尾部带垃圾字符的输入。
  Address value = std::stoull(token, &pos, 0);
  if (pos != token.size()) {
    throw std::invalid_argument("invalid address token: " + token);
  }
  return value;
}

std::uint64_t parse_nonnegative_u64(const std::string& token,
                                    int base,
                                    const std::string& context) {
  if (token.empty() || token.front() == '-') {
    throw std::invalid_argument(context + ": " + token);
  }
  std::size_t pos = 0;
  const std::uint64_t value = std::stoull(token, &pos, base);
  if (pos != token.size()) {
    throw std::invalid_argument(context + ": " + token);
  }
  return value;
}

int parse_nonnegative_int(const std::string& token,
                          const std::string& context) {
  if (token.empty() || token.front() == '-') {
    throw std::invalid_argument(context + ": " + token);
  }
  std::size_t pos = 0;
  const int value = std::stoi(token, &pos, 10);
  if (pos != token.size()) {
    throw std::invalid_argument(context + ": " + token);
  }
  return value;
}

bool looks_like_cycle(const std::string& token) {
  if (token.empty()) {
    return false;
  }
  return std::all_of(token.begin(), token.end(), [](unsigned char c) {
    return std::isdigit(c);
  });
}

std::vector<std::string> split_tokens(const std::string& line) {
  std::istringstream iss(line);
  std::vector<std::string> tokens;
  std::string token;
  while (iss >> token) {
    tokens.push_back(token);
  }
  return tokens;
}

ByteVector trace_data_value(const std::string& value,
                            const Request& req,
                            std::size_t default_size) {
  std::string lowered = lower_token(value);
  if (lowered == "auto" || lowered == "generated" || lowered == "request_id") {
    return make_request_payload(req.address, req.id, default_size);
  }
  if (lowered == "zero" || lowered == "zeros") {
    return ByteVector(default_size, 0);
  }
  if (lowered == "one" || lowered == "ones" || lowered == "ff") {
    return ByteVector(default_size, 0xff);
  }
  return parse_hex_bytes(value);
}

bool is_last_write_value(const std::string& value) {
  std::string lowered = lower_token(value);
  return lowered == "last_write" || lowered == "lastwrite";
}

bool trace_line_needs_last_write_tracking(std::string line) {
  std::size_t comment = line.find('#');
  if (comment != std::string::npos) {
    line.resize(comment);
  }
  std::string lowered = lower_token(std::move(line));
  return lowered.find("last_write") != std::string::npos ||
         lowered.find("lastwrite") != std::string::npos ||
         lowered.find("check=auto") != std::string::npos ||
         lowered.find("verify=auto") != std::string::npos;
}

bool trace_needs_last_write_tracking(const std::string& path) {
  std::ifstream in(path);
  if (!in) {
    throw std::runtime_error("failed to open trace: " + path);
  }
  std::string line;
  while (std::getline(in, line)) {
    if (trace_line_needs_last_write_tracking(std::move(line))) {
      return true;
    }
  }
  return false;
}

struct TraceAddressKey {
  Address address = 0;
  int explicit_stack = -1;

  bool operator==(const TraceAddressKey& other) const {
    return address == other.address && explicit_stack == other.explicit_stack;
  }
};

struct TraceAddressKeyHash {
  std::size_t operator()(const TraceAddressKey& key) const {
    const std::size_t address_hash = std::hash<Address>{}(key.address);
    const std::size_t stack_hash = std::hash<int>{}(key.explicit_stack);
    return address_hash ^ (stack_hash + 0x9e3779b9u +
                           (address_hash << 6) + (address_hash >> 2));
  }
};

using LastWriteMap =
    std::unordered_map<TraceAddressKey, ByteVector, TraceAddressKeyHash>;

TraceAddressKey trace_address_key(const Request& req) {
  return {req.address, req.has_explicit_stack ? req.target_stack : -1};
}

ByteVector lookup_last_write_payload(const LastWriteMap& last_writes,
                                     const Request& req,
                                     int lineno) {
  auto it = last_writes.find(trace_address_key(req));
  if (it == last_writes.end()) {
    throw std::runtime_error("trace line " + std::to_string(lineno) +
                             " uses last_write before any write to address " +
                             format_address(req.address) +
                             (req.has_explicit_stack
                                  ? " on stack " + std::to_string(req.target_stack)
                                  : ""));
  }
  return it->second;
}

void apply_trace_data_token(Request& req,
                            const std::string& token,
                            int lineno,
                            std::size_t default_size,
                            const LastWriteMap* last_writes = nullptr) {
  std::size_t eq = token.find('=');
  if (eq == std::string::npos) {
    if (req.type == RequestType::Write && !req.has_payload) {
      req.payload = trace_data_value(token, req, default_size);
      req.has_payload = true;
      return;
    }
    if (req.type == RequestType::Read && !req.has_expected_payload) {
      if (is_last_write_value(token)) {
        if (last_writes == nullptr) {
          throw std::runtime_error("trace line " + std::to_string(lineno) +
                                   " cannot resolve last_write in this parser");
        }
        req.expected_payload = lookup_last_write_payload(*last_writes, req, lineno);
      } else {
        req.expected_payload = trace_data_value(token, req, default_size);
      }
      req.has_expected_payload = true;
      return;
    }
    throw std::runtime_error("trace line " + std::to_string(lineno) +
                             " has an unqualified data token: " + token);
  }

  std::string key = lower_token(token.substr(0, eq));
  std::string value = token.substr(eq + 1);
  if (key == "data" || key == "payload") {
    if (req.type != RequestType::Write) {
      throw std::runtime_error("trace line " + std::to_string(lineno) +
                               " uses data= on a non-write request");
    }
    req.payload = trace_data_value(value, req, default_size);
    req.has_payload = true;
  } else if (key == "expect" || key == "expected" || key == "check" || key == "verify") {
    if (req.type != RequestType::Read) {
      throw std::runtime_error("trace line " + std::to_string(lineno) +
                               " uses expect/check on a non-read request");
    }
    if (is_last_write_value(value)) {
      if (last_writes == nullptr) {
        throw std::runtime_error("trace line " + std::to_string(lineno) +
                                 " cannot resolve last_write in this parser");
      }
      req.expected_payload = lookup_last_write_payload(*last_writes, req, lineno);
    } else {
      req.expected_payload = trace_data_value(value, req, default_size);
    }
    req.has_expected_payload = true;
  } else if (key == "mask" || key == "byte_mask") {
    if (req.type != RequestType::Write) {
      throw std::runtime_error("trace line " + std::to_string(lineno) +
                               " uses mask= on a non-write request");
    }
    req.byte_mask = parse_hex_bytes(value);
    req.has_byte_mask = true;
  } else if (key == "qos" || key == "priority") {
    req.qos_class = parse_nonnegative_int(
        value, "trace line " + std::to_string(lineno) +
                   " has invalid qos class");
  } else if (key == "stack" || key == "stack_id") {
    req.target_stack = parse_nonnegative_int(
        value, "trace line " + std::to_string(lineno) +
                   " has invalid stack id");
    req.has_explicit_stack = true;
  } else {
    throw std::runtime_error("trace line " + std::to_string(lineno) +
                             " has unknown data key: " + key);
  }
}

ByteVector burst_payload_value(const std::string& value,
                               Address address,
                               std::size_t line_size) {
  const std::string lowered = lower_token(value);
  if (lowered.empty() || lowered == "auto" || lowered == "generated" ||
      lowered == "request_id") {
    return make_request_payload(address, 0, line_size);
  }
  if (lowered == "zero" || lowered == "zeros") {
    return ByteVector(line_size, 0x00);
  }
  if (lowered == "one" || lowered == "ones" || lowered == "ff") {
    return ByteVector(line_size, 0xff);
  }
  ByteVector parsed = parse_hex_bytes(value);
  if (parsed.size() != line_size) {
    throw std::runtime_error("burst hex payload length must equal line_size");
  }
  return parsed;
}

std::size_t host_request_bytes(const DramSpec& spec, const Request& req) {
  if (req.type == RequestType::Maintenance) {
    return 0;
  }
  std::size_t bytes = req.transfer_bytes;
  if (req.has_payload) {
    bytes = std::max(bytes, req.payload.size());
  }
  if (req.has_expected_payload) {
    bytes = std::max(bytes, req.expected_payload.size());
  }
  if (req.has_byte_mask) {
    bytes = std::max(bytes, req.byte_mask.size());
  }
  return bytes > 0 ? bytes
                   : static_cast<std::size_t>(std::max(1, spec.bytes_per_request()));
}

std::uint64_t checked_multiply_u64(std::uint64_t lhs, std::uint64_t rhs,
                                   const char* context) {
  if (lhs != 0 && rhs > std::numeric_limits<std::uint64_t>::max() / lhs) {
    throw std::overflow_error(std::string(context) + " exceeds Address space");
  }
  return lhs * rhs;
}

std::uint64_t random_start_count(const DramSpec& spec,
                                 const TrafficOptions& options) {
  const std::uint64_t stacks =
      static_cast<std::uint64_t>(std::max(1, options.stack_count));
  const std::uint64_t aggregate_capacity = checked_multiply_u64(
      spec.addressable_capacity_bytes(), stacks,
      "aggregate random traffic capacity");
  const std::uint64_t address_space =
      options.random_address_space_bytes == 0
          ? aggregate_capacity
          : options.random_address_space_bytes;
  const std::uint64_t alignment =
      static_cast<std::uint64_t>(std::max(1, spec.org.line_size));
  const std::uint64_t host_bytes =
      static_cast<std::uint64_t>(std::max(1, spec.bytes_per_request()));
  if (address_space > aggregate_capacity) {
    throw std::invalid_argument(
        "random_address_space_bytes exceeds aggregate model capacity");
  }
  if (address_space < host_bytes) {
    throw std::invalid_argument(
        "random_address_space_bytes is smaller than one host request");
  }
  if (address_space % alignment != 0) {
    throw std::invalid_argument(
        "random_address_space_bytes must be a multiple of line_size");
  }
  return 1 + (address_space - host_bytes) / alignment;
}

ByteVector slice_bytes(const ByteVector& bytes, std::size_t offset, std::size_t size) {
  if (offset >= bytes.size()) {
    return {};
  }
  const std::size_t end = std::min(bytes.size(), offset + size);
  return ByteVector(bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                    bytes.begin() + static_cast<std::ptrdiff_t>(end));
}

std::vector<Request> split_host_request(const DramSpec& spec, const Request& host) {
  if (host.type == RequestType::Maintenance) {
    Request control = host;
    control.host_request_id = host.id;
    control.transaction_index = 0;
    control.transaction_count = 1;
    control.transfer_bytes = 0;
    return {std::move(control)};
  }

  const std::size_t host_bytes = host_request_bytes(spec, host);
  const std::size_t transaction_bytes =
      static_cast<std::size_t>(std::max(1, spec.transaction_bytes()));
  const std::size_t count =
      1 + (host_bytes - 1) / transaction_bytes;
  if (count > std::numeric_limits<std::uint32_t>::max()) {
    throw std::overflow_error(
        "host request needs more than uint32 transaction_count entries");
  }
  validate_address_span(host.address, static_cast<std::uintmax_t>(host_bytes),
                        "host request address range");
  AddressMapper mapper(spec);
  const ByteVector normalized_host_mask =
      host.has_byte_mask ? normalize_mask(host.byte_mask, host_bytes)
                         : ByteVector{};
  std::vector<Request> transactions;
  transactions.reserve(count);
  for (std::size_t index = 0; index < count; index++) {
    const std::size_t offset = index * transaction_bytes;
    const std::size_t bytes = std::min(transaction_bytes, host_bytes - offset);
    Request req = host;
    req.host_request_id = host.id;
    req.transaction_index = static_cast<std::uint32_t>(index);
    req.transaction_count = static_cast<std::uint32_t>(count);
    req.transfer_bytes = bytes;
    req.address = host.address + static_cast<Address>(offset);
    req.decoded = mapper.decode(req.address);
    req.storage_decoded = {};
    req.has_storage_decoded = false;
    req.payload = slice_bytes(host.payload, offset, bytes);
    req.expected_payload = slice_bytes(host.expected_payload, offset, bytes);
    req.byte_mask = slice_bytes(normalized_host_mask, offset, bytes);
    req.has_payload = host.has_payload && !req.payload.empty();
    req.has_expected_payload =
        host.has_expected_payload && !req.expected_payload.empty();
    req.has_byte_mask = host.has_byte_mask;
    req.burst_offset = host.burst_offset + static_cast<std::uint64_t>(offset);
    transactions.push_back(std::move(req));
  }
  return transactions;
}

void validate_options(const TrafficOptions& options) {
  // 尽早拒绝明显无效的输入，避免把 typo 静默当成 stream 运行。
  if (options.read_ratio < 0 || options.read_ratio > 100) {
    throw std::invalid_argument("read_ratio must be in [0, 100]");
  }
  if (!options.trace_path.empty()) {
    // trace 模式允许 pattern 任意，因为 pattern 不会被使用；这能避免 CLI
    // 调用者在指定 trace 时还必须清理默认 pattern。
    return;
  }
  if (options.pattern != "stream" && options.pattern != "random") {
    throw std::invalid_argument("unsupported pattern: " + options.pattern);
  }
  if (options.addr_stride == 0 && options.pattern == "stream") {
    throw std::invalid_argument("addr_stride must be > 0 for stream traffic");
  }
}

class SyntheticTrafficStream final : public TrafficStream {
 public:
  SyntheticTrafficStream(DramSpec spec, TrafficOptions options)
      : spec_(std::move(spec)),
        options_(std::move(options)),
        mapper_(spec_),
        rng_(options_.seed),
        ratio_dist_(1, 100),
        line_space_(random_start_count(spec_, options_)),
        addr_dist_(0, line_space_ - 1) {}

  bool next(Request& req) override {
    if (next_id_ >= options_.requests) return false;
    Address address = options_.pattern == "random"
        ? addr_dist_(rng_) * static_cast<std::uint64_t>(spec_.org.line_size)
        : next_id_ * options_.addr_stride;
    req = Request{};
    req.id = next_id_;
    req.address = address;
    req.type = ratio_dist_(rng_) <= options_.read_ratio
        ? RequestType::Read
        : RequestType::Write;
    req.decoded = mapper_.decode(address);
    req.inject_cycle = static_cast<Cycle>(next_id_ * options_.inject_interval);
    if (req.type == RequestType::Write) {
      req.payload = make_request_payload(
          req.address, req.id, static_cast<std::size_t>(std::max(1, spec_.bytes_per_request())));
      req.has_payload = true;
    }
    next_id_++;
    stats_.emitted_requests++;
    return true;
  }

  std::optional<std::uint64_t> remaining_hint() const override {
    return options_.requests - next_id_;
  }

  const TrafficStreamStats& stream_stats() const override { return stats_; }

 private:
  DramSpec spec_;
  TrafficOptions options_;
  AddressMapper mapper_;
  std::mt19937_64 rng_;
  std::uniform_int_distribution<int> ratio_dist_;
  std::uint64_t line_space_;
  std::uniform_int_distribution<std::uint64_t> addr_dist_;
  std::uint64_t next_id_ = 0;
  TrafficStreamStats stats_;
};

class TraceTrafficStream final : public TrafficStream {
 public:
  TraceTrafficStream(DramSpec spec, TrafficOptions options)
      : spec_(std::move(spec)),
        options_(std::move(options)),
        mapper_(spec_),
        input_(options_.trace_path),
        track_last_writes_(trace_needs_last_write_tracking(options_.trace_path)) {
    if (!input_) {
      throw std::runtime_error("failed to open trace: " + options_.trace_path);
    }
  }

  bool next(Request& req) override {
    if (limit_reached()) return false;
    if (burst_.active) {
      emit_burst(req);
      return true;
    }

    std::string line;
    while (std::getline(input_, line)) {
      lineno_++;
      std::size_t comment = line.find('#');
      if (comment != std::string::npos) line.resize(comment);
      std::vector<std::string> tokens = split_tokens(line);
      if (tokens.empty()) continue;
      parse_trace_line(tokens, req);
      return true;
    }
    done_ = true;
    return false;
  }

  std::optional<std::uint64_t> remaining_hint() const override {
    if (done_ || limit_reached()) return 0;
    if (options_.requests != 0) {
      return options_.requests - stats_.emitted_requests;
    }
    return std::nullopt;
  }

  const TrafficStreamStats& stream_stats() const override { return stats_; }

 private:
  struct BurstState {
    bool active = false;
    RequestType type = RequestType::Read;
    Address address = 0;
    Cycle inject_cycle = 0;
    std::uint64_t line_count = 0;
    std::uint64_t next_line = 0;
    std::uint64_t burst_id = 0;
    std::string pattern;
    std::string check;
    int qos_class = 0;
    int target_stack = -1;
    bool has_explicit_stack = false;
  };

  bool limit_reached() const {
    return options_.requests != 0 && stats_.emitted_requests >= options_.requests;
  }

  void account_request(const Request& req) {
    stats_.emitted_requests++;
    if (req.burst_id == 0) return;
    stats_.burst_split_requests++;
    if (req.type == RequestType::Read) {
      stats_.burst_read_requests++;
      stats_.burst_read_bytes += static_cast<std::uint64_t>(spec_.org.line_size);
    } else {
      stats_.burst_write_requests++;
      stats_.burst_write_bytes += static_cast<std::uint64_t>(spec_.org.line_size);
    }
  }

  void emit_burst(Request& req) {
    const Address address = burst_.address +
        burst_.next_line * static_cast<std::uint64_t>(spec_.org.line_size);
    req = Request{};
    req.id = next_id_++;
    req.type = burst_.type;
    req.address = address;
    req.inject_cycle = burst_.inject_cycle;
    req.decoded = mapper_.decode(address);
    req.burst_id = burst_.burst_id;
    req.burst_offset = burst_.next_line * static_cast<std::uint64_t>(spec_.org.line_size);
    req.qos_class = burst_.qos_class;
    req.target_stack = burst_.target_stack;
    req.has_explicit_stack = burst_.has_explicit_stack;
    if (req.type == RequestType::Write) {
      req.payload = burst_payload_value(
          burst_.pattern, req.address, static_cast<std::size_t>(spec_.org.line_size));
      req.has_payload = true;
      if (track_last_writes_) {
        last_write_payloads_[trace_address_key(req)] = req.payload;
      }
    } else if (burst_.check == "last_write" || burst_.check == "auto" ||
               burst_.check == "zero" || burst_.check == "zeros" ||
               burst_.check == "ff" || burst_.check == "request_id") {
      if (burst_.check == "last_write") {
        req.expected_payload = lookup_last_write_payload(
            last_write_payloads_, req, lineno_);
      } else if (burst_.check == "auto") {
        auto it = track_last_writes_ ? last_write_payloads_.find(trace_address_key(req))
                                     : last_write_payloads_.end();
        req.expected_payload = it == last_write_payloads_.end()
            ? burst_payload_value(
                  "request_id", req.address, static_cast<std::size_t>(spec_.org.line_size))
            : it->second;
      } else {
        req.expected_payload = burst_payload_value(
            burst_.check, req.address, static_cast<std::size_t>(spec_.org.line_size));
      }
      req.has_expected_payload = true;
    }
    burst_.next_line++;
    if (burst_.next_line >= burst_.line_count || limit_reached()) {
      burst_.active = false;
    }
    account_request(req);
  }

  void parse_trace_line(const std::vector<std::string>& tokens, Request& req) {
    if (tokens.size() < 2) {
      throw std::runtime_error("trace line " + std::to_string(lineno_) + " missing address");
    }
    Cycle inject_cycle = next_id_ * options_.inject_interval;
    std::size_t idx = 0;
    if (tokens.size() >= 3 && looks_like_cycle(tokens[0])) {
      inject_cycle = static_cast<Cycle>(parse_nonnegative_u64(
          tokens[0], 10,
          "trace line " + std::to_string(lineno_) +
              " has invalid inject cycle"));
      idx = 1;
    }
    if (tokens.size() < idx + 2) {
      throw std::runtime_error("trace line " + std::to_string(lineno_) + " missing address");
    }

    if (is_maintenance_type(tokens[idx])) {
      if (tokens.size() < idx + 3) {
        throw std::runtime_error("trace line " + std::to_string(lineno_) +
                                 " maintenance syntax is M COMMAND ADDRESS");
      }
      req = Request{};
      req.id = next_id_++;
      req.type = RequestType::Maintenance;
      req.next = parse_maintenance_command(tokens[idx + 1]);
      req.address = parse_address(tokens[idx + 2]);
      req.inject_cycle = inject_cycle;
      req.decoded = mapper_.decode(req.address);
      for (std::size_t i = idx + 3; i < tokens.size(); ++i) {
        const std::size_t eq = tokens[i].find('=');
        if (eq == std::string::npos) {
          throw std::runtime_error("trace line " + std::to_string(lineno_) +
                                   " has invalid maintenance option: " + tokens[i]);
        }
        const std::string key = lower_token(tokens[i].substr(0, eq));
        const std::string value = tokens[i].substr(eq + 1);
        if (key == "stack" || key == "stack_id") {
          req.target_stack = parse_nonnegative_int(
              value, "trace line " + std::to_string(lineno_) +
                         " has invalid stack id");
          req.has_explicit_stack = true;
        } else if (key == "qos" || key == "priority") {
          req.qos_class = parse_nonnegative_int(
              value, "trace line " + std::to_string(lineno_) +
                         " has invalid qos class");
        } else {
          throw std::runtime_error("trace line " + std::to_string(lineno_) +
                                   " has unknown maintenance option: " + key);
        }
      }
      account_request(req);
      return;
    }

    const bool is_burst = is_burst_type(tokens[idx]);
    const Address address = parse_address(tokens[idx + 1]);
    if (!is_burst) {
      req = Request{};
      req.id = next_id_++;
      req.type = parse_type(tokens[idx]);
      req.address = address;
      req.inject_cycle = inject_cycle;
      req.decoded = mapper_.decode(address);
      // stack= 可以写在 expect=last_write 之后；先预读 stack token，避免 token
      // 顺序改变 last-write 命名空间。随后统一解析仍会完成合法性检查。
      for (std::size_t i = idx + 2; i < tokens.size(); i++) {
        const std::size_t eq = tokens[i].find('=');
        if (eq == std::string::npos) continue;
        const std::string key = lower_token(tokens[i].substr(0, eq));
        if (key == "stack" || key == "stack_id") {
          req.target_stack = parse_nonnegative_int(
              tokens[i].substr(eq + 1),
              "trace line " + std::to_string(lineno_) +
                  " has invalid stack id");
          req.has_explicit_stack = true;
        }
      }
      for (std::size_t i = idx + 2; i < tokens.size(); i++) {
        apply_trace_data_token(
            req,
            tokens[i],
            lineno_,
            static_cast<std::size_t>(std::max(1, spec_.bytes_per_request())),
            track_last_writes_ ? &last_write_payloads_ : nullptr);
      }
      if (req.type == RequestType::Write) {
        if (!req.has_payload || req.payload.empty()) {
          req.payload = make_request_payload(
              req.address,
              req.id,
              static_cast<std::size_t>(std::max(1, spec_.bytes_per_request())));
          req.has_payload = true;
        }
        if (req.has_byte_mask && req.byte_mask.size() != req.payload.size()) {
          throw std::runtime_error(
              "trace line " + std::to_string(lineno_) +
              " byte mask length must equal write payload length");
        }
        if (track_last_writes_) {
          last_write_payloads_[trace_address_key(req)] = req.payload;
        }
      }
      account_request(req);
      return;
    }

    std::uint64_t burst_len = 0;
    std::string pattern;
    std::string check;
    int qos_class = 0;
    int target_stack = -1;
    bool has_explicit_stack = false;
    for (std::size_t i = idx + 2; i < tokens.size(); i++) {
      std::size_t eq = tokens[i].find('=');
      if (eq == std::string::npos) continue;
      const std::string key = lower_token(tokens[i].substr(0, eq));
      const std::string value = tokens[i].substr(eq + 1);
      if (key == "len" || key == "length") {
        burst_len = parse_nonnegative_u64(
            value, 0, "trace line " + std::to_string(lineno_) +
                          " has invalid burst length");
      } else if (key == "pattern") {
        pattern = lower_token(value);
      } else if (key == "check" || key == "verify") {
        check = lower_token(value);
      } else if (key == "qos" || key == "priority") {
        qos_class = parse_nonnegative_int(
            value, "trace line " + std::to_string(lineno_) +
                       " has invalid qos class");
      } else if (key == "stack" || key == "stack_id") {
        target_stack = parse_nonnegative_int(
            value, "trace line " + std::to_string(lineno_) +
                       " has invalid stack id");
        has_explicit_stack = true;
      }
    }
    if (burst_len == 0) {
      throw std::runtime_error("trace line " + std::to_string(lineno_) +
                               " burst requires len=N");
    }
    if (burst_len % static_cast<std::uint64_t>(spec_.org.line_size) != 0) {
      throw std::runtime_error("trace line " + std::to_string(lineno_) +
                               " burst length must be multiple of line_size");
    }
    validate_address_span(address, burst_len,
                          "trace line " + std::to_string(lineno_) +
                              " burst address range");
    burst_.active = true;
    burst_.type = parse_type(tokens[idx]);
    burst_.address = address;
    burst_.inject_cycle = inject_cycle;
    burst_.line_count = burst_len / static_cast<std::uint64_t>(spec_.org.line_size);
    burst_.next_line = 0;
    burst_.burst_id = next_burst_id_++;
    burst_.pattern = std::move(pattern);
    burst_.check = std::move(check);
    burst_.qos_class = qos_class;
    burst_.target_stack = target_stack;
    burst_.has_explicit_stack = has_explicit_stack;
    stats_.burst_trace_lines++;
    emit_burst(req);
  }

  DramSpec spec_;
  TrafficOptions options_;
  AddressMapper mapper_;
  std::ifstream input_;
  bool track_last_writes_ = false;
  std::uint64_t next_id_ = 0;
  std::uint64_t next_burst_id_ = 1;
  int lineno_ = 0;
  bool done_ = false;
  BurstState burst_;
  TrafficStreamStats stats_;
  LastWriteMap last_write_payloads_;
};

class ControlPrefixedTrafficStream final : public TrafficStream {
 public:
  ControlPrefixedTrafficStream(std::vector<Request> control,
                               std::unique_ptr<TrafficStream> workload,
                               Cycle interval)
      : control_(std::move(control)),
        workload_(std::move(workload)),
        shift_((static_cast<Cycle>(control_.size()) + 1) * std::max<Cycle>(1, interval)),
        id_shift_(control_.size()) {}

  bool next(Request& req) override {
    if (control_next_ < control_.size()) {
      req = control_[control_next_++];
      stats_.emitted_requests++;
      return true;
    }
    if (!workload_->next(req)) return false;
    req.id += id_shift_;
    req.inject_cycle += shift_;
    stats_ = workload_->stream_stats();
    stats_.emitted_requests += control_.size();
    return true;
  }

  std::optional<std::uint64_t> remaining_hint() const override {
    const std::uint64_t controls_left = control_.size() - control_next_;
    auto workload_left = workload_->remaining_hint();
    if (!workload_left.has_value()) return std::nullopt;
    return controls_left + *workload_left;
  }

  const TrafficStreamStats& stream_stats() const override { return stats_; }

 private:
  std::vector<Request> control_;
  std::unique_ptr<TrafficStream> workload_;
  Cycle shift_;
  std::uint64_t id_shift_;
  std::size_t control_next_ = 0;
  TrafficStreamStats stats_;
};

class TransactionSplitTrafficStream final : public TrafficStream {
 public:
  TransactionSplitTrafficStream(DramSpec spec, std::unique_ptr<TrafficStream> source)
      : spec_(std::move(spec)), source_(std::move(source)) {}

  bool next(Request& req) override {
    if (pending_.empty()) {
      Request host;
      if (!source_->next(host)) {
        refresh_stats();
        return false;
      }
      if (host.type != RequestType::Maintenance) {
        host_requests_++;
      }
      std::vector<Request> transactions = split_host_request(spec_, host);
      for (Request& transaction : transactions) {
        pending_.push_back(std::move(transaction));
      }
    }
    req = std::move(pending_.front());
    pending_.pop_front();
    req.id = next_transaction_id_++;
    emitted_transactions_++;
    if (req.type != RequestType::Maintenance) {
      dram_transactions_++;
    }
    refresh_stats();
    return true;
  }

  std::optional<std::uint64_t> remaining_hint() const override {
    auto hosts = source_->remaining_hint();
    if (!hosts.has_value()) {
      return std::nullopt;
    }
    const std::uint64_t default_count = static_cast<std::uint64_t>(
        (std::max(1, spec_.bytes_per_request()) +
         std::max(1, spec_.transaction_bytes()) - 1) /
        std::max(1, spec_.transaction_bytes()));
    return static_cast<std::uint64_t>(pending_.size()) + *hosts * default_count;
  }

  const TrafficStreamStats& stream_stats() const override { return stats_; }

 private:
  void refresh_stats() {
    stats_ = source_->stream_stats();
    stats_.host_requests = host_requests_;
    stats_.dram_transactions = dram_transactions_;
    stats_.emitted_requests = emitted_transactions_;
  }

  DramSpec spec_;
  std::unique_ptr<TrafficStream> source_;
  std::deque<Request> pending_;
  std::uint64_t next_transaction_id_ = 0;
  std::uint64_t host_requests_ = 0;
  std::uint64_t dram_transactions_ = 0;
  std::uint64_t emitted_transactions_ = 0;
  TrafficStreamStats stats_;
};

std::unique_ptr<TrafficStream> make_base_traffic_stream(const DramSpec& spec,
                                                        const TrafficOptions& options) {
  if (!options.trace_path.empty()) {
    return std::make_unique<TraceTrafficStream>(spec, options);
  }
  return std::make_unique<SyntheticTrafficStream>(spec, options);
}

DecodedAddress control_decoded_for_channel(int channel) {
  // 控制命令没有真实 payload 地址，不能通过 AddressMapper 从地址反推目标。
  // 因此这里直接构造一个 channel-scoped decoded 坐标：channel 决定交给哪个
  // Controller，其余维度保持 0。MemorySystem 对 Maintenance 请求会尊重
  // decoded.channel，不再套用 round_robin/xor channel mapper。
  DecodedAddress d;
  d.channel = channel;
  d.pseudo_channel = 0;
  d.sid = 0;
  d.rank = 0;
  d.bank_group = 0;
  d.bank = 0;
  d.row = 0;
  d.column = 0;
  return d;
}

void append_control(std::vector<Request>& out,
                    std::uint64_t& id,
                    Cycle& inject_cycle,
                    Cycle interval,
                    int stack,
                    int channel,
                    Command command) {
  Request req;
  req.id = id++;
  req.type = RequestType::Maintenance;
  req.next = command;
  req.address = 0;
  req.target_stack = stack;
  req.decoded = control_decoded_for_channel(channel);
  req.inject_cycle = inject_cycle;
  out.push_back(req);
  inject_cycle += std::max<Cycle>(1, interval);
}

}  // namespace

std::vector<Request> generate_traffic(const DramSpec& spec, const TrafficOptions& options) {
  validate_options(options);
  // 批量 API 只负责把权威流式实现排空到 vector。这样 trace 语法、burst
  // 限制、随机序列和 transaction 拆分不会再维护第二套容易分叉的代码。
  std::unique_ptr<TrafficStream> base = make_base_traffic_stream(spec, options);
  TransactionSplitTrafficStream stream(spec, std::move(base));
  std::vector<Request> requests;
  Request request;
  while (stream.next(request)) {
    requests.push_back(std::move(request));
  }
  return requests;
}

std::unique_ptr<TrafficStream> make_traffic_stream(const DramSpec& spec,
                                                   const TrafficOptions& options) {
  validate_options(options);
  std::unique_ptr<TrafficStream> workload = make_base_traffic_stream(spec, options);
  std::vector<Request> control = generate_control_sequence(spec, options);
  if (!control.empty()) {
    workload = std::make_unique<ControlPrefixedTrafficStream>(
        std::move(control), std::move(workload), options.init_sequence_interval);
  }
  return std::make_unique<TransactionSplitTrafficStream>(spec, std::move(workload));
}

std::vector<Request> generate_control_sequence(const DramSpec& spec, const TrafficOptions& options) {
  // 这个函数把“启动 firmware/PHY 会做的控制动作”抽象成普通 Maintenance Request。
  // 好处是它们会经过和读写请求相同的 priority buffer、timing gate、命令状态机、
  // CommandExecutor 和离线 validator，而不是在 Controller 外部直接改状态。
  std::string sequence = lower_token(options.init_sequence);
  if (sequence.empty() || sequence == "none" || sequence == "off") {
    return {};
  }
  if (sequence == "auto") {
    if (spec.lpddr_family) {
      sequence = spec.name == "LPDDR6" ? "lpddr6" : "lpddr";
    } else {
      sequence = spec.name == "HBM4" ? "hbm4" : "hbm";
    }
  }

  const bool hbm_sequence = sequence == "hbm" || sequence == "hbm3" ||
                            sequence == "hbm4";
  const bool lpddr_sequence = sequence == "lpddr" || sequence == "lpddr5" ||
                              sequence == "lpddr6" ||
                              sequence == "lpddr6_full";
  if (!hbm_sequence && !lpddr_sequence) {
    throw std::invalid_argument("unsupported init_sequence: " +
                                options.init_sequence);
  }
  if ((hbm_sequence && spec.lpddr_family) ||
      (lpddr_sequence && !spec.lpddr_family)) {
    throw std::invalid_argument("init_sequence '" + options.init_sequence +
                                "' is incompatible with base standard " +
                                spec.name);
  }

  const int channels = std::max(1, spec.org.channels);
  const int stacks = std::max(1, options.stack_count);
  const Cycle interval = std::max<Cycle>(1, options.init_sequence_interval);
  std::vector<Request> out;
  out.reserve(static_cast<std::size_t>(stacks * channels) * 8);
  std::uint64_t id = 0;
  Cycle inject_cycle = 0;

  for (int stack = 0; stack < stacks; stack++) {
    for (int ch = 0; ch < channels; ch++) {
      if (hbm_sequence) {
        // HBM 侧先覆盖 mode register 访问；如果启用了 ECC 或链路 retry/CRC/RAS
        // metadata，再加入对应的 scrub/error-report 抽象命令。这里按 channel
        // 复制，是为了让 full-stack 多 controller 模式能观察每个 channel
        // 的控制开销。
        append_control(out, id, inject_cycle, interval, stack, ch,
                       Command::MRW);
        append_control(out, id, inject_cycle, interval, stack, ch,
                       Command::MRR);
        if (spec.supports_ecc) {
          append_control(out, id, inject_cycle, interval, stack, ch,
                         Command::ECCSCRUB);
        }
        if (spec.hbm_link_retry_enabled ||
            spec.hbm_link_crc_bits_per_request > 0 ||
            spec.hbm_ras_metadata_bits_per_request > 0) {
          append_control(out, id, inject_cycle, interval, stack, ch,
                         Command::RASERR);
        }
      } else if (lpddr_sequence) {
        // LPDDR 侧显式建模 MR programming、WCK training 和 DVFS。lpddr6_full
        // 额外走 PDE/PDX 与 SREFEN/SREFEX 往返路径，用于测试低功耗状态机和
        // validator 对 power-down/self-refresh 状态的重放能力。
        append_control(out, id, inject_cycle, interval, stack, ch,
                       Command::MRW);
        append_control(out, id, inject_cycle, interval, stack, ch,
                       Command::MRR);
        if (spec.lpddr_dvfs_mode != LpddrDvfsMode::Disabled) {
          append_control(out, id, inject_cycle, interval, stack, ch,
                         Command::DVFS);
        }
        if (spec.lpddr_wck_training_required) {
          // DVFS 可能改变 WCK 相位/频率关系；因此训练放在 DVFS 之后。若 DVFS
          // disabled，这条命令仍表达 startup training。
          append_control(out, id, inject_cycle, interval, stack, ch,
                         Command::WCKTRAIN);
        }
        if (spec.lpddr_link_ecc_enabled) {
          append_control(out, id, inject_cycle, interval, stack, ch,
                         Command::ECCSCRUB);
        }
        if (spec.lpddr_link_protection) {
          append_control(out, id, inject_cycle, interval, stack, ch,
                         Command::RASERR);
        }
        if (sequence == "lpddr6_full") {
          append_control(out, id, inject_cycle, interval, stack, ch,
                         Command::PDE);
          append_control(out, id, inject_cycle, interval, stack, ch,
                         Command::PDX);
          append_control(out, id, inject_cycle, interval, stack, ch,
                         Command::SREFEN);
          append_control(out, id, inject_cycle, interval, stack, ch,
                         Command::SREFEX);
        }
      } else {
        throw std::invalid_argument("unsupported init_sequence: " +
                                    options.init_sequence);
      }
    }
  }

  return out;
}

void prepend_control_sequence(const DramSpec& spec,
                              const TrafficOptions& options,
                              std::vector<Request>& requests) {
  std::vector<Request> control = generate_control_sequence(spec, options);
  if (control.empty()) {
    return;
  }

  // 控制序列要发生在 workload 前。这里不只做 vector prepend，还把原请求的
  // inject_cycle 和 id 整体后移，避免普通读写与 MRW/WCK_TRAIN/DVFS 在 cycle 0
  // 同时进入 buffer 导致实验者误以为初始化不占用前端时间。
  const Cycle interval = std::max<Cycle>(1, options.init_sequence_interval);
  const Cycle shift = (static_cast<Cycle>(control.size()) + 1) * interval;
  const std::uint64_t id_shift = static_cast<std::uint64_t>(control.size());
  for (auto& req : requests) {
    req.id += id_shift;
    req.inject_cycle += shift;
  }
  control.insert(control.end(), requests.begin(), requests.end());
  requests = std::move(control);
}

}  // namespace hbm_sim
