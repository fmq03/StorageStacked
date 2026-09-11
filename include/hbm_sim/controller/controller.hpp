#pragma once

#include "hbm_sim/controller/request_queues.hpp"

// Controller 层顶层接口：一个 Controller 对应一个 channel 内的调度与 DRAM
// 状态。 MemorySystem 可以按 [stack][channel] 并行持有多个 Controller；跨 stack
// 路由、 反压和 QoS 位于 system 层，不污染这里的单 channel JEDEC 调度状态。

#include <cstddef>
#include <deque>
#include <memory>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "hbm_sim/controller/command.hpp"
#include "hbm_sim/controller/refresh.hpp"
#include "hbm_sim/controller/rfm.hpp"
#include "hbm_sim/controller/row_policy.hpp"
#include "hbm_sim/controller/timing.hpp"
#include "hbm_sim/core/data.hpp"
#include "hbm_sim/core/request.hpp"
#include "hbm_sim/core/response.hpp"
#include "hbm_sim/dram/bank_state.hpp"
#include "hbm_sim/dram/mem_phy.hpp"
#include "hbm_sim/dram/spec.hpp"
#include "hbm_sim/stats/stats.hpp"

namespace hbm_sim {

struct ControllerOptions {
  // 普通读队列容量。frontend 注入时如果队列满，MemorySystem/Controller 会记录
  // stall。
  std::size_t read_buffer_size = 32;
  // 普通写队列容量。写请求进入队列后会把地址加入 buffered_write_addrs_，
  // 使后续同地址读可以 forward、同地址写可以 coalesce。
  std::size_t write_buffer_size = 32;
  // priority buffer 容量。refresh/RFM/row-policy precharge 都进入这个队列。
  std::size_t priority_buffer_size = 2048;
  // 完整命令保留用于测试、CSV 和验证，但内存随请求数增长；大型运行默认关闭。
  bool retain_command_trace = true;
  // 写排空低水位。write_mode_ 打开后，写队列降到该比例以下才回到读优先。
  double write_low_watermark = 0.2;
  // 写排空高水位。写队列超过该比例时进入 write-drain 模式。
  double write_high_watermark = 0.8;
  // 请求选择策略；只影响候选排序，不改变命令状态机。
  SchedulerKind scheduler = SchedulerKind::FRFCFS;
  // 行策略；决定 row hit 后使用 RD/WR、RDA/WRA，或 ClosedCAP 计数触发关闭。
  RowPolicyKind row_policy = RowPolicyKind::OpenPage;
  // ClosedCAP 策略下，同一 bank 连续列访问达到 cap 后优先 auto-precharge。
  int row_policy_cap = 4;
  // 可选共享真实存储镜像。MemorySystem 将同一镜像交给所有通道控制器。
  std::shared_ptr<MemoryImage> memory_image;
  // 可选共享数据验证器。它记录错误明细，但不在 RD/WR 时序路径执行文件 I/O。
  std::shared_ptr<DataValidator> data_validator;
  // 可选 MC-PHY-Stack 路径。Direct 保持历史行为，Behavioral 启用在线 PHY。
  MemPhyOptions phy;
  // 本控制器代表的全局通道。调度使用局部通道 0，存储和物理事件保留原通道。
  int global_channel_id = 0;
  // 0 表示不限制响应队列。设置为非零时，Frontend 不取走响应会最终反压
  // pending completion，但已经完成的数据不会丢失。
  std::size_t response_queue_capacity = 0;
  // 独立 Controller 的异步用户默认保留事务响应。只做统计的 CLI 单控制器
  // 批处理会关闭它，避免请求数很大时积累无用 payload；MemorySystem 始终覆盖
  // 为 true，由系统层选择保留 Host/Transaction 哪一种视图。
  bool retain_responses = true;
};

// 简化 FR-FCFS 风格控制器。HBM 模式下支持 row/column 双发射；LPDDR 模式下
// 使用统一命令总线，并显式建模 ACT1/ACT2、CAS_RD/CAS_WR 与 WCK 活跃窗口。
class Controller {
public:
  // 构造函数复制 DramSpec，形成该 controller 的配置快照。这样一次仿真中
  // 外部即使修改原始 spec，也不会影响正在运行的 controller。
  explicit Controller(DramSpec spec, ControllerOptions options = {});

