#include <gtest/gtest.h>

#include <vector>

#include "spi_master.h"

class MockFtdi : public FtdiInterface {
 public:
  std::vector<uint8_t> written;
  std::vector<uint8_t> to_read;
  size_t read_idx = 0;

  int write_data(const uint8_t* buf, int size) override {
    written.insert(written.end(), buf, buf + size);
    return size;
  }

  int read_data(uint8_t* buf, int size) override {
    int n = 0;
    while (n < size && read_idx < to_read.size()) {
      buf[n++] = to_read[read_idx++];
    }
    return n;
  }

  int purge_buffers() override {
    // Keep written commands for verification in tests that span multiple calls.
    return 0;
  }
};

TEST(NexusLoaderTest, WriteLines) {
  MockFtdi mock;
  SpiMaster spi(&mock);
  uint8_t data[16] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
                      0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00};

  // Simulate read returning dummy byte for the drain wait loop
  mock.to_read.push_back(0xFF);

  spi.v2_write_lines(0x1000, data, 1);

  // Assert header presence
  ASSERT_GT(mock.written.size(), 20u);
  EXPECT_EQ(mock.written[0], 0x80);  // CS Low
}

TEST(NexusLoaderTest, ReadLines) {
  MockFtdi mock;
  SpiMaster spi(&mock);

  size_t window =
      1 * SpiMaster::kBytesPerBeat + SpiMaster::kInitialLatencyPaddingBytes;
  mock.to_read.resize(window, 0);
  mock.to_read[5] = 0xFE;  // Sync token

  for (int i = 0; i < 16; i++) {
    mock.to_read[6 + i] = i + 1;  // 1, 2, 3... 16
  }

  uint8_t out[16] = {0};
  bool res = spi.v2_read_lines(0x2000, 1, out);

  EXPECT_TRUE(res);
  EXPECT_EQ(out[0], 1);
  EXPECT_EQ(out[15], 16);
}

TEST(NexusLoaderTest, ReadLinesMultiBeat) {
  MockFtdi mock;
  SpiMaster spi(&mock);

  // Total window size in new implementation is num_beats * kBytesPerBeat +
  // kInitialLatencyPaddingBytes
  size_t window =
      2 * SpiMaster::kBytesPerBeat + SpiMaster::kInitialLatencyPaddingBytes;
  mock.to_read.resize(window, 0);

  // Beat 0: sync at 100
  mock.to_read[100] = 0xFE;
  for (int i = 0; i < 16; i++) mock.to_read[101 + i] = 0xA0 + i;

  // Beat 1: sync at 200 (must be after beat 0's data)
  mock.to_read[200] = 0xFE;
  for (int i = 0; i < 16; i++) mock.to_read[201 + i] = 0xB0 + i;

  uint8_t out[32] = {0};
  bool res = spi.v2_read_lines(0x3000, 2, out);

  EXPECT_TRUE(res);
  EXPECT_EQ(out[0], 0xA0);
  EXPECT_EQ(out[15], 0xAF);
  EXPECT_EQ(out[16], 0xB0);
  EXPECT_EQ(out[31], 0xBF);
}

TEST(NexusLoaderTest, WriteWord) {
  MockFtdi mock;
  SpiMaster spi(&mock);

  // write_word does RMW. First it reads 16 bytes.
  // window = 1 * 17 + 2048 = 2065
  size_t window =
      1 * SpiMaster::kBytesPerBeat + SpiMaster::kInitialLatencyPaddingBytes;
  mock.to_read.resize(window, 0xCC);
  mock.to_read[500] = 0xFE;  // sync token

  // For the v2_write_lines drain loop
  mock.to_read.push_back(0xFF);

  spi.write_word(0x4000, 0x12345678);

  // mock.written will have:
  // 1. Read command (v2_read_lines)
  // 2. Write command (v2_write_lines)
  // Search for the write command (Op=0x02)
  size_t write_cmd_start = 0;
  bool found = false;
  for (size_t i = 0; i < mock.written.size() - 7; ++i) {
    if (mock.written[i] == 0x80 && mock.written[i + 6] == 0x02) {
      write_cmd_start = i;
      found = true;
    }
  }

  ASSERT_TRUE(found);
  // Payload is at write_cmd_start + 16
  ASSERT_GT(mock.written.size(), write_cmd_start + 16u + 4u);
  EXPECT_EQ(mock.written[write_cmd_start + 16], 0x78);
  EXPECT_EQ(mock.written[write_cmd_start + 17], 0x56);
  EXPECT_EQ(mock.written[write_cmd_start + 18], 0x34);
  EXPECT_EQ(mock.written[write_cmd_start + 19], 0x12);
  // Rest of the 16-byte line should still be 0xCC
  EXPECT_EQ(mock.written[write_cmd_start + 20], 0xCC);
}

