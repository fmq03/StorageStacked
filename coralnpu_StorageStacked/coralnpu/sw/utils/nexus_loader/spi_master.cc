#include "spi_master.h"

#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>

void SpiMaster::send_cmd(const std::vector<uint8_t>& cmd) {
  int n = ftdi_->write_data(cmd.data(), cmd.size());
  if (n != static_cast<int>(cmd.size())) {
    fprintf(stderr, "[ERROR] FTDI write failed: wrote %d/%zu bytes\n", n, cmd.size());
  }
}

std::vector<uint8_t> SpiMaster::read_data(size_t len) {
  std::vector<uint8_t> rx(len);
  size_t total = 0;
  auto start = std::chrono::steady_clock::now();
  const double timeout_sec = 1.0;

  while (total < len) {
    int n = ftdi_->read_data(rx.data() + total, len - total);
    if (n > 0) {
      total += n;
      start = std::chrono::steady_clock::now();  // Reset timeout on progress
    } else if (n < 0) {
      fprintf(stderr, "\n[ERROR] FTDI read error: %d\n", n);
      rx.resize(total);
      return rx;
    } else {
      auto now = std::chrono::steady_clock::now();
      if (std::chrono::duration<double>(now - start).count() > timeout_sec) {
        break;
      }
      usleep(10);
    }
  }
  if (total < len) {
    fprintf(stderr, "\n[WARN] Read timeout: got %zu/%zu bytes\n", total, len);
    rx.resize(total);
  }
  return rx;
}

void SpiMaster::v2_write_lines(uint32_t addr, const uint8_t* data,
                               uint16_t num_beats) {
  if (num_beats == 0) return;
  std::vector<uint8_t> cmd;
  cmd.insert(cmd.end(), {MPSSE_SET_DATA_BITS_LOW, kCsLow, kDirMask});

  uint16_t count_val = num_beats - 1;
  uint8_t header[7] = {0x02,
                       (uint8_t)(addr >> 24),
                       (uint8_t)(addr >> 16),
                       (uint8_t)(addr >> 8),
                       (uint8_t)addr,
                       (uint8_t)(count_val >> 8),
                       (uint8_t)count_val};
  cmd.push_back(MPSSE_DO_WRITE_NVE_MSB);
  cmd.push_back(6);
  cmd.push_back(0);
  for (int i = 0; i < 7; i++) cmd.push_back(header[i]);

  size_t total_bytes = (size_t)num_beats * 16;
  for (size_t off = 0; off < total_bytes;) {
    size_t chunk = (total_bytes - off > 65536) ? 65536 : total_bytes - off;
    cmd.push_back(MPSSE_DO_WRITE_NVE_MSB);
    cmd.push_back((uint8_t)((chunk - 1) & 0xFF));
    cmd.push_back((uint8_t)((chunk - 1) >> 8));
    cmd.insert(cmd.end(), data + off, data + off + chunk);
    off += chunk;
  }

  // Drain and Flush
  cmd.insert(cmd.end(), {MPSSE_DO_WRITE_NVE_MSB, 15, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                         0, 0, 0, 0, 0, 0, 0, 0});
  cmd.insert(cmd.end(), {MPSSE_SET_DATA_BITS_LOW, kCsHigh, kDirMask,
                         MPSSE_READ_DATA_BITS_LOW, MPSSE_SEND_IMMEDIATE});

  send_cmd(cmd);
  uint8_t dummy;
  int retry = 1000;
  while (ftdi_->read_data(&dummy, 1) <= 0 && retry-- > 0) usleep(10);
}

