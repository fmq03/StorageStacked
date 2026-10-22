#include <bit>
#include <cstdint>

uint32_t result __attribute__((section(".data")))  = 0;
uint32_t faulted __attribute__((section(".data"))) = 0;
uint32_t mcause __attribute__((section(".data")))  = 0;
uint32_t mtval __attribute__((section(".data")))   = 0;

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
  // 1. Pre-initialize frm to an invalid value (5)
  asm volatile("csrw frm, %[invalid]" : : [invalid] "r"(5));

  // 2. Write valid value (3) to fcsr and immediately execute fmul.s
  float a = std::bit_cast<float>(0x3f800001U);
  float b = std::bit_cast<float>(0x3f800001U);
  float res;
  uint32_t fcsr_val = 0x60;  // frm = 3 (RUP)
  asm volatile(
      "csrw fcsr, %[fcsr];"
      "fmul.s %[res], %[a], %[b], dyn;"
      : [res] "=f"(res)
      : [fcsr] "r"(fcsr_val), [a] "f"(a), [b] "f"(b));
  result = std::bit_cast<uint32_t>(res);
  return 0;
}
