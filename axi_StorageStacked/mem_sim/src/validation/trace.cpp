// Command trace recorder：把已发出的 DRAM 命令序列导出为 CSV。
// 该文件只处理序列化，不做任何合法性判断；合法性由 CommandValidator 独立审计。
#include "hbm_sim/validation/trace.hpp"

#include <fstream>
#include <ostream>
#include <stdexcept>

namespace hbm_sim {

void write_command_trace_csv(std::ostream& os, const std::vector<IssuedCommand>& trace) {
  // CSV 字段保持显式 DRAM 坐标，而不是只写原始地址，因为很多维护命令没有真实
  // byte address；scope/timing 调试也更关心 channel/PC/SID/BG/bank/row。
  os << "cycle,request_id,command,bus,stack_id,channel,pseudo_channel,sid,rank,bank_group,bank,row,column,address,system_address\n";
  for (const auto& issued : trace) {
    // 不做 CSV quoting：这里所有字段都是整数或无逗号命令名，保持输出轻量且易 grep。
    os << issued.cycle << ','
       << issued.request_id << ','
       << to_string(issued.command) << ','
       << to_string(issued.bus) << ','
       << issued.stack_id << ','
       << issued.decoded.channel << ','
       << issued.decoded.pseudo_channel << ','
       << issued.decoded.sid << ','
       << issued.decoded.rank << ','
       << issued.decoded.bank_group << ','
       << issued.decoded.bank << ','
       << issued.decoded.row << ','
       << issued.decoded.column << ','
       << "0x" << std::hex << issued.address << ','
       << "0x" << issued.system_address << std::dec << '\n';
  }
}

void write_command_trace_csv(const std::string& path, const std::vector<IssuedCommand>& trace) {
  std::ofstream out(path);
  if (!out) {
    throw std::runtime_error("failed to open command trace for write: " + path);
  }
  // 文件路径版本复用 ostream 版本，确保 CLI 输出和测试中的 stringstream 输出完全一致。
  write_command_trace_csv(out, trace);
}

}  // namespace hbm_sim
