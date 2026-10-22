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

#include <stdint.h>
#include <stdlib.h>

#define CLK_REG_MAGIC 0x0
#define CLK_REG_MAIN 0x4
#define CLK_REG_ISP 0x8
#define CLK_REG_SPIM 0xC

#define CLK_MAGIC_VALUE 0x434C4B54  // "CLKT"

#define REG32(addr) (*(volatile uint32_t*)(uintptr_t)(addr))

static void check_magic(void) {
  if (REG32(CLK_TABLE_BASE + CLK_REG_MAGIC) != CLK_MAGIC_VALUE) {
    // Clock table not found! This is a fatal error in this new world.
    while (1);
  }
}

uint32_t clk_get_main_freq_mhz(void) {
  check_magic();
  return REG32(CLK_TABLE_BASE + CLK_REG_MAIN);
}

uint32_t clk_get_isp_freq_mhz(void) {
  check_magic();
  return REG32(CLK_TABLE_BASE + CLK_REG_ISP);
}

uint32_t clk_get_spim_freq_mhz(void) {
  check_magic();
  return REG32(CLK_TABLE_BASE + CLK_REG_SPIM);
}
