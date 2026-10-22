#ifndef SPI_MASTER_H
#define SPI_MASTER_H

#include <cstddef>
#include <cstdint>
#include <vector>

constexpr uint8_t MPSSE_SET_DATA_BITS_LOW = 0x80;
constexpr uint8_t MPSSE_SET_DATA_BITS_HIGH = 0x82;
constexpr uint8_t MPSSE_READ_DATA_BITS_LOW = 0x81;
constexpr uint8_t MPSSE_READ_DATA_BITS_HIGH = 0x83;
constexpr uint8_t MPSSE_LOOPBACK_ENABLE = 0x84;
constexpr uint8_t MPSSE_LOOPBACK_DISABLE = 0x85;
constexpr uint8_t MPSSE_SET_TCK_DIVISOR = 0x86;
constexpr uint8_t MPSSE_SEND_IMMEDIATE = 0x87;
constexpr uint8_t MPSSE_DISABLE_CLK_DIV_5 = 0x8a;
constexpr uint8_t MPSSE_ENABLE_CLK_DIV_5 = 0x8b;
constexpr uint8_t MPSSE_DISABLE_3PHASE_CLK = 0x8c;
constexpr uint8_t MPSSE_ENABLE_3PHASE_CLK = 0x8d;

constexpr uint8_t MPSSE_DO_WRITE_NVE_MSB = 0x11;
constexpr uint8_t MPSSE_DO_READ_NVE_MSB = 0x24;

constexpr uint8_t kDirMask = 0x0b;
constexpr uint8_t kCsHigh = 0x08;
constexpr uint8_t kCsLow = 0x00;

class FtdiInterface {
 public:
  virtual ~FtdiInterface() = default;
  virtual int write_data(const uint8_t* buf, int size) = 0;
  virtual int read_data(uint8_t* buf, int size) = 0;
  virtual int purge_buffers() = 0;
};

class SpiMaster {
  FtdiInterface* ftdi_;

  void send_cmd(const std::vector<uint8_t>& cmd);
  std::vector<uint8_t> read_data(size_t len);

 public:
  SpiMaster(FtdiInterface* ftdi) : ftdi_(ftdi) {}

  void v2_write_lines(uint32_t addr, const uint8_t* data, uint16_t num_beats);
  static constexpr size_t kBytesPerBeat = 17;
  static constexpr size_t kInitialLatencyPaddingBytes = 2048;

  bool v2_read_lines(uint32_t addr, uint16_t num_beats, uint8_t* buf);
  void v2_write_data(uint32_t addr, const uint8_t* data, size_t len);
  bool v2_read_data(uint32_t addr, size_t len, uint8_t* out);
  void write_word(uint32_t addr, uint32_t val);
  void soft_reset();
  void device_reset();
};

#endif