bool SpiMaster::v2_read_lines(uint32_t addr, uint16_t num_beats, uint8_t* out) {
  if (num_beats == 0) return true;
  ftdi_->purge_buffers();

  std::vector<uint8_t> cmd;
  cmd.insert(cmd.end(), {MPSSE_SET_DATA_BITS_LOW, kCsLow, kDirMask});

  uint16_t count_val = num_beats - 1;
  uint8_t header[7] = {0x01,
                       (uint8_t)(addr >> 24),
                       (uint8_t)(addr >> 16),
                       (uint8_t)(addr >> 8),
                       (uint8_t)addr,
                       (uint8_t)(count_val >> 8),
                       (uint8_t)count_val};
  cmd.push_back(MPSSE_DO_WRITE_NVE_MSB);
  cmd.push_back(6);
  cmd.push_back(0);
  for (int i = 0; i < 7; i++) cmd.push_back(header[i]);

  // Multi-beat read: One sync token (0xFE) per beat.
  // Window: num_beats * kBytesPerBeat bytes payload +
  // kInitialLatencyPaddingBytes bytes for initial DDR/latency padding.
  size_t window =
      (size_t)num_beats * kBytesPerBeat + kInitialLatencyPaddingBytes;
  for (size_t off = 0; off < window;) {
    size_t chunk = (window - off > 65536) ? 65536 : window - off;
    cmd.push_back(MPSSE_DO_READ_NVE_MSB);
    cmd.push_back((uint8_t)((chunk - 1) & 0xFF));
    cmd.push_back((uint8_t)((chunk - 1) >> 8));
    off += chunk;
  }

  cmd.insert(cmd.end(), {MPSSE_SET_DATA_BITS_LOW, kCsHigh, kDirMask,
                         MPSSE_SEND_IMMEDIATE});
  send_cmd(cmd);

  usleep(100);
  std::vector<uint8_t> rx = read_data(window);
  if (rx.size() < window) return false;

  // Optimized Bit-level search using a shift register
  uint8_t sr = 0;
  int bits_in_sr = 0;
  size_t rx_idx = 0;

  auto get_next_bit = [&]() -> int {
    if (bits_in_sr == 0) {
      if (rx_idx >= rx.size()) return -1;
      sr = rx[rx_idx++];
      bits_in_sr = 8;
    }
    int bit = (sr >> (bits_in_sr - 1)) & 1;
    bits_in_sr--;
    return bit;
  };

  int current_beat = 0;
  uint32_t sync_reg = 0;
  int bits_scanned = 0;
  int total_bits = static_cast<int>(window) * 8;

  while (current_beat < num_beats && bits_scanned < total_bits) {
    int bit = get_next_bit();
    if (bit == -1) break;
    bits_scanned++;
    sync_reg = (sync_reg << 1) | bit;

    if ((sync_reg & 0xFF) == 0xFE) {
      // Found sync token. Extract 16 bytes (128 bits)
      for (int i = 0; i < 16; i++) {
        uint8_t val = 0;
        for (int k = 0; k < 8; k++) {
          int b = get_next_bit();
          if (b == -1) break;
          bits_scanned++;
          val = (val << 1) | b;
        }
        out[current_beat * 16 + i] = val;
      }
      current_beat++;
      sync_reg = 0;  // Reset sync search for next beat
    }
  }

  return (current_beat == num_beats);
}

bool SpiMaster::v2_read_data(uint32_t addr, size_t len, uint8_t* out) {
  if (len == 0) return true;

  size_t bytes_read = 0;
  while (bytes_read < len) {
    uint32_t current_addr = addr + bytes_read;
    uint32_t aligned_addr = (current_addr / 16) * 16;
    uint32_t offset = current_addr % 16;

    // Calculate how many beats we can read in this chunk.
    // Max beats per SPI transaction is 65535.
    size_t remaining_len = len - bytes_read;
    size_t chunk_beats = (remaining_len + offset + 15) / 16;
    if (chunk_beats > 65535) chunk_beats = 65535;

    size_t chunk_len = (chunk_beats * 16) - offset;
    if (chunk_len > remaining_len) chunk_len = remaining_len;

    std::vector<uint8_t> lines(chunk_beats * 16);
    if (!v2_read_lines(aligned_addr, static_cast<uint16_t>(chunk_beats),
                       lines.data())) {
      return false;
    }
    memcpy(out + bytes_read, lines.data() + offset, chunk_len);
    bytes_read += chunk_len;
  }
  return true;
}

