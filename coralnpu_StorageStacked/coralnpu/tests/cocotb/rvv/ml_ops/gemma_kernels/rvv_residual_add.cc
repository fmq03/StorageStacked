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
#include <stddef.h>
#include <stdint.h>

extern "C" {
void rvv_residual_add_f32(const float* A, const float* B, float* Y,
                          size_t total_elements) {
  size_t i = 0;

  while (total_elements - i >= 2) {
    size_t vl = __riscv_vsetvl_e32m4((total_elements - i) / 2);

    vfloat32m4_t va1 = __riscv_vle32_v_f32m4(&A[i], vl);
    vfloat32m4_t vb1 = __riscv_vle32_v_f32m4(&B[i], vl);

    vfloat32m4_t va2 = __riscv_vle32_v_f32m4(&A[i + vl], vl);
    vfloat32m4_t vb2 = __riscv_vle32_v_f32m4(&B[i + vl], vl);

    vfloat32m4_t vy1 = __riscv_vfadd_vv_f32m4(va1, vb1, vl);
    vfloat32m4_t vy2 = __riscv_vfadd_vv_f32m4(va2, vb2, vl);

    __riscv_vse32_v_f32m4(&Y[i], vy1, vl);
    __riscv_vse32_v_f32m4(&Y[i + vl], vy2, vl);

    i += 2 * vl;
  }

  while (i < total_elements) {
    size_t vl = __riscv_vsetvl_e32m8(total_elements - i);
    vfloat32m8_t va = __riscv_vle32_v_f32m8(&A[i], vl);
    vfloat32m8_t vb = __riscv_vle32_v_f32m8(&B[i], vl);
    vfloat32m8_t vy = __riscv_vfadd_vv_f32m8(va, vb, vl);
    __riscv_vse32_v_f32m8(&Y[i], vy, vl);
    i += vl;
  }
}
}
