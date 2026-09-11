#include "hettrace/het_axi_monitor.hh"

#include <algorithm>
#include <limits>
#include <utility>

#include "base/logging.hh"
#include "hettrace/addrmap.h"
#include "mem/request.hh"
#include "sim/core.hh"
#include "sim/cur_tick.hh"
#include "sim/sim_exit.hh"
#include "sim/system.hh"

namespace gem5
{

namespace
{

unsigned
largestPowerOfTwo(unsigned value)
{
    unsigned result = 1;
    while (result <= value / 2)
        result <<= 1;
    return result;
}

} // anonymous namespace

HetAxiMonitor::HetAxiMonitor(const Params &p)
    : SimObject(p),
      memSidePort(name() + ".mem_side_port", *this),
      cpuSidePort(name() + ".cpu_side_port", *this),
      system(p.system),
      enable(p.trace_enable),
      traceHost(p.trace_host),
      traceVortex(p.trace_vortex),
      traceCoralNpu(p.trace_coralnpu),
      traceInstFetch(p.trace_inst_fetch),
      axiDataBytes(p.axi_data_bytes),
      axiIdBits(p.axi_id_bits),
      uniquePacketIds(p.unique_packet_ids),
      vortexPatterns(p.vortex_requestor_patterns),
      coralNpuPatterns(p.coralnpu_requestor_patterns)
{
    fatal_if(axiDataBytes == 0 || axiDataBytes > 64 ||
             (axiDataBytes & (axiDataBytes - 1)) != 0,
             "HetAxiMonitor axi_data_bytes must be a power of two in [1,64]");
    fatal_if(axiIdBits == 0 || axiIdBits > 16,
             "HetAxiMonitor axi_id_bits must be in [1,16]");
}

HetAxiMonitor::~HetAxiMonitor()
{
    closeTrace();
}

Port &
HetAxiMonitor::getPort(const std::string &if_name, PortID idx)
{
    if (if_name == "mem_side_port")
        return memSidePort;
    if (if_name == "cpu_side_port")
        return cpuSidePort;
    return SimObject::getPort(if_name, idx);
}

void
HetAxiMonitor::init()
{
    fatal_if(!cpuSidePort.isConnected() || !memSidePort.isConnected(),
             "HetAxiMonitor must be connected on both sides");
}

void
HetAxiMonitor::startup()
{
    if (!enable)
        return;

    // The address-map defaults use ps; native gem5/SystemC integrations may
    // select fs. Keep record ticks unchanged and describe their actual scale.
    const auto frequency = sim_clock::Frequency;
    const auto period = [frequency](uint64_t ticks) {
        return ticks * frequency / hettrace::kTicksPerSecond;
    };
    const bool host_open = traceHost && hostWriter.Open(
        hettrace::kSrcHost, "host", hettrace::kLevelInterconnect,
        period(hettrace::kClockPeriodTicks_host), axiDataBytes,
        hettrace::kMapAddrBits, true, frequency);
    const bool vortex_open = traceVortex && vortexWriter.Open(
        hettrace::kSrcVortex, "vortex", hettrace::kLevelInterconnect,
        period(hettrace::kClockPeriodTicks_vortex), axiDataBytes,
        hettrace::kMapAddrBits, true, frequency);
    const bool npu_open = traceCoralNpu && coralNpuWriter.Open(
        hettrace::kSrcCoralnpu, "coralnpu", hettrace::kLevelInterconnect,
        period(hettrace::kClockPeriodTicks_coralnpu), axiDataBytes,
        // CoralNPU 的 timing seam 保留原生地址/ID/WSTRB，但到这个
        // 统一点时已是 gem5 Packet。AW/W/B/AR/R 事件、LEN/SIZE/
        // BURST/USER 与响应时刻都由 monitor 重构，不能冒充 pin trace。
        hettrace::kNpuAddrBits, true, frequency);
    active = host_open || vortex_open || npu_open;
    if (!active)
        return;

    fatal_if((traceHost && !host_open) || (traceVortex && !vortex_open) ||
             (traceCoralNpu && !npu_open),
             "HetAxiMonitor could not open every requested source trace");
    inform("HetAxiMonitor: one interconnect tap enabled for %s%s%s",
           host_open ? "host " : "", vortex_open ? "vortex " : "",
           npu_open ? "coralnpu" : "");
    registerExitCallback([this] { closeTrace(); });
}

bool
HetAxiMonitor::containsPattern(
    const std::string &requestor_name,
    const std::vector<std::string> &patterns) const
{
    for (const auto &pattern : patterns) {
        if (!pattern.empty() && requestor_name.find(pattern) !=
                                    std::string::npos) {
            return true;
        }
    }
    return false;
}

HetAxiMonitor::Source
HetAxiMonitor::classify(RequestorID requestor_id) const
{
    const std::string requestor_name = system->getRequestorName(requestor_id);
    if (containsPattern(requestor_name, coralNpuPatterns))
        return Source::CoralNpu;
    if (containsPattern(requestor_name, vortexPatterns))
        return Source::Vortex;
    return Source::Host;
}

hettrace::TraceWriter &
HetAxiMonitor::writer(Source source)
{
    switch (source) {
      case Source::Host:
        return hostWriter;
      case Source::Vortex:
        return vortexWriter;
      case Source::CoralNpu:
        return coralNpuWriter;
    }
    panic("HetAxiMonitor: invalid source classification");
}

std::unique_ptr<HetAxiMonitor::TraceSenderState>
HetAxiMonitor::makeTraceState(PacketPtr pkt) const
{
    if (!active || (!pkt->isRead() && !pkt->isWrite()) ||
        pkt->getSize() == 0) {
        return nullptr;
    }
    if (pkt->req->isInstFetch() && !traceInstFetch)
        return nullptr;

    auto state = std::make_unique<TraceSenderState>();
    state->source = classify(pkt->req->requestorId());
    if ((state->source == Source::Host && !traceHost) ||
        (state->source == Source::Vortex && !traceVortex) ||
        (state->source == Source::CoralNpu && !traceCoralNpu)) {
        return nullptr;
    }
    state->write = pkt->isWrite();

    uint8_t flags = 0;
    if (pkt->req->isInstFetch())
        flags |= hettrace::kFlagInstr;
    if (pkt->req->isPrefetch())
        flags |= hettrace::kFlagPrefetch;
    // Vortex core requests carry stream/substream IDs.  CP copy-engine
    // requests do not, so they remain distinguishable after both paths have
    // converged on the same gem5 DmaPort.
    if (state->source == Source::Vortex && !pkt->req->hasStreamId())
        flags |= hettrace::kFlagDma;

    const uint32_t requestor = pkt->req->requestorId();
    const uint32_t stream = pkt->req->hasStreamId() ?
        pkt->req->streamId() : requestor;
    const uint32_t context =
        state->source == Source::Host ? requestor : stream;
    const uint32_t id_mask = (uint32_t(1) << axiIdBits) - 1;
    const uint16_t axi_id = static_cast<uint16_t>(stream & id_mask);
    const uint8_t user = state->source == Source::Host ?
        hettrace::kSrcHost : state->source == Source::Vortex ?
        hettrace::kSrcVortex : hettrace::kSrcCoralnpu;

    const auto &byte_enable = pkt->req->getByteEnable();
    Addr address = pkt->getAddr();
    unsigned remaining = pkt->getSize();
    unsigned packet_offset = 0;

    while (remaining != 0) {
        unsigned beat_bytes = largestPowerOfTwo(
            std::min(axiDataBytes, remaining));
        while (address % beat_bytes != 0 && beat_bytes > 1)
            beat_bytes >>= 1;

        const unsigned bytes_to_4k = 4096 - (address & 0xfff);
        const unsigned beats = std::min(
            {remaining / beat_bytes, 256U, bytes_to_4k / beat_bytes});
        panic_if(beats == 0,
                 "HetAxiMonitor cannot form AXI burst at address %#x",
                 address);

        LoggedTxn logged;
        logged.txn.addr = address;
        logged.txn.bytes = beats * beat_bytes;
        logged.txn.ctx = context;
        logged.txn.axi_id = axi_id;
        logged.txn.axi_size = hettrace::Log2Size(beat_bytes);
        logged.txn.burst = hettrace::kBurstIncr;
        logged.txn.user = user;
        logged.txn.flags = flags;

        if (state->write) {
            logged.strobes.reserve(beats);
            for (unsigned beat = 0; beat < beats; ++beat) {
                const Addr beat_address = address + beat * beat_bytes;
                uint64_t strobe = 0;
                for (unsigned byte = 0; byte < beat_bytes; ++byte) {
                    const unsigned source_byte =
                        packet_offset + beat * beat_bytes + byte;
                    const bool enabled = byte_enable.empty() ||
                        (source_byte < byte_enable.size() &&
                         byte_enable[source_byte]);
                    if (enabled) {
                        const unsigned lane =
                            (beat_address + byte) % axiDataBytes;
                        strobe |= uint64_t(1) << lane;
                    }
                }
                logged.strobes.push_back(strobe);
            }
        }

        packet_offset += logged.txn.bytes;
        address += logged.txn.bytes;
        remaining -= logged.txn.bytes;
        state->transactions.push_back(std::move(logged));
    }
    return state;
}

void
HetAxiMonitor::emitBegin(TraceSenderState &state)
{
    auto &trace_writer = writer(state.source);
    if (uniquePacketIds) {
        // gem5 packets from one requestor may complete out of order. Giving
        // all of them the requestor's ID would invent an AXI ordering error.
        const auto source = static_cast<unsigned>(state.source);
        const uint32_t count = uint32_t(1) << axiIdBits;
        for (uint32_t tried = 0; tried < count; ++tried) {
            const uint16_t candidate = nextIds[source];
            nextIds[source] = (nextIds[source] + 1) % count;
            if (liveIds[source].insert(candidate).second) {
                state.allocatedId = candidate;
                break;
            }
        }
        fatal_if(state.allocatedId < 0,
                 "HetAxiMonitor: synthetic IDs exhausted; increase axi_id_bits");
    }
    for (auto &logged : state.transactions) {
        if (uniquePacketIds)
            logged.txn.axi_id = state.allocatedId;
        logged.txn.txn = trace_writer.NextTxn();
        if (state.write) {
            trace_writer.BeginWrite(curTick(), logged.txn,
                                    logged.strobes.data());
        } else {
            trace_writer.BeginRead(curTick(), logged.txn);
        }
    }
}

void
HetAxiMonitor::emitComplete(const TraceSenderState &state, uint8_t response,
                            Tick completion_tick)
{
    auto &trace_writer = writer(state.source);
    for (const auto &logged : state.transactions) {
        if (state.write)
            trace_writer.CompleteWrite(completion_tick, logged.txn, response);
        else
            trace_writer.CompleteRead(completion_tick, logged.txn, response);
    }
    if (uniquePacketIds) {
        const auto source = static_cast<unsigned>(state.source);
        const auto removed = liveIds[source].erase(state.allocatedId);
        panic_if(removed != 1, "HetAxiMonitor: synthetic ID released twice");
    }
}

void
HetAxiMonitor::queueAtomicCompletion(
    std::unique_ptr<TraceSenderState> state, uint8_t response,
    Tick completion_tick)
{
    const size_t source = static_cast<size_t>(state->source);
    pendingAtomicCompletions[source].emplace(
        AtomicCompletionKey{completion_tick, nextAtomicCompletionSequence++},
        PendingAtomicCompletion{std::move(state), response});
}

void
HetAxiMonitor::flushAtomicCompletions(Source source, Tick through_tick)
{
    auto &pending = pendingAtomicCompletions[static_cast<size_t>(source)];
    while (!pending.empty() && pending.begin()->first.first <= through_tick) {
        auto it = pending.begin();
        const Tick completion_tick = it->first.first;
        auto state = std::move(it->second.state);
        const uint8_t response = it->second.response;
        pending.erase(it);
        emitComplete(*state, response, completion_tick);
    }
}

void
HetAxiMonitor::flushAllAtomicCompletions()
{
    constexpr Tick last_tick = std::numeric_limits<Tick>::max();
    flushAtomicCompletions(Source::Host, last_tick);
    flushAtomicCompletions(Source::Vortex, last_tick);
    flushAtomicCompletions(Source::CoralNpu, last_tick);
}

void
HetAxiMonitor::recvFunctional(PacketPtr pkt)
{
    memSidePort.sendFunctional(pkt);
}

void
HetAxiMonitor::recvFunctionalSnoop(PacketPtr pkt)
{
    cpuSidePort.sendFunctionalSnoop(pkt);
}

Tick
HetAxiMonitor::recvAtomic(PacketPtr pkt)
{
    auto state = makeTraceState(pkt);
    if (state) {
        flushAtomicCompletions(state->source, curTick());
        emitBegin(*state);
    }
    const Tick delay = memSidePort.sendAtomic(pkt);
    if (state) {
        // sendAtomic 返回服务延迟但不会在当前调用栈内推进 curTick()。
        // 响应先按完成时刻排队；后续请求仍用真实 curTick()，不会因为
        // 前一笔已经写入了“未来”响应而造成 trace 时间戳回退。
        const Source source = state->source;
        queueAtomicCompletion(
            std::move(state),
            pkt->isError() ? hettrace::kRespSlvErr : hettrace::kRespOkay,
            curTick() + delay);
        // 零延迟响应可立即落盘；非零延迟响应会在同源下一事件或退出时落盘。
        flushAtomicCompletions(source, curTick());
    }
    return delay;
}

Tick
HetAxiMonitor::recvAtomicSnoop(PacketPtr pkt)
{
    return cpuSidePort.sendAtomicSnoop(pkt);
}

bool
HetAxiMonitor::recvTimingReq(PacketPtr pkt)
{
    assert(pkt->isRequest());
    auto state = makeTraceState(pkt);
    const bool expects_response =
        pkt->needsResponse() && !pkt->cacheResponding();
    TraceSenderState *attached = nullptr;
    if (state && expects_response) {
        attached = state.release();
        pkt->pushSenderState(attached);
    }

    const bool accepted = memSidePort.sendTimingReq(pkt);
    if (!accepted) {
        if (attached) {
            auto *restored = dynamic_cast<TraceSenderState *>(
                pkt->popSenderState());
            panic_if(restored != attached,
                     "HetAxiMonitor sender-state stack changed on retry");
            state.reset(restored);
        }
        return false;
    }

    if (attached) {
        flushAtomicCompletions(attached->source, curTick());
        emitBegin(*attached);
    } else if (state) {
        // Writebacks and a few maintenance packets do not return a gem5
        // response.  AXI still has B/R completion semantics, represented at
        // the acceptance tick because no later observable event exists.
        flushAtomicCompletions(state->source, curTick());
        emitBegin(*state);
        emitComplete(*state, hettrace::kRespOkay, curTick());
    }
    return true;
}

bool
HetAxiMonitor::recvTimingResp(PacketPtr pkt)
{
    assert(pkt->isResponse());
    auto *state = dynamic_cast<TraceSenderState *>(pkt->senderState);
    if (state)
        pkt->senderState = state->predecessor;
    const bool error = pkt->isError();
    const bool accepted = cpuSidePort.sendTimingResp(pkt);
    if (state) {
        if (accepted) {
            flushAtomicCompletions(state->source, curTick());
            emitComplete(*state, error ? hettrace::kRespSlvErr
                                       : hettrace::kRespOkay,
                         curTick());
            delete state;
        } else {
            pkt->senderState = state;
        }
    }
    return accepted;
}

void
HetAxiMonitor::recvTimingSnoopReq(PacketPtr pkt)
{
    cpuSidePort.sendTimingSnoopReq(pkt);
}

bool
HetAxiMonitor::recvTimingSnoopResp(PacketPtr pkt)
{
    return memSidePort.sendTimingSnoopResp(pkt);
}

void
HetAxiMonitor::recvRetrySnoopResp()
{
    cpuSidePort.sendRetrySnoopResp();
}

AddrRangeList
HetAxiMonitor::getAddrRanges() const
{
    return memSidePort.getAddrRanges();
}

bool
HetAxiMonitor::isSnooping() const
{
    return cpuSidePort.isSnooping();
}

void
HetAxiMonitor::recvReqRetry()
{
    cpuSidePort.sendRetryReq();
}

void
HetAxiMonitor::recvRespRetry()
{
    memSidePort.sendRetryResp();
}

void
HetAxiMonitor::recvRangeChange()
{
    cpuSidePort.sendRangeChange();
}

bool
HetAxiMonitor::tryTiming(PacketPtr pkt)
{
    return memSidePort.tryTiming(pkt);
}

void
HetAxiMonitor::closeWriter(const char *source_name,
                           hettrace::TraceWriter &trace_writer)
{
    if (!trace_writer.is_open())
        return;
    const auto stats = trace_writer.stats();
    trace_writer.Close();
    inform("HetAxiMonitor: %s closed: %llu records, %llu transactions, "
           "%llu bytes, %llu filtered, %llu unmapped",
           source_name,
           static_cast<unsigned long long>(stats.emitted),
           static_cast<unsigned long long>(stats.transactions),
           static_cast<unsigned long long>(stats.bytes),
           static_cast<unsigned long long>(stats.filtered),
           static_cast<unsigned long long>(stats.unmapped));
}

void
HetAxiMonitor::closeTrace()
{
    if (!active)
        return;
    flushAllAtomicCompletions();
    active = false;
    closeWriter("host", hostWriter);
    closeWriter("vortex", vortexWriter);
    closeWriter("coralnpu", coralNpuWriter);
}

} // namespace gem5
