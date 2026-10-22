// Trivial test to validate coralnpu_v2_sim_test infrastructure.
#include <stdint.h>

#include "fpga/sw/uart.h"

int main() {
  uart_init();
  uart_puts("PASS\r\n");
  return 0;
}
