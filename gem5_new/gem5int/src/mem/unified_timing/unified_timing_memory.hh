#ifndef __MEM_UNIFIED_TIMING_MEMORY_HH__
#define __MEM_UNIFIED_TIMING_MEMORY_HH__

#include <array>
#include <list>
#include <memory>
#include <unordered_map>

#include "mem/port.hh"
#include "params/UnifiedTimingMemory.hh"
#include "sim/clocked_object.hh"

namespace gem5
{
namespace memory
{

// A sparse functional responder shared by the explicit XPU windows.  Its
// latency and bandwidth keep gem5's timing protocol executable; they are not
// the project's controller/DRAM performance truth.  That result comes from
// projecting the unified HETTrace into the external mem_sim/hbm_sim flow.
class UnifiedTimingMemory : public ClockedObject
{
  private:
    static constexpr Addr PageBytes = 4096;
    using Page = std::array<uint8_t, PageBytes>;

    class MemoryPort : public ResponsePort
    {
      private:
        UnifiedTimingMemory &memory;

      public:
        MemoryPort(const std::string &name, UnifiedTimingMemory &memory);

      protected:
        Tick recvAtomic(PacketPtr pkt) override;
        void recvFunctional(PacketPtr pkt) override;
        bool recvTimingReq(PacketPtr pkt) override;
        void recvRespRetry() override;
        AddrRangeList getAddrRanges() const override;
    };

    struct DeferredPacket
    {
        Tick tick;
        PacketPtr pkt;
    };

    MemoryPort port;
    const AddrRangeList ranges;
    const Tick latency;
    const double bandwidth;
    std::unordered_map<Addr, std::unique_ptr<Page>> pages;
    std::list<DeferredPacket> packetQueue;
    bool isBusy = false;
    bool retryReq = false;
    bool retryResp = false;
    EventFunctionWrapper releaseEvent;
    EventFunctionWrapper dequeueEvent;
    std::unique_ptr<Packet> pendingDelete;

    bool contains(Addr addr, unsigned size) const;
    void readBytes(Addr addr, uint8_t *dst, unsigned size) const;
    void writeBytes(Addr addr, const uint8_t *src, unsigned size,
                    const std::vector<bool> *byteEnable = nullptr);
    void access(PacketPtr pkt);
    void functionalAccess(PacketPtr pkt);
    Tick recvAtomic(PacketPtr pkt);
    void recvFunctional(PacketPtr pkt);
    bool recvTimingReq(PacketPtr pkt);
    void recvRespRetry();
    void release();
    void dequeue();

  public:
    using Params = UnifiedTimingMemoryParams;
    explicit UnifiedTimingMemory(const Params &p);

    Port &getPort(const std::string &if_name,
                  PortID idx = InvalidPortID) override;
    void init() override;
    DrainState drain() override;
};

} // namespace memory
} // namespace gem5

#endif // __MEM_UNIFIED_TIMING_MEMORY_HH__
