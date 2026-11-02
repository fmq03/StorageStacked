/**
 * 串行突发存储模型。一次处理一个请求，支持字节选通、窄访问和地址范围检查。
 * 响应队列背压会阻止后续服务；不实现引脚时序和独占监视器。
 */
#pragma once
#include "simple_mem_if.h"
#include "axi_contract.h"
#include <stdexcept>

SC_MODULE(SimpleBurstMemory) {
    sc_fifo_in<SimpleMemRequest> request;
    sc_fifo_out<SimpleMemResponse> response;
    uint64_t completed = 0, error_responses = 0;

    SC_HAS_PROCESS(SimpleBurstMemory);
    SimpleBurstMemory(sc_module_name name, uint64_t base, size_t size,
                      sc_time access = sc_time(20, SC_NS),
                      sc_time per_beat = sc_time(2, SC_NS))
        : sc_module(name), base_(base), bytes_(size, 0),
          access_(access), per_beat_(per_beat) {
        if (!size || base > UINT64_MAX - (size - 1))
            throw std::invalid_argument("invalid memory window");
        SC_THREAD(run);
    }

private:
    uint64_t base_;
    std::vector<uint8_t> bytes_;
    sc_time access_, per_beat_;

    // 阻塞读取整笔突发，等待访问延迟后执行；响应 FIFO 的背压延长服务间隔。
    void run() {
        while (true) {
            const auto req = request.read();
            const auto& a = req.address;
            SimpleMemResponse rsp;
            rsp.write = req.write; rsp.rp = req.rp; rsp.id = a.id; rsp.user = a.user;
            if (axi_request_error(a) || (req.write && req.write_beats.size() != req.beats()))
                SC_REPORT_FATAL("SimpleBurstMemory", "invalid burst transaction");
            const unsigned width = 1u << a.size;
            const uint64_t length = uint64_t(width) * req.beats();
            const bool in_range = a.addr >= base_ && a.addr - base_ < bytes_.size() &&
                length <= bytes_.size() - (a.addr - base_);

            // 越界标记为 DECERR，独占请求标记为 SLVERR；非法字节选通也置 SLVERR。
            // 先检查全部写数据，任一错误均不修改内存，避免部分写副作用。
            unsigned status = !in_range ? 3 : (a.lock ? 2 : 0);
            if (req.write) {
                for (unsigned n = 0; n < req.beats(); ++n) {
                    unsigned lane = (a.addr + uint64_t(n) * width) % AXI_DATA_BYTES;
                    for (unsigned j = 0; j < AXI_DATA_BYTES; ++j)
                        if (req.write_beats[n].strobe[j] && (j < lane || j >= lane + width))
                            status = 2;
                }
            }
            wait(access_ + per_beat_ * double(req.beats()));
            if (req.write) {
                rsp.resp = status;
                if (!status) {
                    for (unsigned n = 0; n < req.beats(); ++n) {
                        uint64_t addr = a.addr + uint64_t(n) * width;
                        unsigned lane = addr % AXI_DATA_BYTES;
                        for (unsigned j = 0; j < width; ++j)
                            if (req.write_beats[n].strobe[lane + j])
                                bytes_[addr - base_ + j] = req.write_beats[n].data[lane + j];
                    }
                }
            } else {
                // 窄读取按地址映射到总线字节通道，其余数据字节保持零。
                rsp.read_beats.resize(req.beats());
                for (unsigned n = 0; n < req.beats(); ++n) {
                    auto& beat = rsp.read_beats[n];
                    beat.resp = status; beat.user = a.user;
                    if (!status) {
                        uint64_t addr = a.addr + uint64_t(n) * width;
                        unsigned lane = addr % AXI_DATA_BYTES;
                        for (unsigned j = 0; j < width; ++j)
                            beat.data[lane + j] = bytes_[addr - base_ + j];
                    }
                }
            }
            ++completed;
            if (status) ++error_responses;
            response.write(rsp);
        }
    }
};
