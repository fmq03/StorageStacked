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

#include "fpga/sw/gpio.h"

#include <stdint.h>

#define GPIO_BASE 0x40030000
#define GPIO_DATA_IN (GPIO_BASE + 0x00)
#define GPIO_DATA_OUT (GPIO_BASE + 0x04)
#define GPIO_OUT_EN (GPIO_BASE + 0x08)

#define REG32(addr) (*(volatile uint32_t*)(uintptr_t)(addr))

uint32_t gpio_get_base_addr(void) { return GPIO_BASE; }

void gpio_set_output_enable(uint32_t mask) { REG32(GPIO_OUT_EN) = mask; }

void gpio_write(uint32_t value) { REG32(GPIO_DATA_OUT) = value; }

uint32_t gpio_read(void) { return REG32(GPIO_DATA_IN); }

uint32_t gpio_read_output(void) { return REG32(GPIO_DATA_OUT); }
