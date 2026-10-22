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

// 1D x 2D Decode MatMul (Register-Pinned GeMV)
// A: [1, K] (row-major), B: [K, N] (row-major), C: [1, N] (row-major)
extern "C" void rvv_gemv_1d_f32(const float* __restrict__ A,
                                const float* __restrict__ B,
                                float* __restrict__ C, int K, int N) {
  int c = 0;
  while (c < N) {
    int remaining = N - c;

    if (remaining >= 5) {
      // Unrolled Block (5 x LMUL=4)
      // Software pipelined to completely hide load-use latency on in-order
      // pipelines. Uses exactly 28 vector registers: 20 for accumulators (5x4)
      // + 8 for pipelined loads (2x4).
      size_t vl = __riscv_vsetvl_e32m4(remaining / 5);

      vfloat32m4_t vacc0 = __riscv_vfmv_v_f_f32m4(0.0f, vl);
      vfloat32m4_t vacc1 = __riscv_vfmv_v_f_f32m4(0.0f, vl);
      vfloat32m4_t vacc2 = __riscv_vfmv_v_f_f32m4(0.0f, vl);
      vfloat32m4_t vacc3 = __riscv_vfmv_v_f_f32m4(0.0f, vl);
      vfloat32m4_t vacc4 = __riscv_vfmv_v_f_f32m4(0.0f, vl);

      const float* rhs_ptr = B + c;
      for (int k = 0; k < K; k++) {
        float a = A[k];

        vfloat32m4_t vb0 = __riscv_vle32_v_f32m4(rhs_ptr + 0, vl);
        vfloat32m4_t vb1 = __riscv_vle32_v_f32m4(rhs_ptr + vl, vl);

        vacc0 = __riscv_vfmacc_vf_f32m4(vacc0, a, vb0, vl);
        vfloat32m4_t vb2 = __riscv_vle32_v_f32m4(rhs_ptr + 2 * vl, vl);

        vacc1 = __riscv_vfmacc_vf_f32m4(vacc1, a, vb1, vl);
        vfloat32m4_t vb3 = __riscv_vle32_v_f32m4(rhs_ptr + 3 * vl, vl);

        vacc2 = __riscv_vfmacc_vf_f32m4(vacc2, a, vb2, vl);
        vfloat32m4_t vb4 = __riscv_vle32_v_f32m4(rhs_ptr + 4 * vl, vl);

        vacc3 = __riscv_vfmacc_vf_f32m4(vacc3, a, vb3, vl);
        vacc4 = __riscv_vfmacc_vf_f32m4(vacc4, a, vb4, vl);

        rhs_ptr += N;
      }

      __riscv_vse32_v_f32m4(C + c + 0, vacc0, vl);
      __riscv_vse32_v_f32m4(C + c + vl, vacc1, vl);
      __riscv_vse32_v_f32m4(C + c + 2 * vl, vacc2, vl);
      __riscv_vse32_v_f32m4(C + c + 3 * vl, vacc3, vl);
      __riscv_vse32_v_f32m4(C + c + 4 * vl, vacc4, vl);

      c += 5 * vl;
    } else {
      size_t vl = __riscv_vsetvl_e32m4(remaining);
      vfloat32m4_t vacc0 = __riscv_vfmv_v_f_f32m4(0.0f, vl);

      const float* rhs_ptr = B + c;
      for (int k = 0; k < K; k++) {
        float a = A[k];
        vfloat32m4_t vb0 = __riscv_vle32_v_f32m4(rhs_ptr, vl);
        vacc0 = __riscv_vfmacc_vf_f32m4(vacc0, a, vb0, vl);
        rhs_ptr += N;
      }
      __riscv_vse32_v_f32m4(C + c, vacc0, vl);
      c += vl;
    }
  }
}

extern "C" void rvv_tiled_matmul_2d_f32(const float* __restrict__ A,
                                        const float* __restrict__ B,
                                        float* __restrict__ C, int M, int K,
                                        int N) {
  int m = 0;
  while (m < M) {
    int rows_rem = M - m;

    if (rows_rem >= 6) {
      for (int n = 0; n < N;) {
        int cols_rem = N - n;
        size_t vl = __riscv_vsetvl_e32m4(cols_rem);

        vfloat32m4_t vacc0 = __riscv_vfmv_v_f_f32m4(0.0f, vl);
        vfloat32m4_t vacc1 = __riscv_vfmv_v_f_f32m4(0.0f, vl);
        vfloat32m4_t vacc2 = __riscv_vfmv_v_f_f32m4(0.0f, vl);
        vfloat32m4_t vacc3 = __riscv_vfmv_v_f_f32m4(0.0f, vl);
        vfloat32m4_t vacc4 = __riscv_vfmv_v_f_f32m4(0.0f, vl);
        vfloat32m4_t vacc5 = __riscv_vfmv_v_f_f32m4(0.0f, vl);

        for (int k = 0; k < K; k++) {
          float a0 = A[(m + 0) * K + k];
          float a1 = A[(m + 1) * K + k];
          float a2 = A[(m + 2) * K + k];
          float a3 = A[(m + 3) * K + k];
          float a4 = A[(m + 4) * K + k];
          float a5 = A[(m + 5) * K + k];

          vfloat32m4_t vb = __riscv_vle32_v_f32m4(B + k * N + n, vl);

          vacc0 = __riscv_vfmacc_vf_f32m4(vacc0, a0, vb, vl);
          vacc1 = __riscv_vfmacc_vf_f32m4(vacc1, a1, vb, vl);
          vacc2 = __riscv_vfmacc_vf_f32m4(vacc2, a2, vb, vl);
          vacc3 = __riscv_vfmacc_vf_f32m4(vacc3, a3, vb, vl);
          vacc4 = __riscv_vfmacc_vf_f32m4(vacc4, a4, vb, vl);
          vacc5 = __riscv_vfmacc_vf_f32m4(vacc5, a5, vb, vl);
        }

        __riscv_vse32_v_f32m4(C + (m + 0) * N + n, vacc0, vl);
        __riscv_vse32_v_f32m4(C + (m + 1) * N + n, vacc1, vl);
        __riscv_vse32_v_f32m4(C + (m + 2) * N + n, vacc2, vl);
        __riscv_vse32_v_f32m4(C + (m + 3) * N + n, vacc3, vl);
        __riscv_vse32_v_f32m4(C + (m + 4) * N + n, vacc4, vl);
        __riscv_vse32_v_f32m4(C + (m + 5) * N + n, vacc5, vl);

        n += vl;
      }
      m += 6;
    } else {
      // Tail processing
      for (int i = 0; i < rows_rem; i++) {
        int act_m = m + i;
        for (int c = 0; c < N;) {
          int cols_rem = N - c;
          size_t vl = __riscv_vsetvl_e32m4(cols_rem);
          vfloat32m4_t vacc = __riscv_vfmv_v_f_f32m4(0.0f, vl);
          for (int k = 0; k < K; k++) {
            float a = A[act_m * K + k];
            vfloat32m4_t vb = __riscv_vle32_v_f32m4(B + k * N + c, vl);
            vacc = __riscv_vfmacc_vf_f32m4(vacc, a, vb, vl);
          }
          __riscv_vse32_v_f32m4(C + act_m * N + c, vacc, vl);
          c += vl;
        }
      }
      m += rows_rem;
    }
  }
}
