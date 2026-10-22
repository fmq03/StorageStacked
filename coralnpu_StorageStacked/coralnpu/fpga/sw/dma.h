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

#ifndef FPGA_SW_DMA_H_
#define FPGA_SW_DMA_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DMA_BASE 0x40050000

uint32_t dma_get_base_addr(void);

// Descriptor layout (32 bytes, must be 32-byte aligned)
struct __attribute__((packed, aligned(32))) dma_descriptor {
  uint32_t src_addr;
  uint32_t dst_addr;
  uint32_t len_flags;
  uint32_t next_desc;
  uint32_t poll_addr;
  uint32_t poll_mask;
  uint32_t poll_value;
  uint32_t reserved;
};

void dma_start(uint32_t desc_addr);
int dma_wait_done(void);
uint32_t dma_get_status(void);

uint32_t dma_make_len_flags(uint32_t len, uint32_t width_log2, int src_fixed,
                            int dst_fixed, int poll_en);

#ifdef __cplusplus
}
#endif

#endif  // FPGA_SW_DMA_H_
