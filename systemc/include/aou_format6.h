/**
 * @file aou_format6.h
 * @brief AoU v0.8 图 4 的逻辑 250B 与物理 256B 字节散布/收集。
 *
 * 本文件不依赖 SystemC，供桥侧单测和 UCIe 参考模型共同使用。
 * 这里只负责标准规定的字节位置；FH 内容、CRC 多项式及计算由链路提供。
 * 当前参考链路沿用自身的序号/replay 头和 CRC16-CCITT 行为抽象，不能据此
 * 宣称实现了完整 UCIe DLL 规范。CRC 槽分别为 126/127、254/255。
 */
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>

namespace aou_format6 {
using Payload = std::array<std::uint8_t, 250>;
using Frame = std::array<std::uint8_t, 256>;

inline constexpr std::array<unsigned, 10> ph_offsets{
    62, 63, 64, 65, 128, 129, 190, 191, 192, 193};

// 四个 60B 数据区分别装 12 个 5B granule，中间交错插入 PH/FH/CRC。
inline Frame scatter(const Payload& plp,
                     const std::array<std::uint8_t, 2>& fh = {},
                     const std::array<std::uint8_t, 4>& crc = {}) {
    Frame frame{};
    std::copy(fh.begin(), fh.end(), frame.begin());
    for (unsigned i = 0; i < ph_offsets.size(); ++i) frame[ph_offsets[i]] = plp[i];
    for (unsigned block = 0; block < 4; ++block)
        std::copy_n(plp.begin() + 10 + block * 60, 60, frame.begin() + block * 64 + 2);
    frame[126] = crc[0]; frame[127] = crc[1];
    frame[254] = crc[2]; frame[255] = crc[3];
    return frame;
}

inline Payload gather(const Frame& frame) {
    Payload plp{};
    for (unsigned i = 0; i < ph_offsets.size(); ++i) plp[i] = frame[ph_offsets[i]];
    for (unsigned block = 0; block < 4; ++block)
        std::copy_n(frame.begin() + block * 64 + 2, 60, plp.begin() + 10 + block * 60);
    return plp;
}
} // namespace aou_format6
