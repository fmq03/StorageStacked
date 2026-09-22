// Bridge 主体：介于 UCIe 链路与堆叠存储之间的协议转换/适配层。
//
// 端口刻意与 axi2flit 的 SimpleBurstMemory 完全一致，因此可以直接替换进
// 现有的全链路 tb，白拿那套已验证的 scoreboard：
//
//     UcieLink ⇄ AouTarget ⇄ [本模块] ⇄ mem_sim
//
// AouTarget 负责 AOU 消息解析、credit、写突发组装与 (write,rp,id) 配对，
// 本模块只负责"整笔 AXI 突发 ⇄ mem_sim 逐事务"的转换与地址/掩码/错误的适配。
//
// 将来 Bridge 换 RTL 时，被替换的就是这一层（连同 AouTarget）。
#pragma once

#include "simple_mem_if.h"

#include "mem_backend.h"

#include <cstdint>
#include <deque>
#include <memory>
#include <systemc.h>
#include <unordered_map>
#include <vector>

SC_MODULE(MemSimBackend) {
    sc_in<bool> clk, rst_n;
    sc_fifo_in<SimpleMemRequest> request;
    sc_fifo_out<SimpleMemResponse> response;

    // tb 的终局断言直接引用这两个计数器，语义必须与 SimpleBurstMemory 一致：
    // completed = 已交付到 response FIFO 的响应数。
    std::uint64_t completed = 0, error_responses = 0;

    // 额外诊断计数（SimpleBurstMemory 没有，但排查 Bridge 问题时必需）。
    std::uint64_t submitted_txns = 0, retried_submits = 0;
    std::uint64_t rejected_out_of_range = 0, rejected_lock = 0,
                  rejected_strobe = 0, rejected_contract = 0;
    std::uint64_t read_bytes = 0, write_bytes = 0;

    // 构造签名与 SimpleBurstMemory 对齐，以便直接替换。
    // access/per_beat 是原模块的延迟模型参数；本模块的时序由 mem_sim 的真实
    // DRAM 时序决定，这两个参数仅为兼容签名而保留、不参与行为。
    SC_HAS_PROCESS(MemSimBackend);
    MemSimBackend(sc_module_name name, std::uint64_t base, std::size_t size,
                  sc_time access = sc_time(20, SC_NS),
                  sc_time per_beat = sc_time(2, SC_NS));

    ~MemSimBackend() override;

    bool idle() const;

    // 收尾用：mem_sim 是否已排空 / 生成聚合统计与 flush。
    bool memory_quiescent() const;
    void finish_memory();

private:
    struct Impl;

    std::unique_ptr<Impl> impl_;

    // SC_THREAD：每拍推进 mem_sim、收发事务，全程不阻塞。
    void run();
    // 预校验并接纳一笔突发；预校验失败时合成错误响应而不进入 mem_sim。
    void admit(const SimpleMemRequest& req);
};
