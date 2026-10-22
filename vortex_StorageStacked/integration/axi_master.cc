// 引入主端适配器声明、请求扩展和 AXI 信号类型。
#include "axi_master.hh"
// 引入最小值、最大值和关联容器查找算法。
#include <algorithm>
// 引入整数类型边界，检查请求地址加长度是否溢出。
#include <limits>
// 引入运行时异常类型。
#include <stdexcept>

// 将主端实现放入 storage_axi 命名空间。
namespace storage_axi {
// 使用 SystemC 核心命名空间，简化模块与时间接口名称。
using namespace sc_core;
// 定义构造函数，接收模块名、槽位数、反压开关和日志目录。
Master::Master(sc_module_name n, unsigned s, bool st, const std::string& dir)
    // 初始化 SystemC 基类、容量、反压配置与事务输出流。
    : sc_module(n), slots(s), stalls(st), trace(dir + "/transactions.csv") {
    // 槽位数必须在 1 到 65534 之间且日志必须可写，否则抛出异常。
    if (slots == 0 || slots > 65534 || !trace) throw std::runtime_error("invalid master configuration");
    // 写入事务 CSV 表头，覆盖请求属性、分段数及各阶段时间。
    trace << "id,command,address,bytes,begin_tick,accepted_tick,axi_done_tick,end_resp_tick,segments,requestor,stream,substream,status\n";
    // 将非阻塞前向传输请求绑定到 transport。
    socket.register_nb_transport_fw(this, &Master::transport);
    // 将阻塞传输请求绑定到 blocking。
    socket.register_b_transport(this, &Master::blocking);
    // 将调试传输请求绑定到 debug。
    socket.register_transport_dbg(this, &Master::debug);
    // 注册 DMI 查询回调；当前回调始终拒绝直接内存访问。
    socket.register_get_direct_mem_ptr(this, &Master::dmi);
    // 注册 AXI 时钟方法 tick。
    SC_METHOD(tick);
    // 将 tick 的敏感事件设为 clk 上升沿。
    sensitive << clk.pos();
    // 禁止仿真初始化阶段自动调用 tick，等待实际时钟沿。
    dont_initialize();
// 结束主端构造函数。
}

// 定义把连续地址和字节长度拆为 AXI burst 列表的函数。
std::vector<Master::Burst> Master::split(uint64_t addr, unsigned len) {
    // 创建要返回的分段列表。
    std::vector<Burst> out;
    // 从 payload 的第 0 个字节开始拆分。
    unsigned offset = 0;
    // 只要尚有未覆盖的数据，就继续生成下一段。
    while (offset < len) {
        // 计算还未纳入 burst 的字节数。
        unsigned remaining = len - offset;
        // 先尝试使用完整 AXI 数据总线宽度的一拍，及其 AxSIZE 编码。
        unsigned beat = DataBytes, size = DataSize;
        // 若剩余数据不足一拍或地址未对齐，则逐次减半拍宽并减小 SIZE 编码。
        while (beat > remaining || addr % beat) { beat /= 2; --size; }
        // 拍数受剩余完整拍数和 AXI 单 burst 最多 256 拍的限制。
        unsigned beats = std::min({remaining / beat, 256u,
                                  // 同时限制 burst 不跨越下一个 4 KiB 地址边界。
                                  unsigned((4096 - (addr & 4095)) / beat)});
        // 保存该段的地址、payload 偏移、每拍字节数、拍数和 SIZE。
        out.push_back({addr, offset, beat, beats, size});
        // 计算这一段实际覆盖的总字节数。
        unsigned bytes = beat * beats;
        // 将地址和 payload 偏移同时前移到下一段起点。
        addr += bytes; offset += bytes;
    // 结束所有字节的 burst 拆分循环。
    }
    // 返回按地址顺序排列的分段列表。
    return out;
// 结束拆分函数。
}

// 定义处理 TLM 非阻塞前向协议的函数，接收 payload 引用。
tlm::tlm_sync_enum Master::transport(tlm::tlm_generic_payload& gp,
    // 继续接收协议阶段与相对延迟，进入函数体。
    tlm::tlm_phase& phase, sc_time& delay) {
    // BEGIN_REQ 表示上游开始提交新请求。
    if (phase == tlm::BEGIN_REQ) {
        // 按请求通道互斥约定，必须不存在另一个 pending 请求。
        sc_assert(pending == nullptr);
        // 要求 payload 具有内存管理器，以支持 acquire/release 生命周期管理。
        sc_assert(gp.has_mm());
        // 增加 payload 引用计数，使异步处理期间对象保持有效。
        gp.acquire();
        // 暂存此请求，等待时间与在途容量条件满足后接收。
        pending = &gp;
        // 将当前时间与注入延迟相加，记录请求的逻辑开始时间。
        pendingBegin = sc_time_stamp().value() + delay.value();
        // 取得由 gem5 转换钩子保存的请求属性扩展。
        auto* attrs = gp.get_extension<RequestAttributes>();
        // gem5 clears payloadDelay after packet2payload; preserve it in the
        // conversion hook and conservatively wait for the full payload.
        // 将请求开始时间与保存的数据到达延迟相加，得到完整 payload 的可用时间。
        pendingAt = sc_time::from_value(pendingBegin + (attrs ? attrs->payloadDelay : 0));
        // 告知上游已接受协议调用；稍后正式接收时另行发送 END_REQ。
        return tlm::TLM_ACCEPTED;
    // 结束新请求处理分支。
    }
    // 其余调用必须是当前正在响应的同一 payload 的 END_RESP。
    sc_assert(phase == tlm::END_RESP && responding == &gp);
    // 按当前 gem5/TLM 桥约定，END_RESP 不允许附带非零延迟。
    sc_assert(delay == SC_ZERO_TIME); // contract with bundled Gem5ToTlmBridge
    // 开始遍历在途表，查找持有当前 payload 的事务。
    auto it = std::find_if(active.begin(), active.end(),
                          // 比较每项保存的 payload 地址与 gp 地址以确定匹配项。
                          [&gp](const auto& p) { return p.second->gp == &gp; });
    // 确认该 payload 对应的在途事务确实存在。
    sc_assert(it != active.end());
    // 引用找到的事务，以读取 ID、时间和分段信息。
    Txn& t = *it->second;
    // 取得可选的请求属性扩展，供日志输出发起者和流标识。
    auto* a = gp.get_extension<RequestAttributes>();
    // 输出事务 ID、读写方向和原始访问地址。
    trace << t.id << ',' << (gp.is_write() ? 'W' : 'R') << ',' << gp.get_address()
          // 继续输出原始字节长度、逻辑开始和实际接收时刻。
          << ',' << gp.get_data_length() << ',' << t.begin << ',' << t.accept << ','
          // 继续输出 AXI 完成时刻、当前 END_RESP 时刻及 burst 段数。
          << t.axiDone << ',' << sc_time_stamp().value() << ',' << t.bursts.size()
          // 输出请求发起者与流 ID，无扩展时填零。
          << ',' << (a ? a->requestor : 0) << ',' << (a ? a->stream : 0)
          // 输出子流 ID 与最终 TLM 响应状态，结束该条记录。
          << ',' << (a ? a->substream : 0) << ',' << int(gp.get_response_status()) << '\n';
    // 立即刷新事务 CSV，保证已完成记录写入文件。
    trace.flush();
    // 清空当前响应占用，允许后续事务发 BEGIN_RESP。
    responding = nullptr;
    // 删除在途表项，释放对应事务对象和槽位。
    active.erase(it);
    // 累计完成的 TLM 事务数量。
    ++completed;
    // 释放先前 acquire 的 payload 引用，由其内存管理器决定是否回收。
    gp.release();
    // 返回 TLM_COMPLETED，表示本笔四阶段事务彻底结束。
    return tlm::TLM_COMPLETED;
// 结束非阻塞传输函数。
}

// 定义调试/功能访问入口。
unsigned Master::debug(tlm::tlm_generic_payload& gp) {
    // This first version permits debug/atomic access only while drained.
    // 只允许在适配器排空且已配置功能访问回调时调用。
    sc_assert(idle() && functional);
    // 把 payload 交给功能回调，并返回它报告的处理字节数。
    return functional(gp);
// 结束调试访问入口。
}
// 阻塞传输复用调试路径；此实现不使用传入的延迟参数。
void Master::blocking(tlm::tlm_generic_payload& gp, sc_time&) { debug(gp); }

// 定义将 pending 请求正式纳入在途表的逻辑。
void Master::admit() {
    // 存在待接收请求但槽位已满时，累计一次容量等待。
    if (pending && active.size() >= slots) ++capacityDenials;
    // 没有请求、容量不足或尚未严格越过数据可用时刻时，本周期不接收。
    if (!pending || active.size() >= slots || pendingAt >= sc_time_stamp()) return;
    // 保存待接收 payload 的地址。
    auto* gp = pending;
    // 清除 pending 槽位，为下一笔 BEGIN_REQ 腾出位置。
    pending = nullptr;
    // 检查最大 AXI ID 有效，且 ID 数量足够覆盖全部事务槽位。
    sc_assert(maxId > 0 && maxId <= 65535 && slots <= maxId);
    // 候选 ID 为零或超过配置上限时，从 1 重新开始。
    if (!nextId || nextId > maxId) nextId = 1;
    // 跳过仍在使用的 ID，达到最大 ID 后回绕到 1。
    while (active.count(nextId)) nextId = nextId == maxId ? 1 : nextId + 1;
    // 分配并值初始化一个在途事务对象。
    auto t = std::make_unique<Txn>();
    // 绑定 payload、保存本次 ID，并推进下一候选 ID。
    t->gp = gp; t->id = nextId++;
    // 保存先前记录的开始时刻与当前正式接收时刻。
    t->begin = pendingBegin; t->accept = sc_time_stamp().value();
    // 首先要求命令为读或写，且数据指针非空。
    bool valid = (gp->is_read() || gp->is_write()) && gp->get_data_ptr() &&
                 // 同时要求长度非零且不超过 65536 字节。
                 gp->get_data_length() && gp->get_data_length() <= 65536 &&
                 // 同时要求地址加长度不会超出 uint64_t 的可表示范围。
                 gp->get_address() <= std::numeric_limits<uint64_t>::max() - gp->get_data_length();
    // 不支持 streaming width 小于数据长度的循环地址传输。
    if (gp->get_streaming_width() < gp->get_data_length()) valid = false;
    // 有字节使能指针时必须有非零掩码长度，避免后续循环索引除零。
    if (gp->get_byte_enable_ptr() && !gp->get_byte_enable_length()) valid = false;
    // 合法请求预置 OK 状态，非法请求预置 BURST_ERROR 状态。
    gp->set_response_status(valid ? tlm::TLM_OK_RESPONSE : tlm::TLM_BURST_ERROR_RESPONSE);
    // 只对合法请求按地址和长度生成 AXI burst 分段。
    if (valid) t->bursts = split(gp->get_address(), gp->get_data_length());
    // 保存事务 ID，以便在独占指针移入容器后继续索引。
    uint16_t id = t->id;
    // 将事务所有权移入按 ID 索引的在途表。
    active.emplace(id, std::move(t));
    // 累计已正式接收的请求数，包含会返回错误的非法请求。
    ++accepted;
    // 更新同时占用的在途事务槽位峰值。
    maxActive = std::max<uint64_t>(maxActive, active.size());
    // 准备向上游回送 END_REQ 阶段，结束请求通道占用。
    tlm::tlm_phase p = tlm::END_REQ;
    // END_REQ 回调不附加额外时间延迟。
    sc_time d = SC_ZERO_TIME;
    // 通过 TLM 反向接口通知上游，请求已经正式接收。
    socket->nb_transport_bw(*gp, p, d);
    // 合法请求将首个 burst 加入 AXI 通道队列。
    if (valid) enqueueSegment(*active.at(id));
    // 非法请求不访问 AXI，直接记录完成时刻并进入待响应队列。
    else { active.at(id)->axiDone = sc_time_stamp().value(); responses.push_back(id); }
// 结束请求接收逻辑。
}

// 定义把当前分段加入通道队列的方法。
void Master::enqueueSegment(Txn& t) {
    // 每开始一个分段，将已发送写拍数与已接收读拍数清零。
    t.wbeat = t.rbeat = 0;
    // Alternate AW-leading and W-leading transfers. W order remains FIFO.
    // 启用 stalls 且 ID 为奇数时，写地址通道推迟 3 个 AXI 周期。
    t.awAfter = cycle + (stalls && (t.id & 1) ? 3 : 0);
    // 启用 stalls 且 ID 为偶数时，写数据通道推迟 3 个 AXI 周期，形成 AW/W 不同先后关系。
    t.wAfter = cycle + (stalls && !(t.id & 1) ? 3 : 0);
    // 读地址通道从当前周期起允许发送。
    t.arAfter = cycle;
    // 写事务分别加入 AW 与 W 队列，W 队列始终维持 FIFO 顺序。
    if (t.gp->is_write()) { awq.push_back(t.id); wq.push_back(t.id); }
    // 读事务加入 AR 队列。
    else arq.push_back(t.id);
// 结束分段入队逻辑。
}
// 定义当前 burst 完成后的处理函数。
void Master::finishSegment(Txn& t) {
    // 前移分段索引；若还有剩余段，则将下一段加入通道队列。
    if (++t.segment < t.bursts.size()) enqueueSegment(t);
    // 全部分段结束时记录 AXI 完成时刻，并将事务 ID 加入 TLM 响应队列。
    else { t.axiDone = sc_time_stamp().value(); responses.push_back(t.id); }
// 结束分段完成逻辑。
}

// 定义每个 AXI 上升沿的协议推进方法。
void Master::tick() {
    // 递增周期计数，供通道延迟与反压模式使用。
    ++cycle;
    // 低有效复位被拉低时进入复位分支。
    if (!resetn.read()) {
        // 要求复位时没有在途事务，当前实现不支持中途取消活动事务。
        sc_assert(active.empty());
        // 复位期间撤销写地址、写数据和读地址有效信号。
        axi.awvalid = false; axi.wvalid = false; axi.arvalid = false;
        // 复位期间不接收读响应与写响应。
        axi.rready = false; axi.bready = false;
        // 结束本周期处理，跳过接收、驱动和 TLM 回包。
        return;
    // 结束复位分支。
    }
    // AW 握手后确认队列非空，并弹出已经发送的写地址 ID。
    if (axi.awvalid.read() && axi.awready.read()) { sc_assert(!awq.empty()); awq.pop_front(); }
    // AR 握手后确认队列非空，并弹出已经发送的读地址 ID。
    if (axi.arvalid.read() && axi.arready.read()) { sc_assert(!arq.empty()); arq.pop_front(); }
    // 写数据通道完成一次 VALID/READY 握手时更新写拍进度。
    if (axi.wvalid.read() && axi.wready.read()) {
        // 取得写数据队首事务。
        auto& t = *active.at(wq.front());
        // 增加已发送写拍数，当前 burst 最后一拍完成后弹出 W 队首。
        if (++t.wbeat == t.bursts[t.segment].beats) wq.pop_front();
    // 结束写数据握手分支。
    }
    // 读响应通道握手时接收当前数据拍。
    if (axi.rvalid.read() && axi.rready.read()) {
        // 按返回的 RID 查找在途事务。
        auto& t = *active.at(axi.rid.read().to_uint());
        // 确认响应属于读请求。
        sc_assert(t.gp->is_read());
        // 引用该事务当前正在执行的 burst。
        auto& b = t.bursts[t.segment];
        // 检查 RLAST 与当前 burst 的预期末拍完全一致。
        sc_assert(axi.rlast.read() == (t.rbeat + 1 == b.beats));
        // 任意读拍返回非零响应码时，将整笔 TLM 请求标记为地址错误。
        if (axi.rresp.read() != 0) t.gp->set_response_status(tlm::TLM_ADDRESS_ERROR_RESPONSE);
        // 读取整条 AXI 读数据总线。
        const Data data = axi.rdata.read();
        // 根据当前读拍地址计算其在 AXI 总线中的起始字节 lane。
        unsigned lane = (b.address + t.rbeat * b.bytes) % DataBytes;
        // 遍历当前窄拍中的有效字节。
        for (unsigned j = 0; j < b.bytes; ++j) {
            // 计算该字节在原始 payload 缓冲区中的位置。
            unsigned off = b.offset + t.rbeat * b.bytes + j;
            // 取得 payload 字节使能指针；空指针表示所有字节均有效。
            auto* mask = t.gp->get_byte_enable_ptr();
            // 按掩码长度循环索引，只有无掩码或当前字节被使能时才更新。
            if (!mask || mask[off % t.gp->get_byte_enable_length()])
                // 从 AXI 总线对应的 8 位范围提取数据，写回 payload。
                t.gp->get_data_ptr()[off] = data.range(8*(lane+j)+7, 8*(lane+j)).to_uint();
        // 结束当前读拍的逐字节数据重组循环。
        }
        // 增加已接收读拍数，收齐当前 burst 后转入分段完成处理。
        if (++t.rbeat == b.beats) finishSegment(t);
    // 结束读响应握手分支。
    }
    // 写响应通道握手时处理当前 B 响应。
    if (axi.bvalid.read() && axi.bready.read()) {
        // 按 BID 查找在途事务。
        auto& t = *active.at(axi.bid.read().to_uint());
        // 确认该响应属于写请求。
        sc_assert(t.gp->is_write());
        // 非零 BRESP 将整笔 TLM 事务标记为地址错误。
        if (axi.bresp.read() != 0) t.gp->set_response_status(tlm::TLM_ADDRESS_ERROR_RESPONSE);
        // 当前写 burst 已完成，排入下一段或最终响应队列。
        finishSegment(t);
    // 结束写响应处理分支。
    }
    // 在本周期响应释放进度更新后，尝试接收 pending 请求。
    admit();
    // 根据更新后的通道队列驱动后续 AXI 信号。
    drive();
    // 尝试为已完成的事务发出 TLM BEGIN_RESP。
    respond();
// 结束 AXI 时钟方法。
}

// 定义根据队首事务设置各 AXI 输出信号的方法。
void Master::drive() {
    // 默认撤销 AWVALID，避免空队列产生请求。
    axi.awvalid = false;
    // 写地址队列非空时准备队首 AW。
    if (!awq.empty()) {
        // 取得队首事务与它当前正在执行的 burst。
        auto& t = *active.at(awq.front()); auto& b = t.bursts[t.segment];
        // 输出 burst 起始地址、事务 ID 和拍数减一的 AWLEN。
        axi.awaddr = b.address; axi.awid = t.id; axi.awlen = b.beats - 1;
        // 输出每拍字节数的指数编码 AWSIZE，并选择 INCR 地址递增 burst。
        axi.awsize = b.size; axi.awburst = 1;
        // 到达为该事务指定的延迟阈值后才拉高 AWVALID。
        axi.awvalid = cycle >= t.awAfter;
    // 结束写地址驱动分支。
    }
    // 默认撤销 ARVALID。
    axi.arvalid = false;
    // 读地址队列非空时准备队首 AR。
    if (!arq.empty()) {
        // 取得读地址队首事务和当前 burst。
        auto& t = *active.at(arq.front()); auto& b = t.bursts[t.segment];
        // 输出读地址、事务 ID 和拍数减一的 ARLEN。
        axi.araddr = b.address; axi.arid = t.id; axi.arlen = b.beats - 1;
        // 输出 ARSIZE 和 INCR burst 类型。
        axi.arsize = b.size; axi.arburst = 1;
        // 到达读地址允许周期后拉高 ARVALID。
        axi.arvalid = cycle >= t.arAfter;
    // 结束读地址驱动分支。
    }
    // 默认撤销 WVALID。
    axi.wvalid = false;
    // 写数据队列非空时准备队首数据拍。
    if (!wq.empty()) {
        // 取得写数据队首事务与当前 burst。
        auto& t = *active.at(wq.front()); auto& b = t.bursts[t.segment];
        // 将整条 AXI 数据总线与字节写使能初始清零。
        Data data = 0; uint32_t strb = 0;
        // 根据当前写拍地址确定其在总线中的起始字节 lane。
        unsigned lane = (b.address + t.wbeat * b.bytes) % DataBytes;
        // 遍历当前拍要发送的各字节。
        for (unsigned j = 0; j < b.bytes; ++j) {
            // 计算该字节在原始 payload 中的绝对偏移。
            unsigned off = b.offset + t.wbeat * b.bytes + j;
            // 将 payload 字节放入 AXI 数据总线对应的 8 位范围。
            data.range(8*(lane+j)+7, 8*(lane+j)) = t.gp->get_data_ptr()[off];
            // 取得可选的字节使能数组。
            auto* mask = t.gp->get_byte_enable_ptr();
            // 若无掩码或循环索引到的掩码非零，则拉高对应总线字节的 WSTRB。
            if (!mask || mask[off % t.gp->get_byte_enable_length()]) strb |= uint32_t(1) << (lane + j);
        // 结束本拍数据及字节写使能的组装循环。
        }
        // 输出 WDATA、WSTRB，并只在本 burst 最后一拍设置 WLAST。
        axi.wdata = data; axi.wstrb = strb; axi.wlast = t.wbeat + 1 == b.beats;
        // 达到 W 通道延迟阈值后拉高 WVALID。
        axi.wvalid = cycle >= t.wAfter;
    // 结束写数据驱动分支。
    }
    // 未启用 stalls 时持续接收读响应；启用时每 7 个周期的后 4 个周期接收。
    axi.rready = !stalls || cycle % 7 >= 3;
    // 未启用 stalls 时持续接收写响应；启用时每 5 个周期的后 3 个周期接收。
    axi.bready = !stalls || cycle % 5 >= 2;
// 结束 AXI 输出驱动方法。
}
// 定义从完成队列发起 TLM 响应的方法。
void Master::respond() {
    // 响应通道已被占用或没有待响应事务时，等待后续周期。
    if (responding || responses.empty()) return;
    // 取得完成队首对应的 payload，标记它正在响应。
    responding = active.at(responses.front())->gp;
    // 弹出已开始响应的事务 ID，事务本身仍保留在在途表中。
    responses.pop_front();
    // 准备向上游发送 BEGIN_RESP 阶段。
    tlm::tlm_phase p = tlm::BEGIN_RESP;
    // BEGIN_RESP 不附加额外时间延迟。
    sc_time d = SC_ZERO_TIME;
    // 经 TLM 反向接口将响应交给上游，等待之后的 END_RESP。
    auto result = socket->nb_transport_bw(*responding, p, d);
    // 按当前 gem5 桥约定，上游必须返回 TLM_ACCEPTED，不能同步结束该响应。
    sc_assert(result == tlm::TLM_ACCEPTED); // bundled gem5 bridge contract
// 结束 TLM 响应发送方法。
}
// 结束 storage_axi 命名空间。
}