  // 尝试把一个请求放入对应 buffer。返回 false
  // 表示队列满，调用者应在下一拍重试。
  bool enqueue(Request req);
  // 带 frontend 注入时序的运行入口。requests 会按 inject_cycle 排序后逐拍注入。
  void run(std::vector<Request> requests, Cycle max_cycles);
  void run(RequestSource &source, Cycle max_cycles);
  void run(RequestSource &source, Cycle max_cycles, Cycle progress_interval,
           RunProgressConsumer progress_consumer);
  // 只运行当前已经入队的请求，常用于 sequence test 手工 enqueue 后推进。
  void run_until_done(Cycle max_cycles);
  // 推进一个 controller tick。HBM edge pairing 下，一个 tick 可以代表半个 nCK。
  void tick();
  // 所有 request buffer、maintenance backlog 和 pending completion 都为空时为
  // done。
  bool done() const;
  // 计算带宽、remaining、cycle-limit 等收尾统计。run()/run_until_done()
  // 会自动调用。该函数会显式写回 dirty row buffer（不改变 open-row 状态）
  // 并 flush 持久化后端；失败会
  // 通过异常返回给调用方，不能仅依赖析构函数的 noexcept 兜底。
  void finalize_run_stats();
  void flush_storage();
  Cycle clock() const { return clk_; }

  // Controller 级事务完成端口。front_response() 返回的对象在 pop_response()
  // 前保持稳定，对应 ready/valid 接口中 valid=1、ready=0 的保持语义。
  bool has_response() const { return !responses_.empty(); }
  const TransactionResponse &front_response() const;
  TransactionResponse pop_response();

  const Stats &stats() const { return stats_; }
  const DramSpec &spec() const { return spec_; }
  const std::vector<IssuedCommand> &issued_commands() const { return issued_; }

private:
  using BufferKind = RequestQueueKind;

  struct RisingEdgeCommandInfo {
    // 最近一次 rising edge 行命令，用于判断下一次 falling edge PRE 是否可
    // pairing。
    Command command = Command::NOP;
    // HBM pairing 通常按 pseudo-channel 观察；不同 PC 的 falling PRE 可独立。
    int pseudo_channel = -1;
    // bank_key 把 sid/bank-group/bank 压成一个比较键，用于判断是否同 bank。
    int bank_key = -1;
    // 只有等于该 cycle 的 falling edge 需要受上一次 rising 命令限制。
    Cycle next_pairing_falling_edge = 0;
  };

  // choose() 找到的本周期候选命令，index 指向某个 request buffer 中的请求。
  struct Candidate {
    // buffer 中的下标。issue() 后可能被 erase 或移动到 active buffer。
    std::size_t index = 0;
    BufferKind buffer = BufferKind::Read;
    // 本 cycle 准备发出的下一条命令。
    Command command = Command::NOP;
    // choose() 所服务的总线类型。
    BusClass bus = BusClass::Unified;
    // false 表示本总线本周期没有找到 timing-ready 的命令。
    bool valid = false;
  };

  // choose() 期间记录“除 PHY 准入外已经满足调度条件”的候选被拒原因。
  // 每条总线每 tick 最终无命令发出时，才把它折算为一次反压统计。
  struct PhyBackpressureObservation {
    bool command = false;
    bool data = false;
  };

  // 同地址写可以合并成一条 DRAM 写命令，但重叠地址转发仍必须保持逐 byte
  // 的先后关系。request 保存最终待提交的数据/mask；byte_sequence[i] 记录
  // 该 byte 最后一次被 Controller 接受时的单调序号。仅按整个 Request 的
  // arrival/id 排序会把一次 masked merge 错当成所有 byte 同时更新。
  struct BufferedWriteState {
    Request request;
    std::vector<std::uint64_t> byte_sequence;
  };

