/**
 * 固定250字节协议内容的编解码。只传输消息头和载荷，不传输本地统计字段。
 */
#pragma once

#include "aou_types.h"
#include <stdexcept>

using AouWireFlit = std::array<uint8_t, PROTO_HEADER_BYTES + PAYLOAD_BYTES>;
static_assert(sizeof(AouWireFlit) == 250, "AoU PLP 必须恰好 250B");

inline AouWireFlit serialize_aou(const AouFlit& f) {
    if (f.fdid > 3 || (f.msg_start >> GRANULE_COUNT) != 0)
        throw std::invalid_argument("AoU FDId/MsgStart 超出线上字段宽度");
    AouWireFlit b{};
    b[0] = f.fdid | ((f.msg_start & 0xFULL) << 4);
    b[1] = (f.msg_start >> 4) & 0xFF;
    b[2] = ((f.msg_start >> 12) & 0xF) << 4;
    b[3] = (f.msg_start >> 16) & 0xFF;
    b[4] = f.msg_credit & 0xFF;
    b[5] = f.msg_credit >> 8;
    b[6] = ((f.msg_start >> 24) & 0xF) << 4;
    b[7] = (f.msg_start >> 28) & 0xFF;
    b[8] = ((f.msg_start >> 36) & 0xF) << 4;
    b[9] = (f.msg_start >> 40) & 0xFF;
    std::copy_n(f.payload, PAYLOAD_BYTES, b.begin() + PROTO_HEADER_BYTES);
    return b;
}

inline AouFlit deserialize_aou(const AouWireFlit& b) {
    // 保留位必须为零。错误不能静默掩盖，否则对端版本不匹配也会被接收。
    if ((b[0] & 0x0C) || (b[2] & 0x0F) || (b[6] & 0x0F) || (b[8] & 0x0F))
        throw std::invalid_argument("AoU Protocol Header 保留位非零");
    AouFlit f;
    f.fdid = b[0] & 3;
    f.msg_start = uint64_t(b[0] >> 4) | (uint64_t(b[1]) << 4) |
        (uint64_t(b[2] >> 4) << 12) | (uint64_t(b[3]) << 16) |
        (uint64_t(b[6] >> 4) << 24) | (uint64_t(b[7]) << 28) |
        (uint64_t(b[8] >> 4) << 36) | (uint64_t(b[9]) << 40);
    f.msg_credit = uint16_t(b[4]) | (uint16_t(b[5]) << 8);
    std::copy_n(b.begin() + PROTO_HEADER_BYTES, PAYLOAD_BYTES, f.payload);
    // -1 专门暴露误用辅助字段的代码。接收端不能拿它作为循环上界或统计值。
    f.used_granules = -1;
    f.valid = true;
    return f;
}
