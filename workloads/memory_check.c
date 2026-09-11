#include <stdint.h>
#include <stdio.h>

/* Mapped by configs/run.py: do not run this binary on the host OS. */
int main(void) {
    volatile uint8_t *p = (volatile uint8_t *)(uintptr_t)0x90000000;
    uint8_t expected[192];
    for (unsigned i = 0; i < sizeof(expected); ++i) {
        expected[i] = (i * 29 + 17) & 255;
        p[i] = expected[i];
    }
    for (unsigned i = 1; i < sizeof(expected); i += 3) {
        expected[i] ^= 0x5a;
        p[i] = expected[i];
    }
    uint32_t sum = 0;
    for (unsigned i = 0; i < sizeof(expected); ++i) {
        uint8_t got = p[i];
        if (got != expected[i]) {
            printf("CPU AXI FAIL byte=%u got=%u expected=%u\n", i, got, expected[i]);
            return 1;
        }
        sum += got;
    }
    volatile uint64_t *q = (volatile uint64_t *)(uintptr_t)0x90000ff8;
    q[0] = 0x0123456789abcdefULL;
    q[1] = 0xfedcba9876543210ULL;
    if (q[0] != 0x0123456789abcdefULL || q[1] != 0xfedcba9876543210ULL) return 2;
    printf("CPU AXI PASS bytes=%u checksum=%u boundary64=ok\n",
           (unsigned)sizeof(expected), sum);
    return 0;
}

