// Copyright 2026 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "fpga/sw/clk.h"

#include "fpga/sw/uart.h"

int main() {
  uart_init();

  uint32_t main_freq = clk_get_main_freq_mhz();
  uint32_t isp_freq = clk_get_isp_freq_mhz();
  uint32_t spim_freq = clk_get_spim_freq_mhz();

  if (main_freq == 0 || isp_freq == 0 || spim_freq == 0) {
    return 1;
  }

  uart_puts("PASS\r\n");
  return 0;
}