void SpiMaster::v2_write_data(uint32_t addr, const uint8_t* data, size_t len) {
  if (len == 0) return;

  size_t bytes_written = 0;
  while (bytes_written < len) {
    uint32_t current_addr = addr + bytes_written;
    size_t remaining_len = len - bytes_written;

    uint32_t start_aligned_addr = (current_addr / 16) * 16;
    uint32_t offset = current_addr % 16;

    // Max beats per SPI transaction is 65535.
    size_t chunk_beats = 65535;
    size_t max_chunk_bytes = (chunk_beats * 16) - offset;
    size_t current_chunk_len = std::min(remaining_len, max_chunk_bytes);

    // Recalculate chunk_beats based on current_chunk_len
    uint32_t end_addr = current_addr + current_chunk_len;
    uint32_t end_aligned_addr = (end_addr + 15) / 16 * 16;
    chunk_beats = (end_aligned_addr - start_aligned_addr) / 16;

    std::vector<uint8_t> buffer(chunk_beats * 16);

    // If start is unaligned, we need RMW for the first beat
    if (current_addr % 16 != 0) {
      if (!v2_read_lines(start_aligned_addr, 1, buffer.data())) {
        fprintf(stderr, "[ERROR] RMW read failed at 0x%08x\n",
                start_aligned_addr);
        return;
      }
    }

    // If end is unaligned, we need RMW for the last beat
    if (end_addr % 16 != 0 && (chunk_beats > 1 || current_addr % 16 == 0)) {
      if (!v2_read_lines(end_aligned_addr - 16, 1,
                         buffer.data() + (chunk_beats - 1) * 16)) {
        fprintf(stderr, "[ERROR] RMW read failed at 0x%08x\n",
                end_aligned_addr - 16);
        return;
      }
    }

    memcpy(buffer.data() + offset, data + bytes_written, current_chunk_len);
    v2_write_lines(start_aligned_addr, buffer.data(),
                   static_cast<uint16_t>(chunk_beats));

    bytes_written += current_chunk_len;
  }
}

void SpiMaster::write_word(uint32_t addr, uint32_t val) {
  v2_write_data(addr, reinterpret_cast<const uint8_t*>(&val), 4);
}

void SpiMaster::soft_reset() {
  std::vector<uint8_t> cmd;
  // CSB Low
  cmd.insert(cmd.end(), {MPSSE_SET_DATA_BITS_LOW, kCsLow, kDirMask});

  // Write 2 bytes: Opcode 0x03 (RESET) and dummy 0x00 to provide clocks
  cmd.push_back(MPSSE_DO_WRITE_NVE_MSB);
  cmd.push_back(1); // length - 1 = 1
  cmd.push_back(0);
  cmd.push_back(0x03);
  cmd.push_back(0x00);

  // CSB High & Flush
  cmd.insert(cmd.end(), {MPSSE_SET_DATA_BITS_LOW, kCsHigh, kDirMask,
                         MPSSE_SEND_IMMEDIATE});

  send_cmd(cmd);
  usleep(10000); // 10ms settle time
}

void SpiMaster::device_reset() {
  uint8_t reset_cmd1[] = {MPSSE_SET_DATA_BITS_LOW, 0x08, 0x8b};
  ftdi_->write_data(reset_cmd1, 3);
  usleep(10000);  // 10ms reset pulse
  uint8_t reset_cmd2[] = {MPSSE_SET_DATA_BITS_LOW, 0x88, 0x8b};
  ftdi_->write_data(reset_cmd2, 3);
  usleep(10000);  // 10ms settle time
  uint8_t reset_cmd3[] = {MPSSE_SET_DATA_BITS_LOW, 0x08, kDirMask};
  ftdi_->write_data(reset_cmd3, 3);
}