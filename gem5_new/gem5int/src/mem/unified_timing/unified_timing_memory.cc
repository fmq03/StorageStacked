#include "mem/unified_timing/unified_timing_memory.hh"

#include <algorithm>
#include <cstring>
#include <vector>

#include "base/logging.hh"
#include "debug/Drain.hh"
#include "mem/packet.hh"

namespace gem5
{
namespace memory
{

UnifiedTimingMemory::UnifiedTimingMemory(const Params &p)
  : ClockedObject(p),
    port(name() + ".port", *this),
    ranges(p.ranges.begin(), p.ranges.end()),
    latency(p.latency),
    bandwidth(p.bandwidth),
    releaseEvent([this] { release(); }, name() + ".release"),
    dequeueEvent([this] { dequeue(); }, name() + ".dequeue")
{
    fatal_if(ranges.empty(), "UnifiedTimingMemory requires at least one range");
    for (auto a = ranges.begin(); a != ranges.end(); ++a) {
        for (auto b = std::next(a); b != ranges.end(); ++b) {
            fatal_if(a->intersects(*b),
                     "UnifiedTimingMemory ranges overlap: %s and %s",
                     a->to_string(), b->to_string());
        }
    }
}

bool
UnifiedTimingMemory::contains(Addr addr, unsigned size) const
{
    if (size == 0)
        return false;
    const AddrRange request = RangeSize(addr, size);
    return std::any_of(ranges.begin(), ranges.end(),
        [&request](const AddrRange &range) { return request.isSubset(range); });
}

void
UnifiedTimingMemory::readBytes(Addr addr, uint8_t *dst, unsigned size) const
{
    for (unsigned i = 0; i < size; ++i) {
        const Addr at = addr + i;
        const auto page = pages.find(at / PageBytes);
        dst[i] = page == pages.end() ? 0 : (*page->second)[at % PageBytes];
    }
}

void
UnifiedTimingMemory::writeBytes(Addr addr, const uint8_t *src, unsigned size,
                                const std::vector<bool> *byteEnable)
{
    for (unsigned i = 0; i < size; ++i) {
        if (byteEnable != nullptr && !byteEnable->at(i))
            continue;
        const Addr at = addr + i;
        auto &page = pages[at / PageBytes];
        if (!page)
            page = std::make_unique<Page>();
        (*page)[at % PageBytes] = src[i];
    }
}

void
UnifiedTimingMemory::access(PacketPtr pkt)
{
    if (pkt->cacheResponding())
        return;
    if (pkt->cmd == MemCmd::CleanEvict || pkt->cmd == MemCmd::WritebackClean)
        return;

    panic_if(!contains(pkt->getAddr(), pkt->getSize()),
             "UnifiedTimingMemory access outside ranges: %s", pkt->print());

    std::vector<uint8_t> data(pkt->getSize());
    if (pkt->cmd == MemCmd::SwapReq) {
        readBytes(pkt->getAddr(), data.data(), data.size());
        if (pkt->isAtomicOp()) {
            pkt->setData(data.data());
            (*(pkt->getAtomicOp()))(data.data());
            writeBytes(pkt->getAddr(), data.data(), data.size());
        } else {
            std::vector<uint8_t> replacement(pkt->getSize());
            pkt->writeData(replacement.data());
            pkt->setData(data.data());
            bool replace = true;
            if (pkt->req->isCondSwap()) {
                uint64_t condition = pkt->req->getExtraData();
                replace = std::memcmp(&condition, data.data(),
                                      pkt->getSize()) == 0;
            }
            if (replace)
                writeBytes(pkt->getAddr(), replacement.data(),
                           replacement.size());
        }
    } else if (pkt->isRead()) {
        readBytes(pkt->getAddr(), data.data(), data.size());
        pkt->setData(data.data());
    } else if (pkt->isWrite()) {
        const auto &enable = pkt->req->getByteEnable();
        writeBytes(pkt->getAddr(), pkt->getConstPtr<uint8_t>(), pkt->getSize(),
                   pkt->isMaskedWrite() ? &enable : nullptr);
    } else if (!(pkt->isInvalidate() || pkt->isClean())) {
        panic("UnifiedTimingMemory unexpected packet: %s", pkt->print());
    }

    if (pkt->needsResponse())
        pkt->makeResponse();
}

void
UnifiedTimingMemory::functionalAccess(PacketPtr pkt)
{
    panic_if(!contains(pkt->getAddr(), pkt->getSize()),
             "UnifiedTimingMemory functional access outside ranges: %s",
             pkt->print());
    std::vector<uint8_t> data(pkt->getSize());
    if (pkt->isRead()) {
        readBytes(pkt->getAddr(), data.data(), data.size());
        pkt->setData(data.data());
        pkt->makeResponse();
    } else if (pkt->isWrite()) {
        const auto &enable = pkt->req->getByteEnable();
        writeBytes(pkt->getAddr(), pkt->getConstPtr<uint8_t>(), pkt->getSize(),
                   pkt->isMaskedWrite() ? &enable : nullptr);
        pkt->makeResponse();
    } else {
        panic("UnifiedTimingMemory unsupported functional command %s",
              pkt->cmdString());
    }
}

Tick
UnifiedTimingMemory::recvAtomic(PacketPtr pkt)
{
    access(pkt);
    return latency;
}

void
UnifiedTimingMemory::recvFunctional(PacketPtr pkt)
{
    pkt->pushLabel(name());
    functionalAccess(pkt);
    for (auto &queued : packetQueue) {
        if (pkt->trySatisfyFunctional(queued.pkt))
            break;
    }
    pkt->popLabel();
}

bool
UnifiedTimingMemory::recvTimingReq(PacketPtr pkt)
{
    panic_if(!(pkt->isRead() || pkt->isWrite()),
             "UnifiedTimingMemory expects read/write, got %s", pkt->print());
    if (retryReq)
        return false;
    if (isBusy) {
        retryReq = true;
        return false;
    }

    const Tick receiveDelay = pkt->headerDelay + pkt->payloadDelay;
    pkt->headerDelay = pkt->payloadDelay = 0;
    const Tick duration = static_cast<Tick>(pkt->getSize() * bandwidth);
    if (duration != 0) {
        isBusy = true;
        schedule(releaseEvent, curTick() + duration);
    }

    const bool needsResponse = pkt->needsResponse();
    access(pkt);
    if (needsResponse) {
        const Tick ready = curTick() + receiveDelay + latency;
        auto position = packetQueue.end();
        while (position != packetQueue.begin()) {
            auto previous = std::prev(position);
            if (previous->tick <= ready || previous->pkt->matchAddr(pkt))
                break;
            position = previous;
        }
        packetQueue.insert(position, DeferredPacket{ready, pkt});
        if (!retryResp && !dequeueEvent.scheduled())
            schedule(dequeueEvent, packetQueue.front().tick);
    } else {
        pendingDelete.reset(pkt);
    }
    return true;
}

void
UnifiedTimingMemory::release()
{
    isBusy = false;
    if (retryReq) {
        retryReq = false;
        port.sendRetryReq();
    }
}

void
UnifiedTimingMemory::dequeue()
{
    panic_if(packetQueue.empty(), "UnifiedTimingMemory empty dequeue");
    auto &front = packetQueue.front();
    retryResp = !port.sendTimingResp(front.pkt);
    if (retryResp)
        return;

    packetQueue.pop_front();
    if (!packetQueue.empty()) {
        reschedule(dequeueEvent,
                   std::max(packetQueue.front().tick, curTick()), true);
    } else if (drainState() == DrainState::Draining) {
        DPRINTF(Drain, "UnifiedTimingMemory drain complete\n");
        signalDrainDone();
    }
}

void
UnifiedTimingMemory::recvRespRetry()
{
    panic_if(!retryResp, "UnifiedTimingMemory unexpected response retry");
    retryResp = false;
    dequeue();
}

Port &
UnifiedTimingMemory::getPort(const std::string &if_name, PortID idx)
{
    if (if_name == "port")
        return port;
    return ClockedObject::getPort(if_name, idx);
}

void
UnifiedTimingMemory::init()
{
    ClockedObject::init();
    if (port.isConnected())
        port.sendRangeChange();
}

DrainState
UnifiedTimingMemory::drain()
{
    return packetQueue.empty() ? DrainState::Drained : DrainState::Draining;
}

UnifiedTimingMemory::MemoryPort::MemoryPort(
    const std::string &name, UnifiedTimingMemory &memory)
  : ResponsePort(name), memory(memory)
{}

Tick
UnifiedTimingMemory::MemoryPort::recvAtomic(PacketPtr pkt)
{
    return memory.recvAtomic(pkt);
}

void
UnifiedTimingMemory::MemoryPort::recvFunctional(PacketPtr pkt)
{
    memory.recvFunctional(pkt);
}

bool
UnifiedTimingMemory::MemoryPort::recvTimingReq(PacketPtr pkt)
{
    return memory.recvTimingReq(pkt);
}

void
UnifiedTimingMemory::MemoryPort::recvRespRetry()
{
    memory.recvRespRetry();
}

AddrRangeList
UnifiedTimingMemory::MemoryPort::getAddrRanges() const
{
    return memory.ranges;
}

} // namespace memory
} // namespace gem5
