#ifndef SOFTMAX_EXP_H
#define SOFTMAX_EXP_H

#include <stdint.h>

// Softmax only needs finite x <= 0. Keep all arithmetic in native FP32:
// the RV32 library expf path also calls software double-precision routines.
inline float device_exp(float x) {
    // The discarded contribution is below 2e-35, far below the output tolerance.
    if (x < -80.0f)
        return 0.0f;

    const int k = static_cast<int>(x * 1.4426950408889634f - 0.5f);
    // Split ln(2) to avoid cancellation during range reduction.
    const float r = (x - k * 0.693145751953125f) - k * 1.428606765330187e-6f;
    const float polynomial = 1.0f + r * (1.0f + r * (0.5f + r *
        (1.0f / 6.0f + r * (1.0f / 24.0f + r * (1.0f / 120.0f + r *
        (1.0f / 720.0f + r * (1.0f / 5040.0f)))))));
    const uint32_t bits = static_cast<uint32_t>(k + 127) << 23;
    float scale;
    __builtin_memcpy(&scale, &bits, sizeof(scale));
    return polynomial * scale;
}

inline float device_sqrt(float x) { float y; asm ("fsqrt.s %0, %1" : "=f"(y) : "f"(x)); return y; }

#endif
