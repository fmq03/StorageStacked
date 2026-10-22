#include <riscv_vector.h>

#include <bit>
#include <cstdint>

uint32_t result_frm[4] __attribute__((section(".data")))  = {0};
uint32_t result_fcsr[4] __attribute__((section(".data"))) = {0};
uint32_t faulted __attribute__((section(".data")))        = 0;
uint32_t mcause __attribute__((section(".data")))         = 0;
uint32_t mtval __attribute__((section(".data")))          = 0;

extern "C" {
void coralnpu_exception_handler() {
  faulted = 1;
  uint32_t local_mcause;
  asm volatile("csrr %0, mcause" : "=r"(local_mcause));
  mcause = local_mcause;
  uint32_t local_mtval;
  asm volatile("csrr %0, mtval" : "=r"(local_mtval));
  mtval = local_mtval;
  asm volatile("ebreak");
  while (1) {
  }
}
}

int main() {
  float a           = std::bit_cast<float>(0x3f800001U);
  float b           = std::bit_cast<float>(0x3f800001U);
  float raw_a[4]    = {a, a, a, a};
  float raw_b[4]    = {b, b, b, b};
  float res_frm[4]  = {0};
  float res_fcsr[4] = {0};
  uint32_t vl       = 4;

  // Test 1: Write to frm and vfmul.vv
  asm volatile(
      "vsetvli zero, %[vl], e32, m1, ta, ma;"
      "vle32.v v0, (%[raw_a]);"
      "vle32.v v1, (%[raw_b]);"
      "csrw frm, %[frm_val];"
      "vfmul.vv v2, v0, v1;"
      "vse32.v v2, (%[res_frm]);"
      :
      :
      [frm_val] "r"(3), [vl] "r"(vl), [raw_a] "r"(raw_a), [raw_b] "r"(raw_b), [res_frm] "r"(res_frm)
      : "v0", "v1", "v2", "memory");

  // Reset frm to 0 (RNE) before next test
  asm volatile("csrw frm, zero");

  // Test 2: Write to fcsr and vfmul.vv
  asm volatile(
      "vsetvli zero, %[vl], e32, m1, ta, ma;"
      "vle32.v v0, (%[raw_a]);"
      "vle32.v v1, (%[raw_b]);"
      "csrw fcsr, %[fcsr_val];"
      "vfmul.vv v2, v0, v1;"
      "vse32.v v2, (%[res_fcsr]);"
      :
      : [fcsr_val] "r"(0x60), [vl] "r"(vl), [raw_a] "r"(raw_a), [raw_b] "r"(raw_b),
        [res_fcsr] "r"(res_fcsr)
      : "v0", "v1", "v2", "memory");

  for (int i = 0; i < 4; i++) {
    result_frm[i]  = std::bit_cast<uint32_t>(res_frm[i]);
    result_fcsr[i] = std::bit_cast<uint32_t>(res_fcsr[i]);
  }
  return 0;
}
