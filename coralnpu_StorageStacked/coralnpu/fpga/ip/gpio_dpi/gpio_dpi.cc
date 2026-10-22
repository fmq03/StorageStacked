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

#include <iomanip>
#include <iostream>

#include "svdpi.h"

struct GpioDpiState {
  int prev_gpio_o;
  int prev_gpio_en;
};

extern "C" {

void* gpio_dpi_init() {
  GpioDpiState* ctx = new GpioDpiState();
  ctx->prev_gpio_o = -1;
  ctx->prev_gpio_en = -1;
  std::cout << "DPI: GPIO Initialized" << std::endl;
  return ctx;
}

void gpio_dpi_close(void* ctx) {
  if (ctx) {
    delete static_cast<GpioDpiState*>(ctx);
  }
}

void gpio_dpi_tick(void* ctx_void, int gpio_o, int gpio_en_o, int* gpio_i) {
  GpioDpiState* ctx = static_cast<GpioDpiState*>(ctx_void);

  // Log changes to outputs
  if (gpio_o != ctx->prev_gpio_o || gpio_en_o != ctx->prev_gpio_en) {
    std::cout << "DPI: GPIO Output Changed: Data=0x" << std::hex << gpio_o
              << " En=0x" << gpio_en_o << std::dec << std::endl;
    ctx->prev_gpio_o = gpio_o;
    ctx->prev_gpio_en = gpio_en_o;
  }

  // Basic loopback logic: if output enabled, reflect it back to input.
  // If not enabled, input is 0 (or pulled up/down if we modeled it).
  // Loopback bits where en=1.
  *gpio_i = (gpio_o & gpio_en_o);
}
}
