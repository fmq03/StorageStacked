// Transparent gem5 packet monitor for the AXI4-native HETTrace v2 format.

#ifndef __HETTRACE_HET_AXI_MONITOR_HH__
#define __HETTRACE_HET_AXI_MONITOR_HH__

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "hettrace/writer.h"
#include "mem/packet.hh"
#include "mem/port.hh"
#include "params/HetAxiMonitor.hh"
#include "sim/sim_object.hh"

namespace gem5
{

class System;

/**
 * Observe the single memory-side interconnect boundary and write one
 * HETTrace file per source.  The object is deliberately timing-transparent:
 * retries, snoops, ranges, functional accesses and responses are forwarded
 * unchanged.  Only successfully accepted timing requests are recorded.
 */
class HetAxiMonitor : public SimObject
{
  public:
    using Params = HetAxiMonitorParams;

    explicit HetAxiMonitor(const Params &params);
    ~HetAxiMonitor() override;

    Port &getPort(const std::string &if_name,
                  PortID idx=InvalidPortID) override;
    void init() override;
    void startup() override;

  private:
    enum class Source : uint8_t
    {
        Host,
        Vortex,
        CoralNpu,
    };

    struct LoggedTxn
    {
        hettrace::AxiTxn txn;
        std::vector<uint64_t> strobes;
    };

    class TraceSenderState : public Packet::SenderState
    {
      public:
        Source source = Source::Host;
        bool write = false;
        int allocatedId = -1;
        std::vector<LoggedTxn> transactions;
    };

    struct PendingAtomicCompletion
    {
        std::unique_ptr<TraceSenderState> state;
        uint8_t response = hettrace::kRespOkay;
    };

    using AtomicCompletionKey = std::pair<Tick, uint64_t>;
    using AtomicCompletionQueue =
        std::map<AtomicCompletionKey, PendingAtomicCompletion>;

    class MonitorRequestPort : public RequestPort
    {
      public:
        MonitorRequestPort(const std::string &name, HetAxiMonitor &owner)
            : RequestPort(name), owner(owner)
        {}

      protected:
        void recvFunctionalSnoop(PacketPtr pkt) override
        { owner.recvFunctionalSnoop(pkt); }
        Tick recvAtomicSnoop(PacketPtr pkt) override
        { return owner.recvAtomicSnoop(pkt); }
        bool recvTimingResp(PacketPtr pkt) override
        { return owner.recvTimingResp(pkt); }
        void recvTimingSnoopReq(PacketPtr pkt) override
        { owner.recvTimingSnoopReq(pkt); }
        void recvRangeChange() override
        { owner.recvRangeChange(); }
        bool isSnooping() const override
        { return owner.isSnooping(); }
        void recvReqRetry() override
        { owner.recvReqRetry(); }
        void recvRetrySnoopResp() override
        { owner.recvRetrySnoopResp(); }

      private:
        HetAxiMonitor &owner;
    };

    class MonitorResponsePort : public ResponsePort
    {
      public:
        MonitorResponsePort(const std::string &name, HetAxiMonitor &owner)
            : ResponsePort(name), owner(owner)
        {}

      protected:
        void recvFunctional(PacketPtr pkt) override
        { owner.recvFunctional(pkt); }
        Tick recvAtomic(PacketPtr pkt) override
        { return owner.recvAtomic(pkt); }
        bool recvTimingReq(PacketPtr pkt) override
        { return owner.recvTimingReq(pkt); }
        bool recvTimingSnoopResp(PacketPtr pkt) override
        { return owner.recvTimingSnoopResp(pkt); }
        AddrRangeList getAddrRanges() const override
        { return owner.getAddrRanges(); }
        void recvRespRetry() override
        { owner.recvRespRetry(); }
        bool tryTiming(PacketPtr pkt) override
        { return owner.tryTiming(pkt); }

      private:
        HetAxiMonitor &owner;
    };

    void recvFunctional(PacketPtr pkt);
    void recvFunctionalSnoop(PacketPtr pkt);
    Tick recvAtomic(PacketPtr pkt);
    Tick recvAtomicSnoop(PacketPtr pkt);
    bool recvTimingReq(PacketPtr pkt);
    bool recvTimingResp(PacketPtr pkt);
    void recvTimingSnoopReq(PacketPtr pkt);
    bool recvTimingSnoopResp(PacketPtr pkt);
    void recvRetrySnoopResp();
    AddrRangeList getAddrRanges() const;
    bool isSnooping() const;
    void recvReqRetry();
    void recvRespRetry();
    void recvRangeChange();
    bool tryTiming(PacketPtr pkt);

    Source classify(RequestorID requestor_id) const;
    bool containsPattern(const std::string &name,
                         const std::vector<std::string> &patterns) const;
    hettrace::TraceWriter &writer(Source source);
    std::unique_ptr<TraceSenderState> makeTraceState(PacketPtr pkt) const;
    void emitBegin(TraceSenderState &state);
    void emitComplete(const TraceSenderState &state, uint8_t response,
                      Tick completion_tick);
    void queueAtomicCompletion(std::unique_ptr<TraceSenderState> state,
                               uint8_t response, Tick completion_tick);
    void flushAtomicCompletions(Source source, Tick through_tick);
    void flushAllAtomicCompletions();
    void closeTrace();
    void closeWriter(const char *name, hettrace::TraceWriter &writer);

    MonitorRequestPort memSidePort;
    MonitorResponsePort cpuSidePort;
    System *const system;
    const bool enable;
    const bool traceHost;
    const bool traceVortex;
    const bool traceCoralNpu;
    const bool traceInstFetch;
    const unsigned axiDataBytes;
    const unsigned axiIdBits;
    const bool uniquePacketIds;
    std::array<std::set<uint16_t>, 3> liveIds;
    std::array<uint32_t, 3> nextIds{};
    const std::vector<std::string> vortexPatterns;
    const std::vector<std::string> coralNpuPatterns;
    bool active = false;

    // sendAtomic() returns a delay without advancing curTick() in the current
    // call stack.  Keep its synthetic R/B events pending so later AW/AR/W
    // events at the real current tick can be written before those responses.
    // The sequence component makes equal-tick completion order deterministic.
    std::array<AtomicCompletionQueue, 3> pendingAtomicCompletions;
    uint64_t nextAtomicCompletionSequence = 0;

    hettrace::TraceWriter hostWriter;
    hettrace::TraceWriter vortexWriter;
    hettrace::TraceWriter coralNpuWriter;
};

} // namespace gem5

#endif // __HETTRACE_HET_AXI_MONITOR_HH__
