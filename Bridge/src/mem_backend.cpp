// MemBackend 的实现。本 TU 是 Bridge 里唯一包含 mem_sim 的地方，以 C++20 编译
// （与 mem_sim 自身一致）。public 头 mem_backend.h 保持 C++17 干净，
// 供 SystemC 侧（必须是 C++17）使用。
#include "mem_backend.h"

#include "hbm_sim/config/model.hpp"
#include "hbm_sim/core/addr_map.hpp"
#include "hbm_sim/core/request.hpp"
#include "hbm_sim/core/response.hpp"
#include "hbm_sim/core/system.hpp"
#include "hbm_sim/dram/spec.hpp"

#include <deque>
#include <exception>
#include <stdexcept>
#include <string>
#include <utility>

namespace bridge {
namespace {

using hbm_sim::AddressMapper;
using hbm_sim::DecodedAddress;
using hbm_sim::HostResponse;
using hbm_sim::MemorySystem;
using hbm_sim::MemorySystemOptions;
using hbm_sim::Request;
using hbm_sim::RequestType;
using hbm_sim::ResponseStatus;

std::string to_string(int value) { return std::to_string(value); }

}  // namespace

struct MemBackend::Impl {
    explicit Impl(const Options& opt) : options(opt) {
        // build_model 内部会 finalize_spec，所以拿到的 spec 可直接用于
        // AddressMapper 与 MemorySystem。
        spec = hbm_sim::config::build_model(
            options.standard,
            {{"channels", to_string(options.channels)},
             {"dram_transaction_bytes", to_string(options.transaction_bytes)}});

        const std::uint64_t full_capacity = spec.addressable_capacity_bytes();
        window = options.window_bytes != 0 ? options.window_bytes : full_capacity;
        if (window == 0 || window > full_capacity) {
            throw std::invalid_argument(
                "MemBackend: window_bytes 超出 spec 的可寻址容量");
        }

        MemorySystemOptions sys;
        sys.stack_count = 1;
        // 信任 AddressMapper 填好的 decoded.channel；channels=1 时恒为 0。
        sys.channel_mapper = hbm_sim::ChannelMapperKind::Decoded;
        sys.stack_ingress_buffer_size = 64;
        sys.stack_dispatch_width = 4;
        sys.response_delivery_mode = hbm_sim::ResponseDeliveryMode::HostOnly;
        sys.host_response_queue_capacity = options.response_queue_capacity;
        sys.transaction_response_queue_capacity = 0;
        // 默认 true 会把每条 DRAM 命令连同 payload 一直留着，长跑必然 OOM。
        sys.controller.retain_command_trace = options.retain_command_trace;

        memory = std::make_unique<MemorySystem>(spec, sys);
        // 必须与传给 MemorySystem 的是同一份 spec，否则 decoded 的坐标与
        // 控制器内部的分 bank 视图不一致。
        mapper = std::make_unique<AddressMapper>(spec);
    }

    Options options;
    hbm_sim::DramSpec spec;
    std::uint64_t window = 0;
    std::unique_ptr<MemorySystem> memory;
    std::unique_ptr<AddressMapper> mapper;
    std::deque<MemResp> ready;
    std::string error;
    std::uint64_t next_request_id = 1;
    bool finished = false;

    // 一个 tick 的实际时长 = tCK / tick_multiplier。HBM 的 edge pairing 下
    // tick_multiplier 为 2，因此 HBM4 的 tCK 500 ps 对应 250 ps 一 tick。
    std::uint64_t tick_ps() const {
        const double ps = spec.timing.tCK_ps /
                          static_cast<double>(std::max(1, spec.tick_multiplier));
        return static_cast<std::uint64_t>(ps + 0.5);
    }

    void drain() {
        while (memory->has_response()) {
            HostResponse r = memory->pop_response();
            MemResp out;
            out.host_request_id = r.host_request_id;
            out.write = (r.type == RequestType::Write);
            out.data = std::move(r.data);
            out.status = static_cast<int>(r.status);
            ready.push_back(std::move(out));
        }
    }