TEST(NexusLoaderTest, WriteDataUnaligned) {
  MockFtdi mock;
  SpiMaster spi(&mock);

  // Address 0x1004, length 8. Bounds: 0x1000 to 0x1010 (1 beat)
  // RMW will read 0x1000
  size_t window =
      1 * SpiMaster::kBytesPerBeat + SpiMaster::kInitialLatencyPaddingBytes;
  mock.to_read.resize(window, 0xCC);
  mock.to_read[500] = 0xFE;      // sync token
  mock.to_read.push_back(0xFF);  // drain loop

  uint8_t payload[8] = {1, 2, 3, 4, 5, 6, 7, 8};
  spi.v2_write_data(0x1004, payload, 8);

  // Find write command
  size_t write_cmd_start = 0;
  bool found = false;
  for (size_t i = 0; i < mock.written.size() - 7; ++i) {
    if (mock.written[i] == 0x80 && mock.written[i + 6] == 0x02) {
      write_cmd_start = i;
      found = true;
    }
  }
  ASSERT_TRUE(found);
  // Beat data starts at write_cmd_start + 16
  EXPECT_EQ(mock.written[write_cmd_start + 16 + 0], 0xCC);
  EXPECT_EQ(mock.written[write_cmd_start + 16 + 3], 0xCC);
  EXPECT_EQ(mock.written[write_cmd_start + 16 + 4], 1);
  EXPECT_EQ(mock.written[write_cmd_start + 16 + 11], 8);
  EXPECT_EQ(mock.written[write_cmd_start + 16 + 12], 0xCC);
}

TEST(NexusLoaderTest, ReadDataUnaligned) {
  MockFtdi mock;
  SpiMaster spi(&mock);

  // Address 0x1004, length 8. Bounds: 0x1000 to 0x1010 (1 beat)
  size_t window =
      1 * SpiMaster::kBytesPerBeat + SpiMaster::kInitialLatencyPaddingBytes;
  mock.to_read.resize(window, 0xCC);
  mock.to_read[500] = 0xFE;  // sync token
  for (int i = 0; i < 16; i++) {
    mock.to_read[501 + i] = i;  // Data in line is 0, 1, 2, ... 15
  }

  uint8_t out[8] = {0};
  bool res = spi.v2_read_data(0x1004, 8, out);

  EXPECT_TRUE(res);
  EXPECT_EQ(out[0], 4);
  EXPECT_EQ(out[7], 11);
}

TEST(NexusLoaderTest, ReadLinesTimeout) {
  MockFtdi mock;
  SpiMaster spi(&mock);

  // Return fewer bytes than window size (window = 1*17 + 2048 = 2065)
  mock.to_read.resize(100, 0);

  uint8_t out[16] = {0};
  bool res = spi.v2_read_lines(0x2000, 1, out);

  EXPECT_FALSE(res);
}

TEST(NexusLoaderTest, ReadLinesMissingSync) {
  MockFtdi mock;
  SpiMaster spi(&mock);

  // Return full window size but no 0xFE sync token
  size_t window =
      1 * SpiMaster::kBytesPerBeat + SpiMaster::kInitialLatencyPaddingBytes;
  mock.to_read.resize(window, 0xCC);

  uint8_t out[16] = {0};
  bool res = spi.v2_read_lines(0x2000, 1, out);

  EXPECT_FALSE(res);
}

TEST(NexusLoaderTest, LargeWriteDataChunking) {
  MockFtdi mock;
  SpiMaster spi(&mock);

  // 1MB is 65536 beats. Let's write 1MB + 16 bytes (65537 beats)
  size_t len = (65535 + 2) * 16;
  std::vector<uint8_t> payload(len, 0xDD);

  // Simulation setup for multiple transactions
  // Each write transaction waits for a dummy byte in the drain loop.
  mock.to_read.push_back(0xFF);
  mock.to_read.push_back(0xFF);

  spi.v2_write_data(0x1000, payload.data(), len);

  // We expect two v2_write_lines calls (one for 65535 beats, one for 2 beats)
  // Each call has a header (7 bytes) + data + footer.
  // Count how many times we see the v2 write header (0x02 as 7th byte of an
  // MPSSE write cmd)
  int write_cmd_count = 0;
  for (size_t i = 0; i < mock.written.size() - 7; ++i) {
    if (mock.written[i] == 0x80 && mock.written[i + 6] == 0x02) {
      write_cmd_count++;
    }
  }
  EXPECT_EQ(write_cmd_count, 2);
}

TEST(NexusLoaderTest, DeviceReset) {
  MockFtdi mock;
  SpiMaster spi(&mock);
  spi.device_reset();

  ASSERT_EQ(mock.written.size(), 9u);
  EXPECT_EQ(mock.written[0], 0x80);
  EXPECT_EQ(mock.written[1], 0x08);
  EXPECT_EQ(mock.written[2], 0x8b);

  EXPECT_EQ(mock.written[3], 0x80);
  EXPECT_EQ(mock.written[4], 0x88);
  EXPECT_EQ(mock.written[5], 0x8b);

  EXPECT_EQ(mock.written[6], 0x80);
  EXPECT_EQ(mock.written[7], 0x08);
  EXPECT_EQ(mock.written[8], 0x0b);
}
