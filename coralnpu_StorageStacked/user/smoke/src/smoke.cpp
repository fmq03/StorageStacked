// Four real external transactions: write input, read input, write output, read output.
#include "project_config.h"
#include <cstdint>

int main() {
    auto *input = reinterpret_cast<volatile uint32_t *>(0x90000000u);
    auto *output = reinterpret_cast<volatile uint32_t *>(0x90010000u);
    auto *mailbox = reinterpret_cast<volatile uint32_t *>(0xc0000000u);

    *input = SMOKE_INPUT;
    const uint32_t value = *input;
    *output = value + 1u;
    const uint32_t observed = *output;
    const uint32_t expected = uint32_t(SMOKE_INPUT) + 1u;
    *mailbox = observed == expected ? 0x600d0000u : 0xbad00001u;
    asm volatile("wfi");
    return 0;
}