  // DramSpec 是控制器的“配置快照”。构造后不再从外部修改，保证仿真一致性。
  DramSpec spec_;
  ControllerOptions options_;
  // 所有 bank 的局部状态，按 DecodedAddress::flat_bank() 压平。
  std::vector<BankState> banks_;
  // Ramulator 风格的 request buffers：active 保存已发 opening 命令、等待后续
  // ACT2/CAS/RD/WR 的请求；priority 保存 refresh/RFM；read/write 保存普通请求。
  RequestQueues request_queues_;
  std::deque<Request> pending_maintenance_;
  // 已发出 RD/WR、正在等待完成统计的请求。
  std::deque<Request> pending_;
  // 已完成但尚未被 MemorySystem/Frontend 接收的事务响应。
  std::deque<TransactionResponse> responses_;
  // Ramulator2.1 ControllerBase 风格的写缓冲地址集合：
  // - 后续读同地址可直接转发，不发 DRAM RD
  // - 后续写同地址可合并，不再占用 write buffer
  std::unordered_set<Address> buffered_write_addrs_;
  std::unordered_map<Address, BufferedWriteState> buffered_writes_;
  std::shared_ptr<MemoryImage> memory_image_;
  std::shared_ptr<DataValidator> data_validator_;
  std::unique_ptr<MemPhy> mem_phy_;
  // 当前 controller cycle。tick() 开头自增，因此第一轮调度发生在 cycle 1。
  Cycle clk_ = 0;
  // 全局统计，由 enqueue()/issue()/complete_pending()/run_until_done() 更新。
  Stats stats_;
  // 轻量命令 trace。主要供 tests/sequence_tests.cpp 做精确断言。
  std::vector<IssuedCommand> issued_;
  // 行策略、refresh 和 RFM 都有独立状态对象；Controller 只在 tick()
  // 中协调它们。
  RowPolicyEngine row_policy_;
  RefreshManager refresh_manager_;
  RfmManager rfm_manager_;
  TimingEngine timing_engine_;
  bool write_mode_ = false;
  std::uint64_t next_maintenance_id_ = 1;
  RisingEdgeCommandInfo rising_edge_;
  bool low_power_active_ = false;
  bool explicit_power_down_active_ = false;
  bool explicit_self_refresh_active_ = false;
  bool wck_retrain_required_ = false;
  // 普通请求唤醒显式 PDE/SREFEN 时，由 controller 注入对应退出维护命令；
  // 在该命令真正 issue 前保留状态，避免 PHY 与 controller 状态机脱节。
  std::optional<Command> explicit_low_power_exit_pending_;
  Cycle low_power_idle_since_ = 0;
  Cycle low_power_exit_until_ = 0;
  std::uint64_t next_controller_sequence_ = 1;

  // 扫描 pending_，把 completion <= clk_ 的请求转入完成统计。
  void complete_pending();
  TransactionResponse
  complete_read(Request &req, const MemPhyCompletion *phy_completion = nullptr);
  TransactionResponse
  complete_write(Request &req,
                 const MemPhyCompletion *phy_completion = nullptr);
  ResponseStatus check_read_data(const Request &req, const ByteVector &actual,
                                 bool initialized, bool forwarded);
  void ensure_write_payload(Request &req);
  bool read_hits_buffered_write(const Request &req) const;
  bool has_unresolved_overlapping_read(const Request &write,
                                       std::uint64_t older_than_sequence) const;
  bool has_unresolved_overlapping_write(const Request &read) const;
  ByteVector read_forward_payload(const Request &req, bool *initialized,
                                  ByteVector *initialized_mask) const;
  bool response_queue_full() const;
  TransactionResponse
  make_response(const Request &req, ByteVector data = {},
                ByteVector initialized_mask = {}, bool initialized = true,
                bool forwarded = false, bool coalesced = false,
                ResponseStatus status = ResponseStatus::Ok) const;
  DecodedAddress storage_decoded_for(const Request &req) const;
  void apply_storage_command_event(const Request &req, Command issued);
  void commit_write_data(Request &req);
  void annotate_issued_data(const Request &req, Command cmd,
                            const ByteVector &payload, const ByteVector *mask,
                            const ByteVector *initialized_mask,
                            bool initialized);

