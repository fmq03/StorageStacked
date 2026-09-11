#include "packet_tester.hh"
#include "sim/sim_exit.hh"
#include "base/logging.hh"
#include <algorithm>

namespace gem5 {
AxiPacketTester::AxiPacketTester(const AxiPacketTesterParams& p)
    : SimObject(p), port(name() + ".port", *this), base(p.base),
      requestor(p.system->getRequestorId(this)), hold(p.response_hold), directory(p.trace_dir),
      issueEvent([this] { pump(); }, name() + ".issue"),
      retryEvent([this] { retryResponse(); }, name() + ".respRetry"), golden(8192, 0),
      lifecycle(directory + "/packet_lifecycle.csv") {
    fatal_if(!lifecycle, "cannot open packet lifecycle");
    lifecycle << "serial,command,address,bytes,first_attempt_tick,accepted_tick,response_tick,error\n";
}
Port& AxiPacketTester::getPort(const std::string& n, PortID idx) {
    if (n == "port") return port;
    return SimObject::getPort(n, idx);
}
void AxiPacketTester::startup() { nextStage(); schedule(issueEvent, curTick() + 1000000); }
void AxiPacketTester::add(bool wr, unsigned off, unsigned size, unsigned seed, bool masked, bool error) {
    auto req = std::make_shared<Request>(base + off, size, Request::UNCACHEABLE, requestor);
    req->setStreamId(7); req->setSubstreamId(serial + 100);
    std::vector<bool> enables(size, true);
    if (masked) for (unsigned i = 0; i < size; ++i) enables[i] = i % 3 != 1;
    auto* pkt = new Packet(req, wr ? MemCmd::WriteReq : MemCmd::ReadReq);
    pkt->allocate();
    std::fill(pkt->getPtr<uint8_t>(), pkt->getPtr<uint8_t>() + size, 0xa5);
    std::vector<uint8_t> expected(size, 0xa5);
    if (wr) {
        for (unsigned i = 0; i < size; ++i) {
            pkt->getPtr<uint8_t>()[i] = (seed + i * 37) & 255;
            if (!error && enables[i]) golden.at(off + i) = pkt->getPtr<uint8_t>()[i];
        }
    } else if (!error) {
        for (unsigned i = 0; i < size; ++i) if (enables[i]) expected[i] = golden.at(off + i);
    }
    // Exercise non-zero timing annotations through the existing gem5 bridge.
    pkt->headerDelay = 250000;
    pkt->payloadDelay = wr ? 500000 : 0;
    req->setByteEnable(enables);
    ops.emplace(pkt, Op{pkt, expected, error, wr, serial++});
    queue.push_back(pkt);
}
void AxiPacketTester::nextStage() {
    ++stage;
    if (stage == 1) {
        add(true, 0, 64, 11);
        add(true, 128, 2080, 29); // aligned full-width bursts, high data lanes
        add(true, 3003, 19, 71); // unaligned, legal narrow bursts
        add(true, 4088, 32, 91); // crosses 4KiB
        add(true, 5001, 257, 13); // byte-sized: 256-beat limit plus one beat
        add(true, 6016, 96, 123);
    } else if (stage == 2) {
        add(true, 3, 17, 217, true);
        add(true, 4089, 23, 201, true);
        add(true, 6016, 96, 198, true); // masked full-width writes
        add(true, 12288, 8, 1, false, true); // DECERR in bridge routing range
    } else if (stage == 3) {
        add(false, 0, 64, 0);
        add(false, 128, 2080, 0);
        add(false, 3003, 19, 0);
        add(false, 4088, 32, 0);
        add(false, 5001, 257, 0);
        add(false, 6016, 96, 0, true);
        add(false, 12288, 8, 0, false, true);
    } else {
        std::ofstream f(directory + "/tester_summary.json");
        f << "{\"passed\":true,\"requests\":" << serial << ",\"responses\":" << done
          << ",\"request_retries\":" << reqRetries << ",\"response_retries\":"
          << respRetries << ",\"finish_tick\":" << curTick() << "}\n";
        f.close(); lifecycle.flush();
        exitSimLoop("AXI packet/data/retry tests passed", 0);
    }
}
void AxiPacketTester::pump() {
    if (blocked) return;
    while (!queue.empty()) {
        auto* pkt = queue.front(); auto& op = ops.at(pkt);
        if (!op.attempt) op.attempt = curTick();
        if (!port.sendTimingReq(pkt)) { blocked = true; return; }
        op.accepted = curTick(); ++inflight; queue.pop_front();
    }
}
bool AxiPacketTester::response(PacketPtr pkt) {
    if (hold && !delayed.count(pkt)) {
        delayed.insert(pkt); waitingResponse = pkt; ++respRetries;
        schedule(retryEvent, curTick() + hold);
        return false;
    }
    auto it = ops.find(pkt);
    fatal_if(it == ops.end(), "unknown/duplicate response");
    auto& op = it->second;
    fatal_if(pkt->isError() != op.error, "incorrect AXI error propagation");
    if (!op.error && pkt->isRead()) {
        for (unsigned i = 0; i < pkt->getSize(); ++i)
            fatal_if(pkt->getConstPtr<uint8_t>()[i] != op.expected[i],
                     "read mismatch at %#x byte %u: got %u expected %u", pkt->getAddr(), i,
                     pkt->getConstPtr<uint8_t>()[i], op.expected[i]);
    }
    lifecycle << op.serial << ',' << (op.write ? 'W' : 'R') << ',' << pkt->getAddr()
              << ',' << pkt->getSize() << ',' << op.attempt << ',' << op.accepted << ','
              << curTick() << ',' << pkt->isError() << '\n';
    lifecycle.flush();
    delayed.erase(pkt); ops.erase(it); delete pkt;
    ++done; --inflight;
    if (!inflight && queue.empty()) {
        nextStage();
        if (!queue.empty()) schedule(issueEvent, curTick() + 1);
    }
    return true;
}
void AxiPacketTester::retryResponse() {
    panic_if(!waitingResponse, "response retry without blocked response");
    waitingResponse = nullptr;
    port.sendRetryResp();
}
}
