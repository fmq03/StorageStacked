// Vortex GPU：读取 CPU 上传的输入，加一并写回。
#include "project_config.h"
#include <vx_spawn2.h>
#include <cstdint>

__kernel void kernel_main() {
    const unsigned index = blockIdx.x;
    auto *input = reinterpret_cast<volatile uint32_t *>(0x90000000u);
    auto *output = reinterpret_cast<volatile uint32_t *>(0x90010000u);
    auto *status = reinterpret_cast<volatile uint32_t *>(0x9005000cu);
    const uint32_t value = input[index];
    output[index] = value + 1u;
    const uint32_t observed = output[index];
    if (index == 0)
        *status = observed == value + 1u ? 0x600d0000u : 0xbad00001u;
}