  // 在指定总线上选择一条 timing-ready 命令。choose() 自身不发命令，只返回候选；
  // issue() 才会修改状态。这样便于 HBM 同一 tick 先选 column 再选 row。
  Candidate choose(BusClass bus);
  // 在一个 buffer 内构造候选视图并调用 Scheduler。avoid_active_close 用于避免
  // 维护/PRE 命令关闭仍有 active request 的 bank。
  Candidate pick_best_ready_from(std::deque<Request> &buffer, BufferKind kind,
                                 BusClass bus, bool avoid_active_close,
                                 PhyBackpressureObservation *phy_block);
  // 判断候选是否能在当前 bus 上参与调度；这是 bus/active-close 级过滤，
  // timing/state 是否 ready 由 timing_ok() 另行判断。
  bool candidate_eligible(const Request &req, Command cmd, BusClass bus,
                          BufferKind kind, bool avoid_active_close) const;
  bool phy_admission_ok(Command cmd,
                        PhyBackpressureObservation *phy_block) const;
  // priority buffer 按队首服务，避免 refresh/RFM rotation 被重排。
  Candidate pick_priority_if(BusClass bus,
                             PhyBackpressureObservation *phy_block);
  // 根据 write watermark 在 read/write buffer 中选择普通请求。
  Candidate pick_rw_if(BusClass bus, PhyBackpressureObservation *phy_block);
  // LPDDR ACT1 后如果 ACT2 deadline 到达，优先抢占普通调度。
  Candidate pick_urgent_act2(BusClass bus,
                             PhyBackpressureObservation *phy_block);
  // 根据读写队列占用更新 write_mode_。
  void set_write_mode();
  std::deque<Request> &request_buffer(BufferKind kind);
  const std::deque<Request> &request_buffer(BufferKind kind) const;
  std::size_t queued_requests() const;
  bool buffer_has_space(BufferKind kind) const;
  // 根据请求目标 row 与 bank 状态推导下一条应发命令。
  std::optional<Command> next_command(Request &req);
  // 检查命令是否满足 bank-local 与 scope-level timing gate。
  bool timing_ok(const Request &req, Command cmd) const;
  bool constraint_timing_ok(const Request &req, Command cmd) const;
  // 判断命令是否能占用当前 choose() 正在服务的总线。
  bool bus_matches(const Request &req, Command cmd, BusClass bus) const;
  bool would_close_active(const Request &req, Command cmd,
                          BufferKind source) const;
  bool is_rising_edge() const;
  bool can_issue_falling_edge_pre(const Request &req, Command cmd) const;
  void record_rising_row_command(const Request &req, Command cmd);
  int bank_key(const DecodedAddress &decoded) const;

  // 发出候选命令并更新全部状态。所有命令副作用集中在这里，便于审计。
  void issue(Candidate cand);
  // RD/WR 或维护终端命令后让请求离开 buffer；opening 命令会移动到 active。
  void retire_or_advance(Candidate cand, Command issued);
  void erase_request(BufferKind kind, std::size_t index);
  void promote_to_active(BufferKind kind, std::size_t index);
  // 对请求做一次性 row hit/miss/conflict 分类。
  void classify_row_status(Request &req);
  // 记录命令 trace，不影响仿真行为。
  void append_issued(const Request &req, Command cmd, BusClass bus);

  // 命令分类函数保持独立，避免 bus_matches()/retire_or_advance() 中重复写条件。
  bool is_row_command(Command cmd) const;
  bool is_column_command(Command cmd) const;
  bool is_data_command(Command cmd) const;
  bool is_opening_command(Command cmd) const;
  bool is_maintenance_request(const Request &req) const;
  bool is_terminal_maintenance(const Request &req, Command cmd) const;
  bool is_all_bank_row_command(Command cmd) const;
  bool any_bank_busy_in_channel(const DecodedAddress &decoded) const;
  bool any_bank_busy_in_rank(const DecodedAddress &decoded) const;
  bool any_bank_activating_in_rank(const DecodedAddress &decoded) const;
  std::pair<int, int> rank_bank_range(const DecodedAddress &decoded) const;

  Cycle timing_delay(int cycles) const;
  void schedule_refresh();
  void service_pending_maintenance();
  void schedule_maintenance(Command cmd, const DecodedAddress &decoded);
  void apply_row_policy_pre_schedule();
  void try_upgrade_row_policy_command(Candidate &cand);
  void on_row_policy_issue(const Request &req, Command issued);
  DecodedAddress decoded_from_flat_bank(int flat_bank) const;
  bool dual_bank_target_idle(const DecodedAddress &decoded) const;
  // LPDDR WCK 窗口检查：RD/WR 只能发生在 ready_at 和 active_until 之间。
  bool wck_ready_for_data(const Request &req) const;
  bool state_ok(const Request &req, Command cmd) const;
};

} // namespace hbm_sim
