#pragma once
#include <vortex2.h>
#include "project_config.h"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#define CHECK(call) do { const vx_result_t r=(call); if(r!=VX_SUCCESS) { \
    std::fprintf(stderr,"Runtime error: %s: %s\n",#call,vx_result_string(r)); return 1; } } while(0)

using Readback = std::map<unsigned, std::vector<uint8_t>>;

inline vx_result_t wait_event(vx_event_h event) {
    const auto result = vx_event_wait_value(event, 1, VX_TIMEOUT_INFINITE);
    vx_event_release(event);
    return result;
}

// Preserve the exact bytes returned by the runtime for independent validation.
inline vx_result_t readback_all(vx_queue_h queue, vx_buffer_h buffer, Readback& data) {
    for (const auto& range : readback_ranges) {
        auto& bytes = data[range.offset];
        bytes.resize(range.bytes);
        vx_event_h event = nullptr;
        auto result = vx_enqueue_read(queue, bytes.data(), buffer, range.offset,
                                      range.bytes, 0, nullptr, &event);
        if (result != VX_SUCCESS) return result;
        result = wait_event(event);
        if (result != VX_SUCCESS) return result;
        static constexpr char hex[] = "0123456789abcdef";
        std::string line;
        line.reserve(bytes.size() * 2);
        for (auto byte : bytes) { line += hex[byte >> 4]; line += hex[byte & 15]; }
        std::printf("READBACK %08x %s\n", unsigned(0x90000000u + range.offset), line.c_str());
    }
    return VX_SUCCESS;
}

inline uint32_t readback_word(const std::vector<uint8_t>& data, unsigned index) {
    uint32_t word;
    std::memcpy(&word, data.data() + index * sizeof(word), sizeof(word));
    return word;
}
