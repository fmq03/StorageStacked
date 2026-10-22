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

#ifndef FPGA_SW_I2C_H_
#define FPGA_SW_I2C_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// I2C Flags
#define I2C_START (1 << 8)
#define I2C_STOP (1 << 9)
#define I2C_READ (1 << 10)

void i2c_init(uint32_t target_khz);
void i2c_wait_idle(void);
void i2c_write_fdata(uint32_t data);
uint32_t i2c_read_fdata(void);
uint32_t i2c_get_status(void);

#ifdef __cplusplus
}
#endif

#endif  // FPGA_SW_I2C_H_
