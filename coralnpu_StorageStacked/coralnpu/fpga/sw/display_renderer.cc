// Copyright 2026 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "fpga/sw/display_renderer.h"

#include <cstdio>
#include <cstring>

#include "fpga/sw/dma.h"
#include "fpga/sw/gpio.h"
#include "fpga/sw/spi.h"
#ifdef USE_UART
#include "fpga/sw/uart.h"
#endif
#include "third_party/waveshare_display/LCD_Driver.h"

#define REG32(addr) (*(volatile uint32_t*)(addr))

namespace coralnpu {

namespace {
struct DisplayInternalStorage {
  alignas(32) dma_descriptor dma_chain[32];
  alignas(32) DisplayCmd full_frame_cmd;
};
// Use a static instance for the aligned storage requirements.
static DisplayInternalStorage g_storage __attribute__((section(".extbss")));
}  // namespace

bool DisplayRenderer::Init() {
#ifdef USE_UART
  uart_puts("Renderer Init\r\n");
#endif
  uint32_t spi_base = spi_get_master_base_addr();
  // Enable=1, CPOL=0, HDTX=1 -> 0x0011
  spi_set_control(spi_base, SPI_CTRL_ENABLE | SPI_CTRL_HDTX);
  spi_set_csid(spi_base, 0);
  spi_set_csmode(spi_base, 0);

  gpio_set_output_enable(0xFF);

  LCD_Init(nullptr);
  LCD_Clear(nullptr, BLACK);

  // We don't initialize paint here anymore as it depends on user buffers.
  return true;
}

void DisplayRenderer::SetupFrameDma(const uint16_t* frame_buffer) {
  uint32_t spi_base = spi_get_master_base_addr();
  uint32_t gpio_base = gpio_get_base_addr();
  uint32_t spi_tx_addr = spi_base + SPI_REG_TXDATA;
  uint32_t spi_status_addr = spi_base + SPI_REG_STATUS;
  uint32_t gpio_out_addr = gpio_base + 0x04;

  g_storage.full_frame_cmd.cmd_2a = 0x2a;
  g_storage.full_frame_cmd.x_data[0] = 0;
  g_storage.full_frame_cmd.x_data[1] = 0;
  g_storage.full_frame_cmd.x_data[2] = (kLcdWidth - 1) >> 8;
  g_storage.full_frame_cmd.x_data[3] = (kLcdWidth - 1) & 0xff;
  g_storage.full_frame_cmd.cmd_2b = 0x2b;
  g_storage.full_frame_cmd.y_data[0] = 0;
  g_storage.full_frame_cmd.y_data[1] = 0;
  g_storage.full_frame_cmd.y_data[2] = (kLcdHeight - 1) >> 8;
  g_storage.full_frame_cmd.y_data[3] = (kLcdHeight - 1) & 0xff;
  g_storage.full_frame_cmd.cmd_2c = 0x2c;

  dma_descriptor* d = g_storage.dma_chain;
  int cur = 0;
  auto link = [&]() {
    d[cur].next_desc = (uint32_t)(uintptr_t)&d[cur + 1];
    cur++;
  };

  // 1. Column Address Set (0x2A)
  // DC=0
  d[cur].src_addr = (uint32_t)(uintptr_t)&dc_0_val_;
  d[cur].dst_addr = gpio_out_addr;
  d[cur].len_flags = dma_make_len_flags(4, 2, 1, 1, 0);
  link();
  // CMD 0x2A
  d[cur].src_addr = (uint32_t)(uintptr_t)&g_storage.full_frame_cmd.cmd_2a;
  d[cur].dst_addr = spi_tx_addr;
  d[cur].len_flags = dma_make_len_flags(1, 0, 1, 1, 1);
  d[cur].poll_addr = spi_status_addr;
  d[cur].poll_mask = 4;
  d[cur].poll_value = 0;
  link();
  // DC=1
  d[cur].src_addr = (uint32_t)(uintptr_t)&dc_1_val_;
  d[cur].dst_addr = gpio_out_addr;
  d[cur].len_flags = dma_make_len_flags(4, 2, 1, 1, 0);
  link();
  // DATA Start/End Column
  d[cur].src_addr = (uint32_t)(uintptr_t)g_storage.full_frame_cmd.x_data;
  d[cur].dst_addr = spi_tx_addr;
  d[cur].len_flags = dma_make_len_flags(4, 0, 0, 1, 1);
  d[cur].poll_addr = spi_status_addr;
  d[cur].poll_mask = 4;
  d[cur].poll_value = 0;
  link();

  // 2. Row Address Set (0x2B)
  // DC=0
  d[cur].src_addr = (uint32_t)(uintptr_t)&dc_0_val_;
  d[cur].dst_addr = gpio_out_addr;
  d[cur].len_flags = dma_make_len_flags(4, 2, 1, 1, 0);
  link();
  // CMD 0x2B
  d[cur].src_addr = (uint32_t)(uintptr_t)&g_storage.full_frame_cmd.cmd_2b;
  d[cur].dst_addr = spi_tx_addr;
  d[cur].len_flags = dma_make_len_flags(1, 0, 1, 1, 1);
  d[cur].poll_addr = spi_status_addr;
  d[cur].poll_mask = 4;
  d[cur].poll_value = 0;
  link();
  // DC=1
  d[cur].src_addr = (uint32_t)(uintptr_t)&dc_1_val_;
  d[cur].dst_addr = gpio_out_addr;
  d[cur].len_flags = dma_make_len_flags(4, 2, 1, 1, 0);
  link();
  // DATA Start/End Row
  d[cur].src_addr = (uint32_t)(uintptr_t)g_storage.full_frame_cmd.y_data;
  d[cur].dst_addr = spi_tx_addr;
  d[cur].len_flags = dma_make_len_flags(4, 0, 0, 1, 1);
  d[cur].poll_addr = spi_status_addr;
  d[cur].poll_mask = 4;
  d[cur].poll_value = 0;
  link();

  // 3. Memory Write (0x2C)
  // DC=0
  d[cur].src_addr = (uint32_t)(uintptr_t)&dc_0_val_;
  d[cur].dst_addr = gpio_out_addr;
  d[cur].len_flags = dma_make_len_flags(4, 2, 1, 1, 0);
  link();
  // CMD 0x2C
  d[cur].src_addr = (uint32_t)(uintptr_t)&g_storage.full_frame_cmd.cmd_2c;
  d[cur].dst_addr = spi_tx_addr;
  d[cur].len_flags = dma_make_len_flags(1, 0, 1, 1, 1);
  d[cur].poll_addr = spi_status_addr;
  d[cur].poll_mask = 4;
  d[cur].poll_value = 0;
  link();
  // DC=1
  d[cur].src_addr = (uint32_t)(uintptr_t)&dc_1_val_;
  d[cur].dst_addr = gpio_out_addr;
  d[cur].len_flags = dma_make_len_flags(4, 2, 1, 1, 0);
  link();

  // 4. Pixel data (Full frame)
  size_t frame_bytes = kLcdWidth * kLcdHeight * 2;
  d[cur].src_addr = (uint32_t)(uintptr_t)frame_buffer;
  d[cur].dst_addr = spi_tx_addr;
  d[cur].len_flags = dma_make_len_flags(frame_bytes, 0, 0, 1, 1);
  d[cur].poll_addr = spi_status_addr;
  d[cur].poll_mask = 4;
  d[cur].poll_value = 0;
  d[cur].next_desc = 0;
}

void DisplayRenderer::Render(const uint16_t* frame_buffer) {
  SetupFrameDma(frame_buffer);
  WaitDma();
  dma_start((uint32_t)(uintptr_t)g_storage.dma_chain);
  WaitDma();
}

void DisplayRenderer::WaitDma() {
  uintptr_t dma_base = dma_get_base_addr();
  int timeout = 0;
  while (dma_get_status() & 0x1) {
    uint32_t s = dma_get_status();
    if (s & 0x4) {
#ifdef USE_UART
      uart_puts("DMA ERR: Code=");
      uart_puthex8((s >> 4) & 0xF);
      uart_puts(" Desc=");
      uart_puthex32(REG32(dma_base + 0x0c));
      uart_puts("\r\n");
#endif
      (*(volatile uint32_t*)dma_base) = 0x4;
      break;
    }
    if (++timeout > 1000000) {
#ifdef USE_UART
      uart_puts("DMA Timeout! Status=");
      uart_puthex32(s);
      uart_puts(" Desc=");
      uart_puthex32(REG32(dma_base + 0x0c));
      uart_puts("\r\n");
#endif
      break;
    }
  }
}

}  // namespace coralnpu
