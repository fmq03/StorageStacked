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

#include "fpga/sw/i2c.h"

#include <stdint.h>

#include "fpga/sw/clk.h"

#define I2C_BASE 0x40040000

#define I2C_CTRL_OFFSET 0x008
#define I2C_STATUS_OFFSET 0x00C
#define I2C_FDATA_OFFSET 0x010
#define I2C_CLK_DIV_OFFSET 0x018

#define REG32(addr) (*((volatile uint32_t*)(uintptr_t)(addr)))

void i2c_init(uint32_t target_khz) {
  uint32_t main_freq_mhz = clk_get_main_freq_mhz();
  uint32_t clk_div = (main_freq_mhz * 1000) / (target_khz * 4) - 1;
  REG32(I2C_BASE + I2C_CLK_DIV_OFFSET) = clk_div;
  REG32(I2C_BASE + I2C_CTRL_OFFSET) = 0x1;  // Enable
}

uint32_t i2c_get_status(void) { return REG32(I2C_BASE + I2C_STATUS_OFFSET); }

void i2c_wait_idle(void) {
  while (i2c_get_status() & 0x3);  // Busy or !fifo_empty
}

void i2c_write_fdata(uint32_t data) {
  REG32(I2C_BASE + I2C_FDATA_OFFSET) = data;
}

uint32_t i2c_read_fdata(void) { return REG32(I2C_BASE + I2C_FDATA_OFFSET); }
