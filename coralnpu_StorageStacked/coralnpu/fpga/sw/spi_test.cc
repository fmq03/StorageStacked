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

#include "fpga/sw/spi.h"

#include "fpga/sw/uart.h"

#define SPI_MASTER_BASE 0x40020000

int main() {
  uart_init();

  // 1. Enable SPI Master
  spi_init(SPI_MASTER_BASE, 1, 0, 0);

  // 2. Select CSID 0 and Auto Mode
  spi_set_csid(SPI_MASTER_BASE, 0);
  spi_set_csmode(SPI_MASTER_BASE, 0);

  // 3. Send Data
  spi_xfer(SPI_MASTER_BASE, 0xDE);
  spi_xfer(SPI_MASTER_BASE, 0xAD);
  spi_xfer(SPI_MASTER_BASE, 0xBE);
  spi_xfer(SPI_MASTER_BASE, 0xEF);

  // Wait for TX FIFO to empty (Status bit 2 is TX Full, bit 0 is Busy)
  while (spi_get_status(SPI_MASTER_BASE) & 1);

  uart_puts("SPI transmit complete!\n");

  // Signal completion (using a known address or just loop)
  return 0;
}
