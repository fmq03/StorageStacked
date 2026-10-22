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

#include <stdint.h>

#include "fpga/sw/clk.h"
#include "fpga/sw/uart.h"

// SRAM
#define SRAM_BASE 0x20000000

// All buffers placed in SRAM
static volatile uint32_t* const src32 =
    (volatile uint32_t*)(SRAM_BASE + 0x0000);
static volatile uint32_t* const dst32 =
    (volatile uint32_t*)(SRAM_BASE + 0x1000);
static volatile struct dma_descriptor* const desc0 =
    (volatile struct dma_descriptor*)(SRAM_BASE + 0x2000);
static volatile struct dma_descriptor* const desc1 =
    (volatile struct dma_descriptor*)(SRAM_BASE + 0x2020);
static volatile uint32_t* const src32b =
    (volatile uint32_t*)(SRAM_BASE + 0x0100);
static volatile uint32_t* const dst32b =
    (volatile uint32_t*)(SRAM_BASE + 0x1100);

int main() {
  uart_init();

  // ===== Test 1: Simple mem-to-mem (64 bytes = 16 words) =====
  const uint32_t nwords = 16;
  const uint32_t nbytes = nwords * 4;

  for (uint32_t i = 0; i < nwords; i++) src32[i] = 0xDEAD0000 + i;
  for (uint32_t i = 0; i < nwords; i++) dst32[i] = 0;

  desc0->src_addr = (uint32_t)(uintptr_t)src32;
  desc0->dst_addr = (uint32_t)(uintptr_t)dst32;
  desc0->len_flags = dma_make_len_flags(nbytes, 2, 0, 0, 0);
  desc0->next_desc = 0;
  desc0->poll_addr = 0;
  desc0->poll_mask = 0;
  desc0->poll_value = 0;
  desc0->reserved = 0;

  dma_start((uint32_t)(uintptr_t)desc0);

  int rc = dma_wait_done();
  if (rc) {
    uart_puts("FAIL T1\r\n");
    return 1;
  }

  // Verify silently — only print on mismatch
  for (uint32_t i = 0; i < nwords; i++) {
    if (dst32[i] != 0xDEAD0000 + i) {
      uart_puts("FAIL T1v\r\n");
      return 1;
    }
  }

  // ===== Test 2: Descriptor chaining =====
  const uint32_t nwords2 = 8;
  const uint32_t nbytes2 = nwords2 * 4;

  for (uint32_t i = 0; i < nwords2; i++) src32b[i] = 0xBEEF0000 + i;
  for (uint32_t i = 0; i < nwords2; i++) dst32b[i] = 0;
  for (uint32_t i = 0; i < nwords; i++) dst32[i] = 0;

  desc0->src_addr = (uint32_t)(uintptr_t)src32;
  desc0->dst_addr = (uint32_t)(uintptr_t)dst32;
  desc0->len_flags = dma_make_len_flags(nbytes, 2, 0, 0, 0);
  desc0->next_desc = (uint32_t)(uintptr_t)desc1;

  desc1->src_addr = (uint32_t)(uintptr_t)src32b;
  desc1->dst_addr = (uint32_t)(uintptr_t)dst32b;
  desc1->len_flags = dma_make_len_flags(nbytes2, 2, 0, 0, 0);
  desc1->next_desc = 0;
  desc1->poll_addr = 0;
  desc1->poll_mask = 0;
  desc1->poll_value = 0;
  desc1->reserved = 0;

  dma_start((uint32_t)(uintptr_t)desc0);

  rc = dma_wait_done();
  if (rc) {
    uart_puts("FAIL T2\r\n");
    return 1;
  }

  for (uint32_t i = 0; i < nwords; i++) {
    if (dst32[i] != 0xDEAD0000 + i) {
      uart_puts("FAIL T2a\r\n");
      return 1;
    }
  }
  for (uint32_t i = 0; i < nwords2; i++) {
    if (dst32b[i] != 0xBEEF0000 + i) {
      uart_puts("FAIL T2b\r\n");
      return 1;
    }
  }

// ===== Test 3: Mem-to-periph via UART0 (fixed destination) =====
// DMA a known byte sequence to UART0 WDATA. This exercises real
// mem-to-periph with a fixed destination address on an actual peripheral.
#define UART0_BASE 0x40000000
#define UART0_CTRL (UART0_BASE + 0x10)
#define UART0_STATUS (UART0_BASE + 0x14)
#define UART0_WDATA (UART0_BASE + 0x1c)

#define REG32(addr) (*(volatile uint32_t*)(addr))

  // Enable UART0 TX (same NCO formula as uart_init)
  {
    const uint32_t main_freq_mhz = clk_get_main_freq_mhz();
    const uint64_t nco =
        ((uint64_t)115200 << 20) / ((uint64_t)main_freq_mhz * 1000000);
    REG32(UART0_CTRL) = (uint32_t)((nco << 16) | 3);
  }

  const uint32_t t3_len = 4;  // 4 bytes
  volatile uint8_t* const t3_src = (volatile uint8_t*)(SRAM_BASE + 0x3000);
  t3_src[0] = 'D';
  t3_src[1] = 'M';
  t3_src[2] = 'A';
  t3_src[3] = '\n';

  desc0->src_addr = (uint32_t)(uintptr_t)t3_src;
  desc0->dst_addr = UART0_WDATA;
  desc0->len_flags =
      dma_make_len_flags(t3_len, 0, 0, 1, 0);  // 1-byte beats, dst_fixed
  desc0->next_desc = 0;
  desc0->poll_addr = 0;
  desc0->poll_mask = 0;
  desc0->poll_value = 0;
  desc0->reserved = 0;

  dma_start((uint32_t)(uintptr_t)desc0);

  rc = dma_wait_done();
  if (rc) {
    uart_puts("FAIL T3\r\n");
    return 1;
  }

// ===== Test 4: Periph-to-mem (fixed source from UART0 STATUS) =====
// Read UART0 STATUS register repeatedly into a buffer using fixed source.
// All destination words should contain the same status value.
#define UART0_RDATA (UART0_BASE + 0x18)
  const uint32_t nwords4 = 8;
  const uint32_t nbytes4 = nwords4 * 4;

  for (uint32_t i = 0; i < nwords4; i++) dst32[i] = 0;

  // Read UART0 STATUS (a real peripheral register) repeatedly
  desc0->src_addr = UART0_STATUS;
  desc0->dst_addr = (uint32_t)(uintptr_t)dst32;
  desc0->len_flags = dma_make_len_flags(nbytes4, 2, 1, 0, 0);  // src_fixed=1
  desc0->next_desc = 0;
  desc0->poll_addr = 0;
  desc0->poll_mask = 0;
  desc0->poll_value = 0;
  desc0->reserved = 0;

  dma_start((uint32_t)(uintptr_t)desc0);

  rc = dma_wait_done();
  if (rc) {
    uart_puts("FAIL T4\r\n");
    return 1;
  }

  // All reads of the same status register should return the same value.
  // Verify all words match the first one (we don't know the exact value,
  // but they should be consistent).
  uint32_t expected_status = dst32[0];
  for (uint32_t i = 1; i < nwords4; i++) {
    if (dst32[i] != expected_status) {
      uart_puts("FAIL T4v\r\n");
      return 1;
    }
  }

  // ===== Test 5: Polled DMA to UART0 (flow control) =====
  // DMA bytes to UART0 WDATA, polling UART0 STATUS bit 0 (TX full).
  // The DMA waits until the TX FIFO has space before each byte write.
  const uint32_t t5_len = 8;
  volatile uint8_t* const t5_src = (volatile uint8_t*)(SRAM_BASE + 0x3200);
  t5_src[0] = 'P';
  t5_src[1] = 'O';
  t5_src[2] = 'L';
  t5_src[3] = 'L';
  t5_src[4] = '_';
  t5_src[5] = 'O';
  t5_src[6] = 'K';
  t5_src[7] = '\n';

  desc0->src_addr = (uint32_t)(uintptr_t)t5_src;
  desc0->dst_addr = UART0_WDATA;
  desc0->len_flags =
      dma_make_len_flags(t5_len, 0, 0, 1, 1);  // 1-byte, dst_fixed, poll_en
  desc0->next_desc = 0;
  desc0->poll_addr = UART0_STATUS;
  desc0->poll_mask = 0x00000001;   // bit 0 = TX full
  desc0->poll_value = 0x00000000;  // wait until TX not full
  desc0->reserved = 0;

  dma_start((uint32_t)(uintptr_t)desc0);

  rc = dma_wait_done();
  if (rc) {
    uart_puts("FAIL T5\r\n");
    return 1;
  }

  uart_puts("PASS\r\n");
  return 0;
}