    void fail(const std::string& what) {
        error = what;
    }
};

MemBackend::MemBackend() : impl_(new Impl(Options{})) {}

MemBackend::MemBackend(const Options& options) : impl_(new Impl(options)) {}

MemBackend::~MemBackend() = default;

std::uint64_t MemBackend::window_bytes() const { return impl_->window; }

std::uint64_t MemBackend::tick_ps() const { return impl_->tick_ps(); }

std::uint32_t MemBackend::transaction_bytes() const {
    return static_cast<std::uint32_t>(impl_->spec.transaction_bytes());
}

std::uint64_t MemBackend::clock() const { return impl_->memory->clock(); }

void MemBackend::step(std::uint32_t ticks) {
    for (std::uint32_t i = 0; i < ticks; ++i) {
        impl_->memory->step();
    }
    impl_->drain();
}

int MemBackend::submit(const MemTxn& txn) {
    Impl& s = *impl_;
    if (s.finished) {
        s.fail("submit after finish");
        return -1;
    }

    const std::uint32_t bytes =
        txn.bytes != 0 ? txn.bytes : static_cast<std::uint32_t>(txn.payload.size());
    if (bytes == 0) {
        s.fail("MemTxn::bytes 为 0 且没有 payload 可推导");
        return -1;
    }
    // 一次只提交一笔 mem_sim transaction。多事务突发由上层切分后逐笔提交：
    // 若允许一笔 MemTxn 展开成多个 Request，中途遇到反压就会留下"部分已提交"
    // 的状态，而 submit 的契约是"返回 1 时调用方原样重试整笔"——重试会重复
    // 提交前面的子事务。
    if (bytes != transaction_bytes()) {
        s.fail("MemTxn::bytes 必须恰好等于 transaction_bytes()");
        return -1;
    }

    Request req;
    req.id = txn.host_request_id;
    req.host_request_id = txn.host_request_id;
    req.transaction_index = 0;
    req.transaction_count = 1;
    req.type = txn.write ? RequestType::Write : RequestType::Read;
    req.address = txn.address;
    req.system_address = txn.address;
    req.has_system_address = true;
    req.inject_cycle = s.memory->clock();
    req.arrival = s.memory->clock();
    req.transfer_bytes = bytes;

    // 这一行是本后端最关键的一处：MemorySystem::localize_request 在
    // stack_count == 1 时直接 req.storage_decoded = req.decoded，不会按地址
    // 重新解码。留空（全 0）会让所有 cache line 落到同一个 (bank,row,column)，
    // 行缓冲返回上一条线的数据——静默读错，不抛异常。
    try {
        req.decoded = s.mapper->decode(txn.address);
    } catch (const std::exception& e) {
        s.fail(std::string("decode 失败: ") + e.what());
        return -1;
    }

    if (txn.write) {
        req.payload = txn.payload;
        if (req.payload.size() != bytes) {
            s.fail("写请求的 payload 长度与 bytes 不一致");
            return -1;
        }
        req.has_payload = true;
        if (txn.has_byte_mask) {
            if (txn.byte_mask.size() != bytes) {
                s.fail("byte_mask 长度与 bytes 不一致");
                return -1;
            }
            req.byte_mask = txn.byte_mask;
            req.has_byte_mask = true;
        }
    }
    // 读请求不设 byte_mask：控制器会把该字段复用为 initialized_mask。

    try {
        if (!s.memory->try_submit(std::move(req))) {
            return 1;  // 反压：未接受且无副作用，调用方必须原样重试
        }
    } catch (const std::exception& e) {
        // 越界在这里表现为 out_of_range。Bridge 应当在提交前就挡住，
        // 走到这里说明上层预校验有缺口。
        s.fail(std::string("try_submit 抛异常: ") + e.what());
        return -1;
    }
    return 0;
}

bool MemBackend::poll(MemResp& out) {
    if (impl_->ready.empty()) return false;
    out = std::move(impl_->ready.front());
    impl_->ready.pop_front();
    return true;
}

bool MemBackend::quiescent() const {
    if (!impl_->ready.empty()) return false;
    return impl_->memory->quiescent();
}

void MemBackend::finish(std::uint64_t unsubmitted) {
    if (impl_->finished) return;
    impl_->memory->finish(unsubmitted);
    impl_->drain();
    impl_->finished = true;
}

const std::string& MemBackend::last_error() const { return impl_->error; }

}  // namespace bridge
