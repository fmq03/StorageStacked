/**
 * 根据消息起点和首字节重组消息。每个接收方向单独保存续传状态，完整检查后提交结果。
 */
#pragma once

#include "aou_types.h"
#include <stdexcept>
#include <vector>

class AouStreamDecoder {
public:
    struct Result {
        std::vector<AouMessage> messages;
        unsigned used_granules = 0; // 接收侧从线内容计算，仅用于统计
    };

    void reset() { carry_ = AouMessage{}; have_ = 0; }
    bool carry_active() const { return have_ != 0; }

    Result consume(const AouFlit& flit) {
        AouStreamDecoder candidate = *this;
        Result result = candidate.parse(flit);
        *this = candidate;
        return result;
    }

private:
    AouMessage carry_{};
    int have_ = 0;

    Result parse(const AouFlit& f) {
        if (f.msg_start >> GRANULE_COUNT)
            throw std::invalid_argument("MsgStart 超出 48 bit");
        Result r;
        int g = 0;
        if (have_ != 0) {
            const int take = std::min(carry_.granules - have_, GRANULE_COUNT);
            if (f.msg_start & ((1ULL << take) - 1))
                throw std::invalid_argument("MsgStart 与 G0 续传区重叠");
            std::copy_n(f.payload, take * GRANULE_BYTES,
                        carry_.data + have_ * GRANULE_BYTES);
            have_ += take;
            g += take;
            r.used_granules += take;
            if (have_ == carry_.granules) {
                r.messages.push_back(carry_);
                have_ = 0;
            }
        }
        while (g < GRANULE_COUNT) {
            if (((f.msg_start >> g) & 1ULL) == 0) { ++g; continue; }
            const uint8_t b0 = f.payload[g * GRANULE_BYTES];
            const int total = message_granules_from_header(b0);
            if (total <= 0 || total * GRANULE_BYTES > MSG_MAX_BYTES)
                throw std::invalid_argument("非法消息类型、MiscOp 或 DLENGTH");
            const int take = std::min(total, GRANULE_COUNT - g);
            const uint64_t interior = ((1ULL << take) - 1) & ~1ULL;
            if ((f.msg_start >> g) & interior)
                throw std::invalid_argument("MsgStart 落在前一条消息内部");
            AouMessage msg;
            msg.type = msg_type_of(b0);
            msg.rp = msg_rp_of(b0);
            msg.granules = total;
            msg.byte_len = total * GRANULE_BYTES;
            std::copy_n(f.payload + g * GRANULE_BYTES, take * GRANULE_BYTES, msg.data);
            r.used_granules += take;
            if (take == total) r.messages.push_back(msg);
            else { carry_ = msg; have_ = take; }
            g += take;
        }
        return r;
    }
};
