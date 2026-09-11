#pragma once

// Core 层多控制器系统：负责 frontend 注入和 channel-to-controller 分发。
// 它对应 Ramulator2.1 GenericDRAMSystem 的轻量化骨架。

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

#include "hbm_sim/controller/controller.hpp"
#include "hbm_sim/core/data.hpp"
#include "hbm_sim/core/request.hpp"
#include "hbm_sim/core/response.hpp"
#include "hbm_sim/dram/spec.hpp"
#include "hbm_sim/stats/stats.hpp"

namespace hbm_sim {

enum class StackQosPolicy {
  Fcfs,
  StrictPriority,
};

// 决定系统级保留哪一种响应视图。HostOnly 是普通 CPU/NoC/UCIe frontend
// 的默认选择；Both 只用于同时观察乱序 transaction 和重组后 host response。
enum class ResponseDeliveryMode {
  Disabled,
  HostOnly,
  TransactionOnly,
  Both,
};

struct MemorySystemOptions {
  // 每个 channel controller 使用同一份 ControllerOptions。后续如果要研究
  // asymmetric channel，可以把这里扩展成 vector<ControllerOptions>。
  ControllerOptions controller;
  // channel_mapper 决定请求进入哪个 channel controller。Decoded 表示信任
  // AddressMapper 的 decoded.channel；RoundRobin/Xor 用于实验性分散流量。
  ChannelMapperKind channel_mapper = ChannelMapperKind::Decoded;
  // stack_count=1 保持历史单-stack行为。多 stack 时，每颗 stack 拥有独立的
  // MemoryImage 和完整 channel-controller 集合，彼此不共享 JEDEC 状态。
  int stack_count = 1;
  StackMappingKind stack_mapping = StackMappingKind::Interleaved;
  std::uint64_t stack_interleave_bytes = 256;
  // CLI 可预先创建/加载各 stack 的镜像；为空时 MemorySystem 自动创建。
  std::vector<std::shared_ptr<MemoryImage>> stack_memory_images;
  // 每颗 stack 独立入口队列提供反压隔离；dispatch_width 表示每拍最多向该
  // stack 的 channel controllers 分发多少请求。
  std::size_t stack_ingress_buffer_size = 256;
  std::size_t stack_dispatch_width = 4;
  StackQosPolicy stack_qos_policy = StackQosPolicy::StrictPriority;
  // 0 表示不限制。非零容量用于提前模拟有限宽度的 NoC/UCIe/RTL 响应端；
  // 任一已选择的对外响应队列满时，completion 会保留在 Controller，不会丢弃。
  std::size_t transaction_response_queue_capacity = 0;
  std::size_t host_response_queue_capacity = 0;
  ResponseDeliveryMode response_delivery_mode = ResponseDeliveryMode::Disabled;
};

// Ramulator2.1 GenericDRAMSystem 风格的轻量 memory system：
// - 每个 stack 的每个 channel 对应一个 Controller
// - 系统地址先选择 stack，再用 stack-local 地址选择 channel/bank
// - 每个 system cycle 并行 tick 所有 stack/channel controller
class MemorySystem {
 public:
  using HostResponseConsumer = std::function<void(const HostResponse&)>;
  using TransactionResponseCallback =
      std::function<void(const TransactionResponse&)>;
  using HostResponseCallback = std::function<void(const HostResponse&)>;

  // A single streaming driver serves batch experiments and response-exporting hosts.
  // drain_responses models an always-ready host: every retained response is popped,
  // including views without a callback. Callbacks observe responses after each tick.
  struct RunOptions {
    bool drain_responses = false;
    HostResponseConsumer host_response;
    TransactionResponseCallback transaction_response;
    Cycle progress_interval = 0;
    RunProgressConsumer progress;
  };
  void run(RequestSource& source, Cycle max_cycles, const RunOptions& options);

