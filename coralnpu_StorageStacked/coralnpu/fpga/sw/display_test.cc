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

#include "third_party/waveshare_display/LCD_Driver.h"
#include <stdint.h>

#include "fpga/sw/gpio.h"
#include "fpga/sw/spi.h"

// Buffer for text drawing (200x16 pixels)
#define TEXT_W 200
#define TEXT_H 16
static uint16_t text_buffer[TEXT_W * TEXT_H];

int main() {
    uint32_t spi_base = spi_get_master_base_addr();
    // Enable=1, CPOL=0, CPHA=0 -> 0x0001
    spi_set_control(spi_base, SPI_CTRL_ENABLE);
    spi_set_csid(spi_base, 0);
    spi_set_csmode(spi_base, 0); // Auto

    gpio_set_output_enable(0xFF); // All output

    // Initialize text buffer to background color
    for(int i = 0; i < TEXT_W * TEXT_H; i++) {
        text_buffer[i] = BLUE;
    }

    PAINT paint;
    paint.Image = text_buffer;
    paint.WidthMemory = TEXT_W;
    paint.HeightMemory = TEXT_H;
    paint.Color = WHITE;
    paint.Rotate = 0;
    paint.Mirror = 0;
    paint.WidthByte = TEXT_W;
    paint.HeightByte = TEXT_H;
    paint.Width = TEXT_W;
    paint.Height = TEXT_H;
    // Draw string into buffer
    Paint_DrawString_EN(&paint, 0, 0, "Hello Coral!", &Font16, BLUE, WHITE);
    // Swap bytes for Big Endian display (RISC-V is Little Endian)
    for(int i = 0; i < TEXT_W * TEXT_H; i++) {
        text_buffer[i] = (text_buffer[i] << 8) | (text_buffer[i] >> 8);
    }

    LCD_Init(nullptr);
    LCD_Clear(nullptr, RED);
    for (int i = 0; i < 10; i++) {
        // Send buffer to display at position (60, 100)
        LCD_ClearToBufferWindow(nullptr, text_buffer, 60, 100, 60 + TEXT_W, 100 + TEXT_H);
    }
    return 0;
}
