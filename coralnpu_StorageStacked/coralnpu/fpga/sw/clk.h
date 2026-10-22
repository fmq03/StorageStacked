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

#ifndef FPGA_SW_CLK_H_
#define FPGA_SW_CLK_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CLK_TABLE_BASE 0x40001000

uint32_t clk_get_main_freq_mhz(void);
uint32_t clk_get_isp_freq_mhz(void);
uint32_t clk_get_spim_freq_mhz(void);

#ifdef __cplusplus
}
#endif

#endif  // FPGA_SW_CLK_H_
