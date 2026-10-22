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

#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <vector>

#include "svdpi.h"

#define WIDTH 320
#define HEIGHT 240

struct SimDisplay {
  uint16_t framebuffer[WIDTH * HEIGHT];
  uint16_t xs, xe, ys, ye;
  uint16_t x, y;
  bool in_reset;
  bool madctl_mv;  // Row/Column Exchange

  // For handling incoming pixel data
  uint8_t pending_byte;
  bool have_pending_byte;
  bool ramwr_active;

  SimDisplay() { Reset(); }

  void Reset() {
    std::fill(std::begin(framebuffer), std::end(framebuffer), 0);
    xs = 0;
    xe = WIDTH - 1;
    ys = 0;
    ye = HEIGHT - 1;
    x = 0;
    y = 0;
    in_reset = false;
    madctl_mv = false;
    pending_byte = 0;
    have_pending_byte = false;
    ramwr_active = false;
  }

  void ProcessCommand(uint8_t cmd) {
    ramwr_active = false;  // New command ends RAMWR
    have_pending_byte = false;

    switch (cmd) {
      case 0x2A:  // CASET
      case 0x2B:  // RASET
      case 0x36:  // MADCTL
      case 0x3A:  // COLMOD
        // Wait for data
        break;
      case 0x2C:  // RAMWR
        ramwr_active = true;
        x = xs;
        y = ys;
        break;
      case 0x01:  // SWRESET
        Reset();
        break;
    }
  }

  void ProcessData(uint8_t data, uint8_t last_cmd) {
    static int param_idx = 0;
    // Reset param index if last_cmd changed? No, we don't track history that
    // well. Assuming data follows immediately.

    switch (last_cmd) {
      case 0x2A:  // CASET
        if (param_idx == 0) xs = (xs & 0xFF) | (data << 8);
        if (param_idx == 1) xs = (xs & 0xFF00) | data;
        if (param_idx == 2) xe = (xe & 0xFF) | (data << 8);
        if (param_idx == 3) {
          xe = (xe & 0xFF00) | data;
          param_idx = -1;
        }
        param_idx++;
        break;
      case 0x2B:  // RASET
        if (param_idx == 0) ys = (ys & 0xFF) | (data << 8);
        if (param_idx == 1) ys = (ys & 0xFF00) | data;
        if (param_idx == 2) ye = (ye & 0xFF) | (data << 8);
        if (param_idx == 3) {
          ye = (ye & 0xFF00) | data;
          param_idx = -1;
        }
        param_idx++;
        break;
      case 0x36:  // MADCTL
        madctl_mv = (data & 0x20);
        param_idx = 0;  // Single byte param
        break;
      case 0x2C:  // RAMWR
        if (!have_pending_byte) {
          pending_byte = data;
          have_pending_byte = true;
        } else {
          uint16_t pixel = (pending_byte << 8) | data;
          have_pending_byte = false;
          WritePixel(pixel);
        }
        break;
      default:
        param_idx = 0;
    }
  }

  void WritePixel(uint16_t pixel) {
    if (x >= WIDTH || y >= HEIGHT) return;
    framebuffer[y * WIDTH + x] = pixel;

    // Increment cursor
    x++;
    if (x > xe) {
      x = xs;
      y++;
      if (y > ye) {
        y = ys;
      }
    }
  }

  void DumpPPM(const std::string& filename) {
    std::ofstream file(filename, std::ios::binary);
    if (!file.is_open()) return;

    file << "P6\n" << WIDTH << " " << HEIGHT << "\n255\n";
    for (int i = 0; i < WIDTH * HEIGHT; ++i) {
      uint16_t p = framebuffer[i];
      // RGB565 -> RGB888
      uint8_t r = (p >> 11) & 0x1F;
      uint8_t g = (p >> 5) & 0x3F;
      uint8_t b = p & 0x1F;

      r = (r * 255) / 31;
      g = (g * 255) / 63;
      b = (b * 255) / 31;

      file << r << g << b;
    }
    file.close();
    std::cout << "DPI: Display dumped to " << filename << std::endl;
  }
};

struct DisplayDpiState {
  uint8_t csb_prev;
  uint8_t sck_prev;
  uint8_t dc_prev;
  uint8_t rst_prev;
  uint8_t shift_reg;
  int bit_count;
  uint8_t last_cmd;
  SimDisplay display;
};

