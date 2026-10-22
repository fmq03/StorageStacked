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

#include "DEV_Config.h"
#include <stdint.h>

#include "fpga/sw/gpio.h"
#include "fpga/sw/spi.h"

void DEV_Digital_Write(void* ctx, UWORD Pin, UBYTE Value) {
    if (Pin == DEV_CS_PIN) {
        // Ignore CS pin control. The SPI controller handles CS in Auto mode.
        return;
    }

    uint32_t gpio_bit = 0;
    if (Pin == DEV_DC_PIN) gpio_bit = 0;
    else if (Pin == DEV_RST_PIN) gpio_bit = 1;
    else return; // Ignore others

    uint32_t current = gpio_read_output();
    if (Value) current |= (1 << gpio_bit);
    else current &= ~(1 << gpio_bit);
    gpio_write(current);
}

void DEV_SPI_WRITE(void* ctx, UBYTE Value) {
    uint32_t spi_base = spi_get_master_base_addr();
    // Wait for TX FIFO not full (Status bit 2 is TX Full)
    while (spi_get_status(spi_base) & (1 << 2));

    // Write data
    spi_write_txdata(spi_base, Value);

    // Wait for Busy (bit 0) to clear.
    while (spi_get_status(spi_base) & 1);

    // Drain RX FIFO to prevent stalling.
    // Bit 1 is RX Empty (1 if empty).
    while (!(spi_get_status(spi_base) & (1 << 1))) {
        (void)spi_read_rxdata(spi_base);
    }
}

void DEV_SPI_BLOCK_WRITE(void* ctx, const uint8_t* buffer, size_t bytes) {
    for (size_t i = 0; i < bytes; ++i) {
        DEV_SPI_WRITE(ctx, buffer[i]);
    }
}

void DEV_Delay_ms(void* ctx, UWORD Value) {
    // Basic loop for delays.
    for (volatile int i = 0; i < 50000 * Value; ++i) {
        asm volatile("nop");
    }
}
