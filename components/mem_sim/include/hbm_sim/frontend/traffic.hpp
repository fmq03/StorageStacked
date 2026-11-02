#pragma once

// Frontend 流量生成/读取接口。输出 Request 含 decoded 地址，可直接送入 Controller 或 MemorySystem。

#include <string>
#include <memory>
#include <vector>

#include "hbm_sim/core/request.hpp"
#include "hbm_sim/dram/spec.hpp"

namespace hbm_sim {

// 流量生成参数。trace_path 非空时优先读取 trace；否则按 pattern 生成
// stream/random 合成流量。
struct TrafficOptions {
  // 合成流量模式。trace_path 为空时生效；支持 stream 和 random。
  std::string pattern = "stream";
  // 非空时读取 trace 文件，忽略 pattern。
  std::string trace_path;
  // 合成模式下生成请求数；trace 模式下 0 表示读取完整 trace。
  std::uint64_t requests = 10000;
  // 读请求百分比，范围 [0, 100]。写请求比例为 100-read_ratio。
  int read_ratio = 100;
  // random 模式的随机种子，保证实验可复现。
  std::uint64_t seed = 1;
  // random 模式允许使用的聚合系统地址空间，单位 byte。0 表示按标准单 Stack
  // 容量乘 stack_count；非 0 值可与小容量文件后端配合，避免随机地址越界。
  std::uint64_t random_address_space_bytes = 0;
  // stream 模式下相邻请求的 byte stride，默认 64B 即连续 cache line。
  std::uint64_t addr_stride = 64;
  // frontend 注入间隔，单位为 controller tick。0 表示所有请求都在 cycle 0
  // 可注入；1 表示每个 tick 最多产生一条请求。
  Cycle inject_interval = 0;
  // 可选初始化/训练控制序列。默认 none 不改变历史 workload；auto 会按标准选择
  // HBM/LPDDR 控制命令，lpddr6_full 还会覆盖 power-down/self-refresh 往返路径。
  std::string init_sequence = "none";
  // 初始化序列内部命令的注入间隔。保持为 1 可以让控制器按 priority buffer
  // 连续尝试服务；调大可模拟 firmware/PHY 控制命令之间的间隔。
  Cycle init_sequence_interval = 1;
  // 多 stack 初始化会覆盖每颗 stack；random 地址空间按聚合容量扩展。
  int stack_count = 1;
};

struct TrafficStreamStats {
  // host_requests 是拆分前的普通读写请求数；dram_transactions 是拆分后送入
  // Controller 的普通读写事务数。emitted_requests 还包含初始化/训练控制请求。
  std::uint64_t host_requests = 0;
  std::uint64_t dram_transactions = 0;
  std::uint64_t emitted_requests = 0;
  std::uint64_t burst_trace_lines = 0;
  std::uint64_t burst_split_requests = 0;
  std::uint64_t burst_read_requests = 0;
  std::uint64_t burst_write_requests = 0;
  std::uint64_t burst_read_bytes = 0;
  std::uint64_t burst_write_bytes = 0;
};

class TrafficStream : public RequestSource {
 public:
  ~TrafficStream() override = default;
  virtual const TrafficStreamStats& stream_stats() const = 0;
};

std::vector<Request> generate_traffic(const DramSpec& spec, const TrafficOptions& options);
// 大型 CLI 运行使用的流式路径，包含可选控制序列，不在内存中构造完整工作负载。
std::unique_ptr<TrafficStream> make_traffic_stream(const DramSpec& spec,
                                                   const TrafficOptions& options);
std::vector<Request> generate_control_sequence(const DramSpec& spec, const TrafficOptions& options);
void prepend_control_sequence(const DramSpec& spec, const TrafficOptions& options, std::vector<Request>& requests);
// generate_traffic() 的返回值已经按 spec 的 AddressMapper 填好 decoded 字段。
// 调用方不需要再做地址解析；如果后续要接 CPU trace，也建议保持这个约定。

}  // namespace hbm_sim