extern "C" {

struct DisplayDpiState* display_dpi_init() {
  struct DisplayDpiState* ctx = new DisplayDpiState();
  ctx->csb_prev = 1;
  ctx->sck_prev = 0;
  ctx->dc_prev = 0;
  ctx->rst_prev = 0;
  ctx->shift_reg = 0;
  ctx->bit_count = 0;
  ctx->last_cmd = 0;
  std::cout << "DPI: Display Model Initialized" << std::endl;
  return ctx;
}

void display_dpi_close(struct DisplayDpiState* ctx) {
  if (ctx) {
    ctx->display.DumpPPM("/tmp/display_out.ppm");
    delete ctx;
  }
  std::cout << "DPI: Display Model Closed" << std::endl;
}

void display_dpi_reset(struct DisplayDpiState* ctx) {
  if (ctx) {
    ctx->csb_prev = 1;
    ctx->sck_prev = 0;
    ctx->dc_prev = 0;
    ctx->rst_prev = 0;
    ctx->shift_reg = 0;
    ctx->bit_count = 0;
    ctx->last_cmd = 0;
    ctx->display.Reset();
  }
}

void display_dpi_tick(struct DisplayDpiState* ctx, unsigned char sck,
                      unsigned char csb, unsigned char mosi, unsigned char dc,
                      unsigned char rst, unsigned char* miso) {
  *miso = 0;  // Default MISO low

  if (!ctx) return;

  if (dc != ctx->dc_prev) {
    std::cout << "DPI: Display DC changed to " << (int)dc << std::endl;
    ctx->dc_prev = dc;
  }

  if (rst != ctx->rst_prev) {
    std::cout << "DPI: Display RST changed to " << (int)rst << std::endl;
    ctx->rst_prev = rst;
  }

  // Handle Reset (Active Low)
  if (!rst) {
    if (!ctx->display.in_reset) {
      std::cout << "DPI: Display Reset Asserted" << std::endl;
      ctx->display.Reset();
      ctx->display.in_reset = true;
    }
    // Update state even in reset to avoid false edges when reset releases
    ctx->csb_prev = csb;
    ctx->sck_prev = sck;
    return;
  }

  if (ctx->display.in_reset) {
    std::cout << "DPI: Display Reset Deasserted" << std::endl;
    ctx->display.in_reset = false;
  }

  // Detect CSB rising edge (End of transaction)
  if (!ctx->csb_prev && csb) {
    if (ctx->bit_count % 8 != 0) {
      std::cout << "DPI: WARNING: CSB rose with partial byte! bit_count="
                << ctx->bit_count << " shift_reg=0x" << std::hex
                << (int)ctx->shift_reg << std::dec << std::endl;
    }
    ctx->bit_count = 0;

    // If we were writing pixels, maybe dump the frame now?
    if (ctx->display.ramwr_active) {
      // std::cout << "DPI: End of RAMWR transaction" << std::endl;
      ctx->display.DumpPPM("/tmp/display_out.ppm");
    }
  }

  // Capture data on SCK rising edge if CSB is low
  if (!csb && !ctx->sck_prev && sck) {
    ctx->shift_reg = (ctx->shift_reg << 1) | (mosi & 1);
    ctx->bit_count++;

    if (ctx->bit_count == 8) {
      uint8_t byte = ctx->shift_reg;

      if (dc == 0) {  // Command
        std::cout << "DPI: Display CMD: 0x" << std::hex << std::setw(2)
                  << std::setfill('0') << (int)byte << std::dec << std::endl;
        ctx->last_cmd = byte;
        ctx->display.ProcessCommand(byte);
      } else {  // Data
        // Only log data if it's not a pixel flood to avoid spam, or log
        // sparingly For now, let's log everything to debug "black screen"
        std::cout << "DPI: Display DATA: 0x" << std::hex << std::setw(2)
                  << std::setfill('0') << (int)byte << std::dec
                  << " (Last CMD: 0x" << std::hex << (int)ctx->last_cmd << ")"
                  << std::endl;
        ctx->display.ProcessData(byte, ctx->last_cmd);
      }
      ctx->bit_count = 0;
      ctx->shift_reg = 0;
    }
  }

  ctx->csb_prev = csb;
  ctx->sck_prev = sck;
}

}  // extern "C"
