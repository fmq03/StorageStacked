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

int32_t fault_count __attribute__((section(".data"))) = 0;

uint8_t buffer[4096] __attribute__((section(".data")));
uint8_t *in_ptr __attribute__((section(".data")))  = &(buffer[0]);
uint8_t *out_ptr __attribute__((section(".data"))) = &(buffer[0]);
size_t vl __attribute__((section(".data")))        = 32;

extern "C" {
void coralnpu_exception_handler() {
  uint32_t mcause;
  asm volatile("csrr %[mcause], mcause;" : [mcause] "=r"(mcause));
  fault_count += 1;
  asm volatile("ebreak");
  while (1) {
  }
}
}

int main() {
  vuint8m4_t v = __riscv_vle8_v_u8m4(in_ptr, vl);
  // This triggers a fault by attempting to write to ITCM.
  // TODO(davidgao): get an address from ldscript, don't hardcode.
  __riscv_vse8_v_u8m4((uint8_t *)0x0, v, vl);
  // We should have jumped to the handler. This is not reachable.
  __riscv_vse8_v_u8m4(out_ptr, v, vl);

  return 0;
}
