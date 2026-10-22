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

#include <stdint.h>
#include <string.h>

#include "fpga/sw/flash_tool_status.h"
#include "fpga/sw/spi_flash.h"
#include "fpga/sw/uart.h"

// Command IDs
#define FLASH_TOOL_CMD_GET_INFO 1
#define FLASH_TOOL_CMD_PROGRAM_DATA 2
#define FLASH_TOOL_CMD_HELLO 3

// Magic ready value
#define FLASH_TOOL_READY_MAGIC 0xFEEDFACE

// Command structure (128-bit aligned for SPI-friendly access).
struct flash_tool_pkt {
  uint32_t cmd;
  uint32_t addr;
  uint32_t len;
  union {
    uint32_t crc32;   // Host provided CRC32 of the data in buffer
    uint32_t status;  // Device provided status
  };
} __attribute__((aligned(16)));

volatile uint32_t flash_tool_ready
    __attribute__((section(".data"), aligned(16))) = 0;
volatile struct flash_tool_pkt flash_tool_cmd
    __attribute__((section(".data"), aligned(16))) = {0};
volatile struct flash_tool_pkt flash_tool_resp
    __attribute__((section(".data"), aligned(16))) = {0};

uint8_t flash_tool_buffer[256 * 1024]
    __attribute__((section(".noinit"), used, retain, aligned(16)));
uint8_t flash_tool_staging[256 * 1024]
    __attribute__((section(".noinit"), used, retain, aligned(16)));

// Nibble-based CRC32 lookup table (polynomial 0xEDB88320)
static const uint32_t crc32_nibble_table[16] = {
    0x00000000, 0x1DB71064, 0x3B6E20C8, 0x26D930AC, 0x76DC4190, 0x6B6B51F4,
    0x4DB26158, 0x5005713C, 0xEDB88320, 0xF00F9344, 0xD6D6A3E8, 0xCB61B38C,
    0x9B64C2B0, 0x86D3D2D4, 0xA00AE278, 0xBDBDF21C};

static uint32_t calculate_crc32(const uint8_t* data, uint32_t len) {
  uint32_t crc = 0xFFFFFFFF;
  for (uint32_t i = 0; i < len; i++) {
    crc ^= data[i];
    crc = (crc >> 4) ^ crc32_nibble_table[crc & 0x0F];
    crc = (crc >> 4) ^ crc32_nibble_table[crc & 0x0F];
  }
  return ~crc;
}

static flash_tool_status_t handle_hello(void) { return FLASH_TOOL_STATUS_OK; }

static flash_tool_status_t handle_get_info(struct spi_flash_info* info) {
  if (!spi_flash_discover(info)) {
    return FLASH_TOOL_STATUS_DISCOVERY_FAILED;
  }

  if (info->sector_size_bytes > sizeof(flash_tool_buffer)) {
    return FLASH_TOOL_STATUS_BUFFER_TOO_SMALL;
  }

  uint32_t* info_buf = (uint32_t*)flash_tool_buffer;
  info_buf[0] = info->capacity_bytes;
  info_buf[1] = info->sector_size_bytes;
  info_buf[2] = info->page_size_bytes;

  return FLASH_TOOL_STATUS_OK;
}

static flash_tool_status_t handle_program_data(struct spi_flash_info* info,
                                               uint32_t addr, uint32_t len,
                                               uint32_t host_crc) {
  uint32_t dev_crc = calculate_crc32(flash_tool_buffer, len);
  if (dev_crc != host_crc) {
    return FLASH_TOOL_STATUS_CRC_MISMATCH;
  }

  if (addr >= 16 * 1024 * 1024) {
    return FLASH_TOOL_STATUS_ADDRESS_ERROR;
  }

  if (info->sector_size_bytes == 0 ||
      info->sector_size_bytes > sizeof(flash_tool_buffer)) {
    return FLASH_TOOL_STATUS_NOT_INITIALIZED;
  }

  if (len > info->sector_size_bytes) {
    return FLASH_TOOL_STATUS_INVALID_LEN;
  }

  uint32_t sector_mask = ~(info->sector_size_bytes - 1);
  uint32_t sector_addr = addr & sector_mask;
  uint32_t offset = addr & (info->sector_size_bytes - 1);

  if (offset + len > info->sector_size_bytes) {
    return FLASH_TOOL_STATUS_BOUNDARY_ERROR;
  }

  spi_flash_read(sector_addr, flash_tool_staging, info->sector_size_bytes);
  memcpy(flash_tool_staging + offset, flash_tool_buffer, len);

  if (!spi_flash_sector_erase(sector_addr)) {
    return FLASH_TOOL_STATUS_TIMEOUT;
  }

  for (uint32_t off = 0; off < info->sector_size_bytes;
       off += info->page_size_bytes) {
    if (!spi_flash_page_program(sector_addr + off, flash_tool_staging + off,
                                info->page_size_bytes)) {
      return FLASH_TOOL_STATUS_TIMEOUT;
    }
  }

  spi_flash_read(sector_addr, flash_tool_buffer, info->sector_size_bytes);
  if (memcmp(flash_tool_staging, flash_tool_buffer, info->sector_size_bytes) !=
      0) {
    return FLASH_TOOL_STATUS_VERIFY_FAILED;
  }

  return FLASH_TOOL_STATUS_OK;
}

int main() {
  spi_flash_init();

  struct spi_flash_info info;
  memset(&info, 0, sizeof(info));

  flash_tool_ready = FLASH_TOOL_READY_MAGIC;

  while (1) {
    if (flash_tool_cmd.cmd != 0) {
      uint32_t cmd = flash_tool_cmd.cmd;
      uint32_t addr = flash_tool_cmd.addr;
      uint32_t len = flash_tool_cmd.len;
      uint32_t host_crc = flash_tool_cmd.crc32;

      struct flash_tool_pkt resp = {0};
      resp.cmd = cmd;

      switch (cmd) {
        case FLASH_TOOL_CMD_HELLO:
          resp.status = handle_hello();
          break;
        case FLASH_TOOL_CMD_GET_INFO:
          resp.status = handle_get_info(&info);
          break;
        case FLASH_TOOL_CMD_PROGRAM_DATA:
          resp.status = handle_program_data(&info, addr, len, host_crc);
          break;
        default:
          resp.status =
              FLASH_TOOL_STATUS_NOT_INITIALIZED;  // Or some UNKNOWN status
          break;
      }

      // Signal completion
      flash_tool_resp = resp;
      flash_tool_cmd.cmd = 0;
    }
  }

  return 0;
}