  // 按 stack_count * spec.org.channels 创建 channel-local Controller。每个
  // Controller 的 channel 数会被 localize 成 1，因此内部 bank 下标是局部视角。
  explicit MemorySystem(DramSpec spec, MemorySystemOptions options = {});
  // 兼容旧代码的便捷构造：只传 ControllerOptions 时使用 decoded channel mapper。
  explicit MemorySystem(DramSpec spec, ControllerOptions controller_options);

  // 按 Request::inject_cycle 注入请求，并在每个 system cycle 并行 tick 所有 controller。
  void run(std::vector<Request> requests, Cycle max_cycles);
  void run(RequestSource& source, Cycle max_cycles);
  void run(RequestSource& source, Cycle max_cycles, Cycle progress_interval,
           RunProgressConsumer progress_consumer);
  // CLI/离线适配器可在每拍结束后立即消费 HostResponse，避免批处理运行把
  // payload 响应全部留到末尾。consumer 返回后该引用失效，需要保留时自行复制。
  void run(RequestSource& source, Cycle max_cycles,
           HostResponseConsumer consumer);

  // 初步异步 frontend 接口。try_submit() 返回 true 等价于请求 valid && ready
  // 握手成功；false 时调用方必须保持同一个请求并在后续 cycle 重试。
  bool try_submit(Request request);
  // 初始化、refresh 等控制流必须走显式 maintenance 端口；它参与 ingress
  // 反压，但按定义不产生 HostResponse。
  bool try_submit_maintenance(Request request);
  void step();
  // 自行使用 try_submit/step 的外部事件循环在退出时调用，生成与 run() 相同的
  // 聚合统计和 command trace。remaining_frontend_requests 用于 cycle-limit 口径。
  void finish(std::uint64_t remaining_frontend_requests = 0);
  bool idle() const;
  bool responses_drained() const;
  bool quiescent() const { return idle() && responses_drained(); }
  Cycle clock() const { return clk_; }
  // try_submit() 会自动启用；批处理 run() 若也要保留逐请求响应，应在运行前
  // 显式调用。默认关闭可避免纯性能实验为每个请求长期保存多份 payload。
  void enable_response_interface(bool enabled = true);
  void set_response_delivery_mode(ResponseDeliveryMode mode);
  ResponseDeliveryMode response_delivery_mode() const {
    return response_delivery_mode_;
  }

  // HostResponse 是 frontend 默认应消费的完整响应；TransactionResponse 用于
  // 调试或需要自行重组的适配器。front 在 pop 前保持稳定。
  bool has_response() const { return !host_responses_.empty(); }
  const HostResponse& front_response() const;
  HostResponse pop_response();
  bool has_transaction_response() const {
    return !transaction_responses_.empty();
  }
  const TransactionResponse& front_transaction_response() const;
  TransactionResponse pop_transaction_response();

  // callback 是非消费型通知：即使注册 callback，响应仍保留在相应队列中，
  // 便于日志与协议适配器同时观察。正式 RTL 接口可只使用轮询 ready/valid。
  void set_transaction_response_callback(TransactionResponseCallback callback);
  void set_response_callback(HostResponseCallback callback);

  const Stats& stats() const { return stats_; }
  const DramSpec& spec() const { return spec_; }
  // Controller 的命令、队列、PHY 字段是 channel-local；由于同一 stack 的
  // Controllers 共享一个 MemoryImage，其 storage/power/thermal/ECC 字段只是
  // 同一 stack 的共享快照，不能跨 controller 求和。物理统计应读取 stats()
  // 或 per_stack_stats()，这两个入口已按 MemoryImage 去重聚合。
  const std::vector<Controller>& controllers() const { return controllers_; }
  const std::vector<IssuedCommand>& issued_commands() const { return issued_; }
  std::size_t controller_count() const { return controllers_.size(); }
  int stack_count() const { return options_.stack_count; }
  const std::vector<std::shared_ptr<MemoryImage>>& stack_memory_images() const {
    return memory_images_;
  }
  const std::vector<Stats>& per_stack_stats() const { return per_stack_stats_; }

