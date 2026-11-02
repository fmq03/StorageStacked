// DFI 轨迹生成器：把已发出的 DRAM 命令转换为控制器/PHY 边界的 beat 视图，
// 用于后处理和后续信号级扩展。
#include "hbm_sim/validation/dfi.hpp"

#include <algorithm>
#include <fstream>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <tuple>
#include <utility>
#include <vector>

#include "hbm_sim/dram/semantics.hpp"

namespace hbm_sim {
namespace {

Cycle timing_delay(const DramSpec& spec, int cycles) {
  if (cycles <= 0) {
    return 0;
  }
  return static_cast<Cycle>(cycles) * static_cast<Cycle>(std::max(1, spec.tick_multiplier));
}

std::size_t ceil_div(std::size_t value, std::size_t divisor) {
  return divisor == 0 ? value : (value + divisor - 1) / divisor;
}

void write_decoded(std::ostream& out, const DecodedAddress& d) {
  out << d.channel << ',' << d.pseudo_channel << ',' << d.sid << ',' << d.rank << ','
      << d.bank_group << ',' << d.bank << ',' << d.row << ',' << d.column;
}

std::uint64_t pack_field(int value, int shift, int bits) {
  const std::uint64_t mask = bits >= 64 ? ~std::uint64_t{0} : ((std::uint64_t{1} << bits) - 1);
  return (static_cast<std::uint64_t>(std::max(0, value)) & mask) << shift;
}

std::string synthetic_data_hex(const IssuedCommand& issued, int beat, std::size_t bytes) {
  std::uint64_t state = encode_dfi_address(issued) ^
                        (issued.request_id * 0x9e3779b97f4a7c15ull) ^
                        (static_cast<std::uint64_t>(beat) << 32) ^
                        static_cast<std::uint64_t>(issued.command);
  std::ostringstream os;
  os << std::hex << std::setfill('0');
  for (std::size_t i = 0; i < bytes; i++) {
    state ^= state >> 12;
    state ^= state << 25;
    state ^= state >> 27;
    std::uint8_t byte = static_cast<std::uint8_t>((state * 0x2545f4914f6cdd1dull) >> 56);
    os << std::setw(2) << static_cast<unsigned>(byte);
  }
  return os.str();
}

std::string zero_mask_hex(std::size_t bytes) {
  return std::string(bytes * 2, '0');
}

std::string bytes_hex_slice(const std::vector<std::uint8_t>& bytes,
                            std::size_t offset,
                            std::size_t count,
                            std::uint8_t pad = 0) {
  std::ostringstream os;
  os << std::hex << std::setfill('0');
  for (std::size_t i = 0; i < count; i++) {
    const std::size_t index = offset + i;
    const std::uint8_t byte = index < bytes.size() ? bytes[index] : pad;
    os << std::setw(2) << static_cast<unsigned>(byte);
  }
  return os.str();
}

std::string dfi_write_mask_hex(const IssuedCommand& issued, std::size_t offset, std::size_t count) {
  if (!issued.has_byte_mask || issued.byte_mask.empty()) {
    return zero_mask_hex(count);
  }
  std::ostringstream os;
  os << std::hex << std::setfill('0');
  for (std::size_t i = 0; i < count; i++) {
    const std::size_t index = offset + i;
    // 项目写掩码是字节使能。DFI 的 wrdata_mask/DBI 解释取决于目标模式和
    // 信号极性；在目标 PHY 配置给出映射前，导出项目定义的高电平屏蔽视图。
    const bool byte_enabled = index < issued.byte_mask.size() && issued.byte_mask[index] != 0;
    os << std::setw(2) << static_cast<unsigned>(byte_enabled ? 0x00 : 0xff);
  }
  return os.str();
}

std::string payload_init_mask_hex(const IssuedCommand& issued, std::size_t offset, std::size_t count) {
  if (!issued.initialized_mask.empty()) {
    return bytes_hex_slice(issued.initialized_mask, offset, count, 0);
  }
  if (issued.has_payload) {
    return std::string(count * 2, issued.payload_initialized ? 'f' : '0');
  }
  return {};
}

std::size_t command_payload_bytes(const DramSpec& spec, const IssuedCommand& issued) {
  if (issued.has_payload && !issued.payload.empty()) {
    return issued.payload.size();
  }
  return static_cast<std::size_t>(std::max(1, spec.transaction_bytes()));
}

bool is_read_data_command(Command command) {
  return command == Command::RD || command == Command::RDA;
}

bool is_write_data_command(Command command) {
  return command == Command::WR || command == Command::WRA;
}

std::uint64_t encode_dfi_bank(const DecodedAddress& decoded) {
  return (static_cast<std::uint64_t>(std::max(0, decoded.sid)) << 8) |
         (static_cast<std::uint64_t>(std::max(0, decoded.bank_group)) << 4) |
         static_cast<std::uint64_t>(std::max(0, decoded.bank));
}

bool same_decoded_address(const DecodedAddress& lhs, const DecodedAddress& rhs) {
  return lhs.channel == rhs.channel &&
         lhs.pseudo_channel == rhs.pseudo_channel &&
         lhs.sid == rhs.sid &&
         lhs.rank == rhs.rank &&
         lhs.bank_group == rhs.bank_group &&
         lhs.bank == rhs.bank &&
         lhs.row == rhs.row &&
         lhs.column == rhs.column;
}

constexpr std::size_t kMaxDfiValidationErrors = 64;

// 内部 maintenance ID 在各 controller 独立分配；因此 key 必须同时包含
// stack/channel，不能只用 request_id，否则多通道同拍 refresh 会被错误合并。
using DfiEventKey = std::tuple<int, int, std::uint64_t, Command, Cycle>;
using DfiDataKey = std::tuple<int, int, std::uint64_t, Command, Cycle>;

void add_validation_error(DfiValidationReport& report, const std::string& message) {
  if (report.errors.size() < kMaxDfiValidationErrors) {
    report.errors.push_back(message);
  }
}

std::string event_label(const DfiEvent& event) {
  std::ostringstream os;
  os << "stack " << event.stack_id << " cycle " << event.cycle
     << " request " << event.request_id << " "
     << to_string(event.command) << " " << to_string(event.kind);
  if (event.kind != DfiEventKind::Command) {
    os << " beat " << event.beat;
  }
  return os.str();
}

}  // namespace

const char* to_string(DfiEventKind kind) {
  switch (kind) {
    case DfiEventKind::Command: return "COMMAND";
    case DfiEventKind::ReadData: return "READ_DATA";
    case DfiEventKind::WriteData: return "WRITE_DATA";
  }
  return "UNKNOWN";
}

int dfi_phase_count(const DramSpec& spec) {
  return std::max(1, spec.dfi_phase_count > 0 ? spec.dfi_phase_count : spec.tick_multiplier);
}

std::size_t dfi_payload_beat_bytes(const DramSpec& spec) {
  if (spec.dfi_data_lane_bytes > 0) {
    return static_cast<std::size_t>(spec.dfi_data_lane_bytes);
  }
  const std::size_t payload =
      static_cast<std::size_t>(std::max(1, spec.transaction_bytes()));
  const std::size_t beats = static_cast<std::size_t>(std::max(1, spec.timing.nBL));
  return std::max<std::size_t>(1, ceil_div(payload, beats));
}

std::uint64_t encode_dfi_address(const IssuedCommand& command) {
  const DecodedAddress& d = command.decoded;
  // 紧凑的通道局部 DFI 地址视图。这是稳定轨迹编码，不代表精确 JEDEC CA
  // 位布局；PHY 地址映射确定后可替换为目标引脚编码。
  return pack_field(d.column, 0, 10) |
         pack_field(d.row, 10, 20) |
         pack_field(d.bank, 30, 5) |
         pack_field(d.bank_group, 35, 4) |
         pack_field(d.rank, 39, 2) |
         pack_field(d.sid, 41, 4) |
         pack_field(d.pseudo_channel, 45, 3) |
         pack_field(d.channel, 48, 8) |
         pack_field(static_cast<int>(command.command), 56, 8);
}

std::vector<DfiEvent> build_dfi_trace(const DramSpec& spec,
                                      const std::vector<IssuedCommand>& commands) {
  std::vector<DfiEvent> events;
  const std::size_t beat_bytes = dfi_payload_beat_bytes(spec);
  const std::size_t default_payload_bytes =
      static_cast<std::size_t>(std::max(1, spec.transaction_bytes()));
  const int default_beat_count = static_cast<int>(ceil_div(default_payload_bytes, beat_bytes));
  const int phases = dfi_phase_count(spec);
  const int read_latency = spec.dfi_read_latency_nck > 0 ? spec.dfi_read_latency_nck : spec.timing.nCL;
  const int write_latency = spec.dfi_write_latency_nck > 0 ? spec.dfi_write_latency_nck : spec.timing.nCWL;

  events.reserve(commands.size() * static_cast<std::size_t>(std::max(1, default_beat_count + 1)));
  for (const auto& issued : commands) {
    const bool read_command = is_read_data_command(issued.command);
    const bool write_command = is_write_data_command(issued.command);
    DfiEvent command_event;
    command_event.cycle = issued.cycle;
    command_event.issued_cycle = issued.cycle;
    command_event.request_id = issued.request_id;
    command_event.kind = DfiEventKind::Command;
    command_event.command = issued.command;
    command_event.bus = issued.bus;
    command_event.phase = static_cast<int>(issued.cycle % static_cast<Cycle>(phases));
    command_event.address = issued.address;
    command_event.system_address = issued.system_address;
    command_event.stack_id = issued.stack_id;
    command_event.dfi_reset_n = true;
    command_event.dfi_cs_n = false;
    command_event.dfi_cke = true;
    command_event.dfi_odt = write_command;
    command_event.dfi_rddata_en = read_command;
    command_event.dfi_address = encode_dfi_address(issued);
    command_event.dfi_bank = encode_dfi_bank(issued.decoded);
    command_event.decoded = issued.decoded;
    events.push_back(std::move(command_event));

    if (!command_meta(issued.command).data) {
      continue;
    }

    const bool read = read_command;
    const std::size_t payload_bytes = command_payload_bytes(spec, issued);
    const int beat_count = static_cast<int>(ceil_div(payload_bytes, beat_bytes));
    const Cycle first_data_cycle = issued.data_cycle > 0
                                       ? issued.data_cycle
                                       : issued.cycle + timing_delay(spec, read ? read_latency : write_latency);
    for (int beat = 0; beat < beat_count; beat++) {
      Cycle cycle = first_data_cycle + timing_delay(spec, beat);
      const std::size_t bytes_before = static_cast<std::size_t>(beat) * beat_bytes;
      const std::size_t bytes_left = payload_bytes > bytes_before ? payload_bytes - bytes_before : 0;
      DfiEvent data_event;
      data_event.cycle = cycle;
      data_event.issued_cycle = issued.cycle;
      data_event.request_id = issued.request_id;
      data_event.kind = read ? DfiEventKind::ReadData : DfiEventKind::WriteData;
      data_event.command = issued.command;
      data_event.bus = issued.bus;
      data_event.phase = static_cast<int>(cycle % static_cast<Cycle>(phases));
      data_event.beat = beat;
      data_event.beat_count = beat_count;
      data_event.beat_bytes = std::min(beat_bytes, bytes_left);
      data_event.address = issued.address + static_cast<Address>(bytes_before);
      data_event.system_address = issued.system_address + static_cast<Address>(bytes_before);
      data_event.stack_id = issued.stack_id;
      data_event.dfi_reset_n = true;
      data_event.dfi_cs_n = true;
      data_event.dfi_cke = true;
      data_event.dfi_odt = !read;
      data_event.dfi_rddata_en = false;
      data_event.dfi_wrdata_en = !read;
      data_event.dfi_rddata_valid = read;
      data_event.dfi_address = encode_dfi_address(issued);
      data_event.dfi_bank = encode_dfi_bank(issued.decoded);
      data_event.payload_initialized = issued.has_payload ? issued.payload_initialized : true;
      data_event.payload_init_mask = payload_init_mask_hex(issued, bytes_before, data_event.beat_bytes);
      if (read) {
        if (issued.has_payload) {
          data_event.payload_source = "memory_image";
          data_event.dfi_rddata = bytes_hex_slice(issued.payload, bytes_before, data_event.beat_bytes);
        } else {
          data_event.payload_source = "synthetic_fallback";
          data_event.dfi_rddata = synthetic_data_hex(issued, beat, data_event.beat_bytes);
        }
      } else {
        if (issued.has_payload) {
          data_event.payload_source = "request_payload";
          data_event.dfi_wrdata = bytes_hex_slice(issued.payload, bytes_before, data_event.beat_bytes);
        } else {
          data_event.payload_source = "synthetic_fallback";
          data_event.dfi_wrdata = synthetic_data_hex(issued, beat, data_event.beat_bytes);
        }
        data_event.dfi_wrdata_mask = dfi_write_mask_hex(issued, bytes_before, data_event.beat_bytes);
      }
      data_event.decoded = issued.decoded;
      events.push_back(std::move(data_event));
    }
  }

  std::stable_sort(events.begin(), events.end(), [](const DfiEvent& a, const DfiEvent& b) {
    if (a.cycle != b.cycle) return a.cycle < b.cycle;
    if (a.phase != b.phase) return a.phase < b.phase;
    if (a.stack_id != b.stack_id) return a.stack_id < b.stack_id;
    if (a.request_id != b.request_id) return a.request_id < b.request_id;
    return a.beat < b.beat;
  });
  return events;
}

DfiValidationReport validate_dfi_trace(const DramSpec& spec,
                                       const std::vector<IssuedCommand>& commands,
                                       const std::vector<DfiEvent>& events) {
  DfiValidationReport report;
  report.checked_events = events.size();
  const int phases = dfi_phase_count(spec);
  const std::size_t beat_bytes = dfi_payload_beat_bytes(spec);
  const int read_latency = spec.dfi_read_latency_nck > 0 ? spec.dfi_read_latency_nck
                                                         : spec.timing.nCL;
  const int write_latency = spec.dfi_write_latency_nck > 0 ? spec.dfi_write_latency_nck
                                                           : spec.timing.nCWL;

  std::map<DfiEventKey, std::vector<const DfiEvent*>> command_events;
  std::map<DfiDataKey, std::vector<const DfiEvent*>> data_events;
  std::set<DfiEventKey> issued_command_keys;
  std::set<DfiDataKey> issued_data_keys;
  for (const auto& issued : commands) {
    issued_command_keys.emplace(issued.stack_id, issued.decoded.channel, issued.request_id,
                                issued.command, issued.cycle);
    if (is_read_data_command(issued.command) ||
        is_write_data_command(issued.command)) {
      issued_data_keys.emplace(issued.stack_id, issued.decoded.channel, issued.request_id,
                               issued.command, issued.cycle);
    }
  }
  for (const auto& event : events) {
    if (event.kind == DfiEventKind::Command) {
      command_events[{event.stack_id, event.decoded.channel, event.request_id,
                      event.command, event.cycle}].push_back(&event);
      if (event.issued_cycle != event.cycle) {
        add_validation_error(report, event_label(event) +
                                         " has an inconsistent issued cycle");
      }
    } else {
      data_events[{event.stack_id, event.decoded.channel, event.request_id,
                   event.command, event.issued_cycle}].push_back(&event);
    }

    report.phase_checks++;
    const int expected_phase = static_cast<int>(event.cycle % static_cast<Cycle>(phases));
    if (event.phase < 0 || event.phase >= phases || event.phase != expected_phase) {
      add_validation_error(report, event_label(event) + " has an invalid DFI phase");
    }

    report.signal_checks++;
    if (!event.dfi_reset_n || !event.dfi_cke) {
      add_validation_error(report, event_label(event) + " unexpectedly deasserts reset_n/cke");
    }
    if (event.kind == DfiEventKind::Command) {
      const bool read_command = is_read_data_command(event.command);
      const bool write_command = is_write_data_command(event.command);
      if (event.dfi_cs_n) {
        add_validation_error(report, event_label(event) + " must assert chip select");
      }
      if (event.dfi_wrdata_en || event.dfi_rddata_valid) {
        add_validation_error(report, event_label(event) + " drives a data-valid signal");
      }
      if (event.dfi_rddata_en != read_command || event.dfi_odt != write_command) {
        add_validation_error(report, event_label(event) + " has inconsistent command enables");
      }
      continue;
    }

    report.data_beat_checks++;
    const bool read_event = event.kind == DfiEventKind::ReadData;
    if (!event.dfi_cs_n) {
      add_validation_error(report, event_label(event) + " must not assert command chip select");
    }
    if (event.dfi_rddata_en || event.dfi_odt != !read_event) {
      add_validation_error(report, event_label(event) + " has inconsistent data-window enables");
    }
    if (event.beat < 0 || event.beat_count <= 0 || event.beat >= event.beat_count ||
        event.beat_bytes == 0 || event.beat_bytes > beat_bytes) {
      add_validation_error(report, event_label(event) + " has invalid beat metadata");
    }

    report.payload_checks++;
    const std::size_t expected_hex_chars = event.beat_bytes * 2;
    if (event.payload_source != "synthetic_fallback" &&
        event.payload_init_mask.size() != expected_hex_chars) {
      add_validation_error(report, event_label(event) + " has an invalid initialized-mask width");
    }
    if (read_event) {
      if (!event.dfi_rddata_valid || event.dfi_wrdata_en || event.dfi_rddata.size() != expected_hex_chars ||
          !event.dfi_wrdata.empty() || !event.dfi_wrdata_mask.empty()) {
        add_validation_error(report, event_label(event) + " has inconsistent read-data signals");
      }
      if (event.payload_source != "memory_image" &&
          event.payload_source != "synthetic_fallback") {
        add_validation_error(report, event_label(event) + " has an unknown read payload source");
      }
    } else {
      if (!event.dfi_wrdata_en || event.dfi_rddata_valid || event.dfi_wrdata.size() != expected_hex_chars ||
          event.dfi_wrdata_mask.size() != expected_hex_chars || !event.dfi_rddata.empty()) {
        add_validation_error(report, event_label(event) + " has inconsistent write-data signals");
      }
      if (event.payload_source != "request_payload" &&
          event.payload_source != "synthetic_fallback") {
        add_validation_error(report, event_label(event) + " has an unknown write payload source");
      }
    }
  }

  for (auto& [key, indexed_events] : data_events) {
    (void)key;
    std::stable_sort(indexed_events.begin(), indexed_events.end(),
                     [](const DfiEvent* a, const DfiEvent* b) {
                       return a->beat < b->beat;
                     });
  }
  for (const auto& [key, indexed_events] : command_events) {
    (void)indexed_events;
    if (issued_command_keys.count(key) == 0) {
      add_validation_error(report, "DFI trace contains an orphan command event");
    }
  }
  for (const auto& [key, indexed_events] : data_events) {
    (void)indexed_events;
    if (issued_data_keys.count(key) == 0) {
      add_validation_error(report, "DFI trace contains orphan data events");
    }
  }

  for (const auto& issued : commands) {
    const DfiEventKey command_key{issued.stack_id, issued.decoded.channel, issued.request_id,
                                  issued.command, issued.cycle};
    const auto command_it = command_events.find(command_key);
    report.command_checks++;
    if (command_it == command_events.end() || command_it->second.size() != 1 ||
        command_it->second.front()->kind != DfiEventKind::Command) {
      std::ostringstream os;
      os << "cycle " << issued.cycle << " request " << issued.request_id << " "
         << to_string(issued.command) << " does not have exactly one command event";
      add_validation_error(report, os.str());
    } else {
      const DfiEvent& command_event = *command_it->second.front();
      if (command_event.bus != issued.bus ||
          command_event.issued_cycle != issued.cycle ||
          command_event.address != issued.address ||
          command_event.system_address != issued.system_address ||
          command_event.stack_id != issued.stack_id ||
          command_event.dfi_address != encode_dfi_address(issued) ||
          command_event.dfi_bank != encode_dfi_bank(issued.decoded) ||
          !same_decoded_address(command_event.decoded, issued.decoded)) {
        add_validation_error(report, event_label(command_event) +
                                         " does not match the issued command snapshot");
      }
    }

    const bool read = is_read_data_command(issued.command);
    const bool write = is_write_data_command(issued.command);
    if (!read && !write) {
      continue;
    }

    const std::size_t payload_bytes = command_payload_bytes(spec, issued);
    const int expected_beats = static_cast<int>(ceil_div(payload_bytes, beat_bytes));
    const Cycle first_cycle = issued.data_cycle > 0
                                  ? issued.data_cycle
                                  : issued.cycle + timing_delay(spec, read ? read_latency : write_latency);
    const DfiDataKey data_key{issued.stack_id, issued.decoded.channel, issued.request_id,
                              issued.command, issued.cycle};
    const auto data_it = data_events.find(data_key);
    const std::vector<const DfiEvent*> empty_events;
    const auto& indexed_events =
        data_it == data_events.end() ? empty_events : data_it->second;
    if (static_cast<int>(indexed_events.size()) != expected_beats) {
      std::ostringstream os;
      os << "request " << issued.request_id << " " << to_string(issued.command)
         << " expected " << expected_beats << " data beats, got "
         << indexed_events.size();
      add_validation_error(report, os.str());
      continue;
    }

    for (int beat = 0; beat < expected_beats; beat++) {
      const DfiEvent& event = *indexed_events[static_cast<std::size_t>(beat)];
      report.latency_checks++;
      const Cycle expected_cycle = first_cycle + timing_delay(spec, beat);
      const std::size_t bytes_before = static_cast<std::size_t>(beat) * beat_bytes;
      const std::size_t expected_bytes = std::min(beat_bytes, payload_bytes - bytes_before);
      const std::string expected_payload =
          issued.has_payload
              ? bytes_hex_slice(issued.payload, bytes_before, expected_bytes)
              : synthetic_data_hex(issued, beat, expected_bytes);
      const std::string expected_source =
          issued.has_payload ? (read ? "memory_image" : "request_payload")
                             : "synthetic_fallback";
      const std::string expected_init_mask =
          payload_init_mask_hex(issued, bytes_before, expected_bytes);
      const std::string expected_write_mask =
          write ? dfi_write_mask_hex(issued, bytes_before, expected_bytes) : std::string{};
      if (event.beat != beat || event.beat_count != expected_beats ||
          event.issued_cycle != issued.cycle ||
          event.cycle != expected_cycle || event.beat_bytes != expected_bytes ||
          event.address != issued.address + static_cast<Address>(bytes_before) ||
          event.system_address != issued.system_address + static_cast<Address>(bytes_before) ||
          event.stack_id != issued.stack_id ||
          event.bus != issued.bus ||
          event.dfi_address != encode_dfi_address(issued) ||
          event.dfi_bank != encode_dfi_bank(issued.decoded) ||
          !same_decoded_address(event.decoded, issued.decoded)) {
        add_validation_error(report, event_label(event) + " violates beat ordering/latency/address");
      }
      const std::string& actual_payload = read ? event.dfi_rddata : event.dfi_wrdata;
      if (actual_payload != expected_payload ||
          event.payload_source != expected_source ||
          event.payload_initialized != (issued.has_payload ? issued.payload_initialized : true) ||
          event.payload_init_mask != expected_init_mask ||
          event.dfi_wrdata_mask != expected_write_mask) {
        add_validation_error(report, event_label(event) +
                                         " does not match the issued payload/mask snapshot");
      }
      if (read && issued.has_expected_payload) {
        report.expected_payload_checks++;
        if (issued.expected_payload.size() != payload_bytes ||
            actual_payload != bytes_hex_slice(issued.expected_payload,
                                              bytes_before,
                                              expected_bytes)) {
          add_validation_error(report, event_label(event) +
                                           " does not match the request expected payload");
        }
      }
    }
  }

  return report;
}

void write_dfi_trace_csv(const std::string& path, const std::vector<DfiEvent>& events) {
  std::ofstream out(path);
  if (!out) {
    throw std::runtime_error("failed to open DFI trace output: " + path);
  }
  out << "cycle,phase,kind,request_id,command,bus,beat,beat_count,beat_bytes,stack_id,"
         "channel,pseudo_channel,sid,rank,bank_group,bank,row,column,"
         "address,system_address,issued_cycle,payload_source,payload_initialized,payload_init_mask\n";
  for (const auto& event : events) {
    out << event.cycle << ','
        << event.phase << ','
        << to_string(event.kind) << ','
        << event.request_id << ','
        << to_string(event.command) << ','
        << to_string(event.bus) << ','
        << event.beat << ','
        << event.beat_count << ','
        << event.beat_bytes << ','
        << event.stack_id << ',';
    write_decoded(out, event.decoded);
    out << ','
        << "0x" << std::hex << event.address << std::dec << ','
        << "0x" << std::hex << event.system_address << std::dec << ','
        << event.issued_cycle << ','
        << event.payload_source << ','
        << (event.payload_initialized ? 1 : 0) << ','
        << event.payload_init_mask << '\n';
  }
}

void write_dfi_signal_trace_csv(const std::string& path, const std::vector<DfiEvent>& events) {
  std::ofstream out(path);
  if (!out) {
    throw std::runtime_error("failed to open DFI signal trace output: " + path);
  }
  out << "cycle,phase,request_id,kind,command,stack_id,dfi_cs_n,dfi_address,dfi_bank,"
         "dfi_reset_n,dfi_cke,dfi_odt,dfi_rddata_en,dfi_wrdata_en,dfi_rddata_valid,"
         "dfi_wrdata,dfi_wrdata_mask,dfi_rddata,channel,pseudo_channel,sid,rank,"
         "bank_group,bank,row,column,address,system_address,issued_cycle,payload_source,"
         "payload_initialized,payload_init_mask\n";
  for (const auto& event : events) {
    out << event.cycle << ','
        << event.phase << ','
        << event.request_id << ','
        << to_string(event.kind) << ','
        << to_string(event.command) << ','
        << event.stack_id << ','
        << (event.dfi_cs_n ? 1 : 0) << ','
        << "0x" << std::hex << event.dfi_address << std::dec << ','
        << "0x" << std::hex << event.dfi_bank << std::dec << ','
        << (event.dfi_reset_n ? 1 : 0) << ','
        << (event.dfi_cke ? 1 : 0) << ','
        << (event.dfi_odt ? 1 : 0) << ','
        << (event.dfi_rddata_en ? 1 : 0) << ','
        << (event.dfi_wrdata_en ? 1 : 0) << ','
        << (event.dfi_rddata_valid ? 1 : 0) << ','
        << event.dfi_wrdata << ','
        << event.dfi_wrdata_mask << ','
        << event.dfi_rddata << ',';
    write_decoded(out, event.decoded);
    out << ','
        << "0x" << std::hex << event.address << std::dec << ','
        << "0x" << std::hex << event.system_address << std::dec << ','
        << event.issued_cycle << ','
        << event.payload_source << ','
        << (event.payload_initialized ? 1 : 0) << ','
        << event.payload_init_mask << '\n';
  }
}

}  // namespace hbm_sim
