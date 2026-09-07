/**
 * @file aou_wire.h
 * @brief AoU v0.8 图 5 的 Protocol Header 编码及固定 250B PLP 容器。
 *
 * 工程边界统一采用 [PH B0..B9][G0..G47] 的逻辑顺序。这是 FDI 软件容器，
 * 在物理 256B Format 6 中还需按图 4 散布，不能从物理 byte 2 连续复制。
 * 图 5 中每字节右侧的 0..7 是该字节的 bit 编号；不要把消息内部的数据
 * MSB-first 规则再次套到整个 PH 上做字节或 bit 翻转。
 * used_granules、AouFlit::valid 均不序列化；传输有效性由 ready/valid 或 FIFO
 * 的一次成功读写表达。反序列化不推测有效粒度，交给有续传状态的解析器。
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
