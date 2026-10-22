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

#include "fpga/sw/spi_flash.h"

#include <stdint.h>

#include "fpga/sw/gpio.h"
#include "fpga/sw/spi.h"
#include "fpga/sw/uart.h"

static void print_hex8(uint8_t val) {
  char buf[5];
  buf[0] = '0';
  buf[1] = 'x';
  int hi = (val >> 4) & 0xF;
  int lo = val & 0xF;
  buf[2] = hi < 10 ? '0' + hi : 'A' + hi - 10;
  buf[3] = lo < 10 ? '0' + lo : 'A' + lo - 10;
  buf[4] = 0;
  uart_puts(buf);
}

static void print_uint32(uint32_t val) {
  char buf[11];
  for (int i = 0; i < 8; i++) {
    int digit = (val >> (28 - i * 4)) & 0xF;
    buf[i] = digit < 10 ? '0' + digit : 'A' + digit - 10;
  }
  buf[8] = 0;
  uart_puts("0x");
  uart_puts(buf);
}

int main() {
  uart_init();

  spi_flash_init();

  {
    // ===== Test 1: Hardware Reset =====
    uart_puts("T1: HW Reset\r\n");

    // Set WEL
    spi_flash_write_enable();

    // Verify WEL is set (Status Bit 1)
    uint8_t status;
    spi_flash_poll_status(0x02, 0x02, &status, 10000);

    if (!(status & 0x02)) {
      uart_puts("FAIL T1: WEL not set\r\n");
      return 1;
    }

    // Pulse Reset
    spi_flash_hw_reset();

    // Verify WEL is cleared
    spi_flash_poll_status(0x02, 0x00, &status, 10000);

    if (status & 0x02) {
      uart_puts("FAIL T1: WEL still set after reset\r\n");
      return 1;
    }
    uart_puts("T1: OK\r\n");
  }

  {
    // ===== Test 2: Read JEDEC ID =====
    uart_puts("T2: JEDEC ID\r\n");

    uint8_t mfr, id1, id2;
    spi_flash_read_id(&mfr, &id1, &id2);

    uart_puts("  MFR=");
    print_hex8(mfr);
    uart_puts(" ID1=");
    print_hex8(id1);
    uart_puts(" ID2=");
    print_hex8(id2);
    uart_puts("\r\n");

    if (mfr != 0x01 || id1 != 0x02 || id2 != 0x20) {
      uart_puts("FAIL T2: wrong JEDEC ID\r\n");
      return 1;
    }
    uart_puts("T2: OK\r\n");
  }

  {
    // ===== Test 5: SFDP Discovery =====
    uart_puts("T5: SFDP Discovery\r\n");

    struct spi_flash_info info;
    if (!spi_flash_discover(&info)) {
      uart_puts("FAIL T5: discovery failed\r\n");
      return 1;
    }

    uart_puts("  Capacity: ");
    print_uint32(info.capacity_bytes);
    uart_puts(" bytes\r\n");

    uart_puts("  Sector Size: ");
    print_uint32(info.sector_size_bytes);
    uart_puts(" bytes\r\n");

    if (info.capacity_bytes != 64 * 1024 * 1024) {
      uart_puts("FAIL T5: wrong capacity\r\n");
      return 1;
    }

    if (info.sector_size_bytes != 256 * 1024) {
      uart_puts("FAIL T5: wrong sector size\r\n");
      return 1;
    }

    uart_puts("T5: OK\r\n");
  }

  {
    // ===== Test 3: Sector Erase + Verify =====
    uart_puts("T3: Erase+Verify\r\n");

    // Write Enable
    spi_flash_write_enable();
    uint8_t result;
    if (!spi_flash_poll_status(0x02, 0x02, &result, 10000)) {
      uart_puts("FAIL T3: WREN failed: ");
      print_hex8(result);
      uart_puts("\r\n");
      return 1;
    }

    // Sector Erase at address 0x101010
    spi_flash_sector_erase(0x101010);

    // Poll until the write is done (not busy bit 0)
    // poll_limit=0 means poll forever.
    if (!spi_flash_poll_status(0x01, 0x00, &result, 0)) {
      uart_puts("FAIL T3: Erase did not finish: ");
      print_hex8(result);
      uart_puts("\r\n");
      return 1;
    }

    // Read back - should be 0xFF
    uint8_t data[4];
    spi_flash_read(0x101010, data, 4);

    uart_puts("  Read: ");
    print_hex8(data[0]);
    uart_puts(" ");
    print_hex8(data[1]);
    uart_puts(" ");
    print_hex8(data[2]);
    uart_puts(" ");
    print_hex8(data[3]);
    uart_puts("\r\n");

    if (data[0] != 0xFF || data[1] != 0xFF || data[2] != 0xFF ||
        data[3] != 0xFF) {
      uart_puts("FAIL T3: erase failed\r\n");
      return 1;
    }
    uart_puts("T3: OK\r\n");
  }

  {
    // ===== Test 4: Page Program + Read =====
    uart_puts("T4: Program+Read\r\n");

    {
      // Read back at address 0x101010
      uint8_t data[4];
      spi_flash_read(0x101010, data, 4);

      if (data[0] != 0xFF || data[1] != 0xFF || data[2] != 0xFF ||
          data[3] != 0xFF) {
        uart_puts("FAIL T4: Program target not erased?\r\n");
        return 1;
      }
    }

    // Write Enable
    spi_flash_write_enable();

    uint8_t result;
    if (!spi_flash_poll_status(0x02, 0x02, &result, 10000)) {
      uart_puts("FAIL T4: WREN failed: ");
      print_hex8(result);
      uart_puts("\r\n");
      return 1;
    }

    // Page Program at address 0x101010
    uint8_t write_data[4] = {0xDE, 0xAD, 0xBE, 0xEF};
    spi_flash_page_program(0x101010, write_data, 4);

    // Poll until not busy
    if (!spi_flash_poll_status(0x01, 0x00, nullptr, 0)) {
      uart_puts("FAIL T4: Write did not finish?\r\n");
      return 1;
    }

    // Read back at address 0x101010
    uint8_t read_data[4];
    spi_flash_read(0x101010, read_data, 4);

    uart_puts("  Read: ");
    print_hex8(read_data[0]);
    uart_puts(" ");
    print_hex8(read_data[1]);
    uart_puts(" ");
    print_hex8(read_data[2]);
    uart_puts(" ");
    print_hex8(read_data[3]);
    uart_puts("\r\n");

    if (read_data[0] != 0xDE || read_data[1] != 0xAD || read_data[2] != 0xBE ||
        read_data[3] != 0xEF) {
      uart_puts("FAIL T4: data mismatch\r\n");
      return 1;
    }
    uart_puts("T4: OK\r\n");
  }

  uart_puts("TEST PASSED\r\n");
  return 0;
}
