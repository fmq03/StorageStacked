// Copyright 2026 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <riscv_vector.h>

#include <cmath>
#include <cstdint>

// Implements the RMS Normalization layer for Gemma.
// Mathematically equivalent to: x * rsqrt(mean(x^2) + eps) * (1.0 + weight)
// Reference PyTorch implementation:
// https://github.com/google/gemma_pytorch/blob/main/gemma/model.py#L179
extern "C" void RmsNormF(size_t seq_len, size_t hidden_size, float epsilon,
                         const float* input, const float* weight,
                         float* output) {
  size_t vlmax = __riscv_vsetvlmax_e32m4();
  vfloat32m1_t vzero_m1 =
      __riscv_vfmv_v_f_f32m1(0.0f, __riscv_vsetvlmax_e32m1());

  for (size_t s = 0; s < seq_len; ++s) {
    const float* token_in = input + (s * hidden_size);
    float* token_out = output + (s * hidden_size);

    // -------------------------------------------------------------
    // PASS 1: Calculate Sum of Squares (Reduction)
    // -------------------------------------------------------------
    vfloat32m4_t vacc = __riscv_vfmv_v_f_f32m4(0.0f, vlmax);

    size_t k = hidden_size;
    const float* x_ptr = token_in;

    while (k) {
      size_t vl = __riscv_vsetvl_e32m4(k);

      vfloat32m4_t vx = __riscv_vle32_v_f32m4(x_ptr, vl);
      x_ptr += vl;

      vacc = __riscv_vfmacc_vv_f32m4_tu(vacc, vx, vx, vl);
      k -= vl;
    }
    vfloat32m1_t vred = __riscv_vfredusum_vs_f32m4_f32m1(vacc, vzero_m1, vlmax);
    float sum_squares = __riscv_vfmv_f_s_f32m1_f32(vred);
    float rms = sum_squares / hidden_size;
    float sqrt_val = std::sqrt(rms + epsilon);
    float inv_rms = 1.0f / sqrt_val;

    // -------------------------------------------------------------
    // PASS 2: Normalize and Scale with Weights
    // -------------------------------------------------------------
    k = hidden_size;
    x_ptr = token_in;
    const float* w_ptr = weight;
    float* out_ptr = token_out;

    while (k) {
      size_t vl = __riscv_vsetvl_e32m4(k);

      vfloat32m4_t vx = __riscv_vle32_v_f32m4(x_ptr, vl);
      x_ptr += vl;

      vfloat32m4_t vw = __riscv_vle32_v_f32m4(w_ptr, vl);
      w_ptr += vl;

      vfloat32m4_t vx_norm = __riscv_vfmul_vf_f32m4(vx, inv_rms, vl);
      vfloat32m4_t vy = __riscv_vfmacc_vv_f32m4(vx_norm, vx_norm, vw, vl);

      __riscv_vse32_v_f32m4(out_ptr, vy, vl);
      out_ptr += vl;

      k -= vl;
    }
  }
}
