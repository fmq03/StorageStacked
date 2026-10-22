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
#include <stdint.h>

#include <cmath>
#include <cstddef>

extern "C" void rvv_tanh_gelu_mul_f32(const float* gate, const float* up,
                                      float* output, size_t total_elements) {
  const float CA = 0.79788456f;   // sqrt(2/pi)
  const float CB = 0.035677408f;  // sqrt(2/pi) * 0.044715
  const float CC = 0.5f;

  size_t k = total_elements;
  const float* x_ptr = gate;
  const float* up_ptr = up;
  float* out_ptr = output;

  while (k > 0) {
    size_t vl = __riscv_vsetvl_e32m4(k);

    vfloat32m4_t vx = __riscv_vle32_v_f32m4(x_ptr, vl);
    vfloat32m4_t vup = __riscv_vle32_v_f32m4(up_ptr, vl);

    // -------------------------------------------------------------
    // PASS 1: z = CA*x + CB*x^3
    // -------------------------------------------------------------
    vfloat32m4_t vx2 = __riscv_vfmul_vv_f32m4(vx, vx, vl);
    vfloat32m4_t vx3 = __riscv_vfmul_vv_f32m4(vx2, vx, vl);

    vfloat32m4_t vz = __riscv_vfmul_vf_f32m4(vx, CA, vl);
    // Fused Multiply-Accumulate: vz = vz + CB * x^3
    vz = __riscv_vfmacc_vf_f32m4(vz, CB, vx3, vl);

    // -------------------------------------------------------------
    // PASS 2: Vectorized Tanh Approximation
    // y = clamp(z, -3.0, 3.0)
    // tanh(z) ~= (y^3 + 27y) / (9y^2 + 27)
    // -------------------------------------------------------------
    vfloat32m4_t vy = __riscv_vfmax_vf_f32m4(vz, -3.0f, vl);
    vy = __riscv_vfmin_vf_f32m4(vy, 3.0f, vl);

    vfloat32m4_t vy2 = __riscv_vfmul_vv_f32m4(vy, vy, vl);
    vfloat32m4_t v_num = __riscv_vfmul_vv_f32m4(vy2, vy, vl);  // y^3

    v_num = __riscv_vfmacc_vf_f32m4(v_num, 27.0f, vy, vl);
    vfloat32m4_t v_den = __riscv_vfmul_vf_f32m4(vy2, 9.0f, vl);
    v_den = __riscv_vfadd_vf_f32m4(v_den, 27.0f, vl);

    vfloat32m4_t vtanh_z = __riscv_vfdiv_vv_f32m4(v_num, v_den, vl);

    // -------------------------------------------------------------
    // PASS 3: Output = 0.5 * x * up * (1 + tanh(z))
    // -------------------------------------------------------------
    vfloat32m4_t vK = __riscv_vfmul_vv_f32m4(vx, vup, vl);
    vK = __riscv_vfmul_vf_f32m4(vK, CC, vl);

    vfloat32m4_t vout = __riscv_vfmacc_vv_f32m4(vK, vK, vtanh_z, vl);

    __riscv_vse32_v_f32m4(out_ptr, vout, vl);

    x_ptr += vl;
    up_ptr += vl;
    out_ptr += vl;
    k -= vl;
  }
}