 private:
  // 全局 spec 保留原始 channel 数，用于带宽、输出和 channel mapper。
  DramSpec spec_;
  MemorySystemOptions options_;
  std::vector<std::shared_ptr<MemoryImage>> memory_images_;
  // 按 [stack][channel] 压平；每个元素代表一个 channel-local Controller。
  std::vector<Controller> controllers_;
  // 记录 workload 是否真正打到某个 controller，用于 active_controllers 输出。
  std::vector<bool> active_controller_seen_;
  std::vector<bool> active_stack_seen_;
  std::vector<std::deque<Request>> stack_ingress_queues_;
  std::vector<Stats> per_stack_stats_;
  std::uint64_t next_system_sequence_ = 0;
  // 合并后的全局命令 trace；collect_issued_commands() 会把局部 channel 写回全局坐标。
  std::vector<IssuedCommand> issued_;
  Stats stats_;
  // 外层 memory system cycle；所有 controller 在同一个 clk_ 上各 tick 一次。
  Cycle clk_ = 0;
  // frontend_stall_cycles_ 统计请求想注入但目标 stack ingress 满的 cycle。
  std::uint64_t frontend_stall_cycles_ = 0;
  std::optional<Cycle> last_async_frontend_stall_cycle_;
  std::uint64_t stack_ingress_stall_cycles_ = 0;
  std::uint64_t stack_ingress_peak_ = 0;
  std::uint64_t qos_priority_dispatches_ = 0;
  std::vector<std::uint64_t> per_stack_ingress_stalls_;
  std::vector<std::uint64_t> per_stack_ingress_peak_;
  std::vector<std::uint64_t> per_stack_qos_dispatches_;
  // max_cycles 结束时尚未注入的 frontend 请求数。
  std::uint64_t remaining_frontend_requests_ = 0;
  // RoundRobin mapper 的内部游标。
  std::vector<std::uint64_t> next_round_robin_channel_;
  std::deque<TransactionResponse> transaction_responses_;
  std::deque<HostResponse> host_responses_;

  struct HostAssembly {
    std::uint32_t transaction_count = 1;
    std::vector<std::optional<TransactionResponse>> transactions;
    std::size_t received = 0;
  };
  std::unordered_map<std::uint64_t, HostAssembly> host_assemblies_;

  // try_submit() 侧的 Host tag 生命周期。要求 index=0 先建立上下文，并在
  // HostResponse 被 frontend pop 后释放，防止正常有序调用中提前复用
  // host_request_id 导致跨请求错误重组。
  struct HostSubmission {
    std::uint32_t transaction_count = 1;
    RequestType type = RequestType::Read;
    std::vector<bool> accepted;
    std::size_t completed = 0;
  };
  std::unordered_map<std::uint64_t, HostSubmission> host_submissions_;
  TransactionResponseCallback transaction_response_callback_;
  HostResponseCallback host_response_callback_;
  ResponseDeliveryMode response_delivery_mode_ = ResponseDeliveryMode::Disabled;

  // 把 full-stack spec 转成单 channel controller spec。
  static DramSpec make_channel_spec(const DramSpec& spec);
  // 选择目标 channel；可能使用 decoded.channel，也可能按 mapper 覆盖。
  int target_channel(const Request& req);
  int target_stack(const Request& req) const;
  std::size_t controller_index(int stack, int channel) const;
  // 把系统地址转换成 stack-local 地址，并把全局 channel 转成本地 channel=0。
  Request localize_request(Request req, int stack, int channel) const;
  bool enqueue(Request req);
  void dispatch_stack_ingress();
  void collect_responses();
  void run_source(RequestSource& source, Cycle max_cycles, const RunOptions& options);
  void record_submitted_transaction(const Request& request);
  void record_completed_transaction(const TransactionResponse& response);
  void aggregate_response(const TransactionResponse& response);
  HostResponse build_host_response(const HostAssembly& assembly) const;
  void publish_host_response(HostResponse response);
  bool retains_host_responses() const;
  bool retains_transaction_responses() const;
  bool transaction_response_queue_full() const;
  bool host_response_queue_full() const;
  bool done() const;
  void tick();
  void finalize_run_stats();
  void collect_issued_commands();
};

}  // namespace hbm_sim
