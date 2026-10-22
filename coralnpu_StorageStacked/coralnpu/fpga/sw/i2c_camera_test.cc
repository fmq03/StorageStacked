/*
 * Copyright 2026 Google LLC
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <stdint.h>

#include "fpga/sw/i2c.h"
#include "fpga/sw/uart.h"

uint8_t hm01b0_read_reg(uint16_t reg) {
  // START, ADDR 0x24, W
  i2c_write_fdata(I2C_START | (0x24 << 1) | 0);
  // ADDR H
  i2c_write_fdata((reg >> 8) & 0xFF);
  // ADDR L
  i2c_write_fdata(reg & 0xFF);
  // RESTART, ADDR 0x24, R
  i2c_write_fdata(I2C_START | (0x24 << 1) | 1);
  // READ, STOP
  i2c_write_fdata(I2C_READ | I2C_STOP);

  i2c_wait_idle();

  return i2c_read_fdata() & 0xFF;
}

void hm01b0_write_reg(uint16_t reg, uint8_t data) {
  // START, ADDR 0x24, W
  i2c_write_fdata(I2C_START | (0x24 << 1) | 0);
  // ADDR H
  i2c_write_fdata((reg >> 8) & 0xFF);
  // ADDR L
  i2c_write_fdata(reg & 0xFF);
  // DATA, STOP
  i2c_write_fdata(I2C_STOP | data);

  i2c_wait_idle();
}

typedef struct {
  uint16_t addr;
  uint8_t data;
} init_reg_t;

const init_reg_t init_regs[] = {
    {0x0103, 0x00},  // HM01B0_SW_RESET
    {0x0100, 0x00},  // HM01B0_MODE_SELECT
    {0x0101, 0x03},  // HM01B0_IMAGE_ORIENTATION (Non-SPARROW)
    {0x1003, 0x08},  // HM01B0_BLC_TGT
    {0x1007, 0x08},  // HM01B0_BLC2_TGT
    {0x3044, 0x0A}, {0x3045, 0x00},
    {0x3047, 0x0A}, {0x3050, 0xC0},
    {0x3051, 0x42}, {0x3052, 0x50},
    {0x3053, 0x00}, {0x3054, 0x03},
    {0x3055, 0xF7}, {0x3056, 0xF8},
    {0x3057, 0x29}, {0x3058, 0x1F},
    {0x3059, 0x1E},  // HM01B0_BIT_CONTROL
    {0x3064, 0x00},  // HM01B0_SYNC_EN
    {0x3065, 0x04},  // HM01B0_OUTPUT_PIN_STATUS_CTRL
    {0x1000, 0x43},  // HM01B0_BLC_CFG
    {0x1001, 0x40}, {0x1002, 0x32},
    {0x0350, 0x7F}, {0x1006, 0x01},  // HM01B0_BLC2_EN
    {0x1008, 0x00}, {0x1009, 0xA0},
    {0x100A, 0x60}, {0x100B, 0x90},
    {0x100C, 0x40}, {0x1012, 0x00},  // HM01B0_VSYNC_HSYNC_PIXEL_SHIFT_EN
    {0x2000, 0x07},                  // HM01B0_STATISTIC_CTRL
    {0x2003, 0x00}, {0x2004, 0x1C},
    {0x2007, 0x00}, {0x2008, 0x58},
    {0x200B, 0x00}, {0x200C, 0x7A},
    {0x200F, 0x00}, {0x2010, 0xB8},
    {0x2013, 0x00},  // HM01B0_MD_LROI_Y_START_H
    {0x2014, 0x58},  // HM01B0_MD_LROI_Y_START_L
    {0x2017, 0x00},  // HM01B0_MD_LROI_X_END_H
    {0x2018, 0x9B},  // HM01B0_MD_LROI_X_END_L
    {0x2100, 0x01},  // HM01B0_AE_CTRL
    {0x2104, 0x07},  // HM01B0_CONVERGE_OUT_TH
    {0x2105, 0x02},  // HM01B0_MAX_INTG_H  (30Fps)
    {0x2106, 0x14},  // HM01B0_MAX_INTG_L
    {0x2108, 0x03},  // HM01B0_MAX_AGAIN_FULL
    {0x2109, 0x03},  // HM01B0_MAX_AGAIN_BIN2
    {0x210B, 0x80},  // HM01B0_MAX_DGAIN
    {0x210F, 0x00},  // HM01B0_FS_60HZ_H
    {0x2110, 0x85},  // HM01B0_FS_60HZ_L
    {0x2111, 0x00},  // HM01B0_FS_50HZ_H
    {0x2112, 0xA0},  // HM01B0_FS_50HZ_L
    {0x2150, 0x03},  // HM01B0_MD_CTRL
    {0x0340, 0x02},  // HM01B0_FRAME_LENGTH_LINES_H
    {0x0341, 0x16},  // HM01B0_FRAME_LENGTH_LINES_L
    {0x0342, 0x01},  // HM01B0_LINE_LENGTH_PCK_H
    {0x0343, 0x78},  // HM01B0_LINE_LENGTH_PCK_L
    {0x3010, 0x01},  // HM01B0_QVGA_WIN_EN
    {0x0383, 0x01},  // HM01B0_READOUT_X
    {0x0387, 0x01},  // HM01B0_READOUT_Y
    {0x0390, 0x00},  // HM01B0_BINNING_MODE
    {0x3011, 0x70},  // HM01B0_SIX_BIT_MODE_EN
    {0x3059, 0x02},  // HM01B0_BIT_CONTROL
    {0x3060, 0x01},  // HM01B0_OSC_CLK_DIV
    {0x0104, 0x01},  // HM01B0_GRP_PARAM_HOLD
    {0x0100, 0x05}   // HM01B0_MODE_SELECT
};

int main() {
  uart_init();

  i2c_init(100);  // 100kHz standard mode

  uart_puts("I2C Camera Test Starting...\r\n");

  uint8_t id_h = hm01b0_read_reg(0x0000);
  uint8_t id_l = hm01b0_read_reg(0x0001);

  uart_puts("HM01B0 ID: 0x");
  uart_puthex8(id_h);
  uart_puthex8(id_l);
  uart_puts("\r\n");

  if (id_h == 0x01 && id_l == 0xB0) {
    uart_puts("HM01B0 ID Match PASSED\r\n");
  } else {
    uart_puts("HM01B0 ID Match FAILED\r\n");
  }

  uart_puts("Writing Initialization Registers...\r\n");
  for (unsigned int i = 0; i < sizeof(init_regs) / sizeof(init_regs[0]); i++) {
    hm01b0_write_reg(init_regs[i].addr, init_regs[i].data);
  }

  uart_puts("Verifying Initialization Registers...\r\n");
  int err_count = 0;
  for (unsigned int i = 0; i < sizeof(init_regs) / sizeof(init_regs[0]); i++) {
    uint16_t addr = init_regs[i].addr;

    // Check if this is the last write to this address
    int is_last = 1;
    for (unsigned int j = i + 1; j < sizeof(init_regs) / sizeof(init_regs[0]);
         j++) {
      if (init_regs[j].addr == addr) {
        is_last = 0;
        break;
      }
    }

    if (is_last) {
      // Waive write-only registers
      if (addr == 0x0103 || addr == 0x3052 || addr == 0x0104) {
        continue;
      }

      uint8_t val = hm01b0_read_reg(addr);
      if (val != init_regs[i].data) {
        uart_puts("FAIL at 0x");
        uart_puthex8(addr >> 8);
        uart_puthex8(addr & 0xFF);
        uart_puts(" Exp: 0x");
        uart_puthex8(init_regs[i].data);
        uart_puts(" Got: 0x");
        uart_puthex8(val);
        uart_puts("\r\n");
        err_count++;
      }
    }
  }

  if (err_count == 0) {
    uart_puts("HM01B0 All Initialization Registers Write/Read PASSED\r\n");
  } else {
    uart_puts("HM01B0 Initialization Register Write/Read FAILED\r\n");
  }

  return 0;
}
