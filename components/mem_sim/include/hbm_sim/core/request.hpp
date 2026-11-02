#pragma once

// Frontend 与 Controller 之间的请求格式。Request 同时携带原始地址和预解码地址，
// 便于调试，也避免 Controller 热路径反复做地址映射。

#include <cstddef>
#include <cstdint>
#include <functional>

#include "hbm_sim/core/addr_map.hpp"
#include "hbm_sim/core/common.hpp"
#include "hbm_sim/core/data.hpp"

namespace hbm_sim {

// 进入控制器的内存请求。地址在生成流量或读取 trace 时提前解码，控制器热路径
// 只需要读 decoded 中的 bank/row/column 字段。
struct Request {
  // id 只用于追踪与测试，不参与调度优先级。
  std::uint64_t id = 0;
  // 原始 byte address。调度热路径主要使用 decoded，但保留 address 方便输出 trace。
  Address address = 0;
  // 多 stack 路由后 address 改为 stack-local 地址；system_address 保留原始
  // 全局地址，供 trace、完成返回和审计使用。target_stack=-1 表示尚未路由。
  Address system_address = 0;
  bool has_system_address = false;
  int target_stack = -1;
  bool has_explicit_stack = false;
  int target_channel = -1;
  // 数字越大优先级越高；0 是普通流量。它只参与 stack ingress QoS，
  // 进入单 channel Controller 后仍由现有 FR-FCFS/FCFS 调度。
  int qos_class = 0;
  std::uint64_t system_sequence = 0;
  // 上层读/写意图，最终会映射到 RD 或 WR。
  RequestType type = RequestType::Read;
  // 预解码的 DRAM 层级坐标，避免每个 cycle 调度时重复做地址映射。
  DecodedAddress decoded;
  // 保存全局堆叠坐标。MemorySystem 把请求交给控制器前会把
  // decoded.channel 局部化为 0。
  DecodedAddress storage_decoded;
  bool has_storage_decoded = false;
  // 请求进入控制器队列的 cycle，用于读延迟统计。
  Cycle arrival = 0;
  // Controller 接受请求时分配的单调序号。它只用于保护重叠地址的请求顺序，
  // 不参与 FR-FCFS 对彼此独立地址的调度优先级。
  std::uint64_t controller_sequence = 0;
  // 请求从 frontend 注入控制器的目标 cycle。arrival 是实际入队时间；
  // inject_cycle 是 trace/frontend 想要发送请求的时间，两者分开后才能建模
  // controller buffer backpressure。
  Cycle inject_cycle = 0;
  // RD/WR 发出后填入完成 cycle，pending_ 根据它判断何时统计完成。
  Cycle completion = 0;
  // 终端 RD/RDA/WR/WRA 发射的 cycle。旁路转发/合并则记录被 Controller
  // 接受的 cycle，供异步响应和上层端到端延迟分析使用。
  Cycle issued_cycle = 0;
  // true 表示该请求由控制器内部转发/合并完成，不占用 DRAM 数据总线。
  bool bypass_dram = false;
  // frontend host request 拆分信息。host_request_id 保留拆分前的请求标识；
  // transaction_index/count 说明当前 Request 是其中哪一个 DRAM transaction。
  std::uint64_t host_request_id = 0;
  std::uint32_t transaction_index = 0;
  std::uint32_t transaction_count = 1;
  // 当前子事务实际搬运的字节数。0 表示由 payload/expected 或 spec 默认值推导。
  std::size_t transfer_bytes = 0;

  // 真实存储数据。写请求提交到 MemoryImage；读请求在提供期望值时比较返回数据。
  ByteVector payload;
  ByteVector expected_payload;
  ByteVector byte_mask;
  bool has_payload = false;
  bool has_expected_payload = false;
  bool has_byte_mask = false;
  // WR/WRA 已把本请求数据提交到存储模型时置位，避免完成统计再次写入。
  bool data_committed = false;
  // 完成路径内部元数据。普通 Direct/Behavioral 读在完成时填入；写缓冲转发
  // 会在建立数据快照时填入，以便异步响应保留 SECDED 结果。
  bool response_ecc_corrected = false;
  bool response_ecc_uncorrectable = false;
  // 本请求实际发出的终端 RD/RDA/WR/WRA。完成路径和 DFI 导出使用该字段区分
  // 自动预充电形式及 LPDDR/HBM 命令展开。
  Command issued_data_command = Command::NOP;

  // 对维护请求而言，next 保存最终维护命令，例如 REFpb/RFMpb。普通读写请求
  // 仍由 Controller::next_command() 根据 row 状态即时推导。
  Command next = Command::NOP;
  // LPDDR split activate 专用：ACT1 发出后，请求必须继续拥有该 bank 的 ACT2。
  bool issued_first_activate = false;
  // LPDDR WCK/CAS 专用：本请求是否已经发过 CAS_RD/CAS_WR 建立 WCK2CK sync。
  bool cas_sync_issued = false;

  // row hit/miss/conflict 是请求进入控制器状态机时的一次性分类，不能在
  // ACT/PRE/RD/WR 多个阶段重复计数。
  bool row_status_counted = false;

  // 突发轨迹支持：BW/BR 会展开成多个请求。burst_id 标识同一突发行，
  // burst_offset 表示请求在突发中的字节偏移。
  std::uint64_t burst_id = 0;
  std::uint64_t burst_offset = 0;
};

// 流式前端接口。请求必须按 inject_cycle 非递减输出，使 Controller/MemorySystem
// 只保留一个待注入请求即可表达反压。
class RequestSource {
 public:
  virtual ~RequestSource() = default;
  virtual bool next(Request& request) = 0;
  virtual std::optional<std::uint64_t> remaining_hint() const = 0;
};

// 长运行的轻量进度快照。它只用于观测，不改变仿真状态；remaining_frontend
// 是尚未从 source 成功提交的请求数（source 无精确 hint 时为保守下界）。
struct RunProgress {
  Cycle cycle = 0;
  std::uint64_t completed_reads = 0;
  std::uint64_t completed_writes = 0;
  std::uint64_t remaining_frontend = 0;
};

using RunProgressConsumer = std::function<void(const RunProgress&)>;

}  // namespace hbm_sim
