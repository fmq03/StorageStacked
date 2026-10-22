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

#include "fpga/sw/dma.h"

#define DMA_BASE 0x40050000
#define DMA_CTRL (DMA_BASE + 0x00)
#define DMA_STATUS (DMA_BASE + 0x04)
#define DMA_DESC_ADDR (DMA_BASE + 0x08)

#define REG32(addr) (*(volatile uint32_t*)(uintptr_t)(addr))

uint32_t dma_get_base_addr(void) { return DMA_BASE; }

void dma_start(uint32_t desc_addr) {
  REG32(DMA_DESC_ADDR) = desc_addr;
  REG32(DMA_CTRL) = 0x3;
}

uint32_t dma_get_status(void) { return REG32(DMA_STATUS); }

int dma_wait_done(void) {
  // Bounded poll to avoid hanging forever
  for (int i = 0; i < 1000000; i++) {
    uint32_t s = dma_get_status();
    if (s & 0x2) return (s & 0x4) ? -1 : 0;  // done: check error
  }
  return -2;  // timeout
}

uint32_t dma_make_len_flags(uint32_t len, uint32_t width_log2, int src_fixed,
                            int dst_fixed, int poll_en) {
  return (len & 0xFFFFFF) | ((width_log2 & 0x7) << 24) |
         ((src_fixed ? 1u : 0u) << 27) | ((dst_fixed ? 1u : 0u) << 28) |
         ((poll_en ? 1u : 0u) << 29);
}
