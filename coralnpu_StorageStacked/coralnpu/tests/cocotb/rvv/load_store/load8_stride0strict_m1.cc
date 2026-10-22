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

typedef uint8_t uint8x16_t __attribute__((vector_size(16)));

uint8_t in_buf[32] __attribute__((section(".data")));   // 1 in use.
uint8_t out_buf[16] __attribute__((section(".data")));  // 16 in use.

__attribute__((used, retain)) void test_asm(const uint8_t *x, uint8_t *y) {
  vuint8m1_t v;
  asm("vsetivli zero, 16, e8, m1, ta, ma;"
      "vlse8.v %[v], %[addr], %[stride];"
      : [v] "=vr"(v)
      : [addr] "A"(*(uint8_t(*)[16])x), [stride] "r"(0)
      : "vtype", "vl");
  __riscv_vse8_v_u8m1(y, v, 16);
}

__attribute__((used, retain)) void test_vector(const uint8_t *x, uint8_t *y) {
  uint8x16_t v = {
      x[0], x[0], x[0], x[0], x[0], x[0], x[0], x[0],
      x[0], x[0], x[0], x[0], x[0], x[0], x[0], x[0],
  };
  *reinterpret_cast<uint8x16_t *>(y) = v;
}

int main(int argc, char **argv) {
  test_asm(in_buf, out_buf);
  return 0;
}
