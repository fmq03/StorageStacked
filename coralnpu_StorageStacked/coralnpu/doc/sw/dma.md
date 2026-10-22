# DMA Engine Software Guide

## Overview

The CoralNPU DMA engine offloads bulk data transfers from the CPU. It supports
memory-to-memory, memory-to-peripheral, and peripheral-to-memory transfers
using descriptor chains in memory. The CPU builds one or more descriptors,
programs the DMA, and polls for completion.

## Register Map

Base address: `0x40050000`

| Offset | Name        | Access | Description |
|--------|-------------|--------|-------------|
| 0x00   | CTRL        | RW     | `[0]` enable, `[1]` start (write-1-to-set, self-clearing), `[2]` abort |
| 0x04   | STATUS      | RO     | `[0]` busy, `[1]` done, `[2]` error, `[7:4]` error_code |
| 0x08   | DESC_ADDR   | RW     | Address of first descriptor |
| 0x0C   | CUR_DESC    | RO     | Address of current descriptor |
| 0x10   | XFER_REMAIN | RO     | Bytes remaining in current transfer |

### STATUS Error Codes

| Code | Meaning |
|------|---------|
| 0    | No error |
| 1    | Descriptor fetch error |
| 2    | Poll read error |
| 3    | Data read error |
| 4    | Data write error |
| 5    | Abort |

## Descriptor Format

Descriptors must be **32-byte aligned** and reside in memory accessible to the
DMA (SRAM or DDR). Each descriptor is 32 bytes:

```text
Offset  Field        Bits      Description
0x00    src_addr     [31:0]    Source address
0x04    dst_addr     [31:0]    Destination address
0x08    len_flags    [23:0]    Transfer length in bytes
                     [26:24]   Beat width: log2(bytes) — 0=1B, 1=2B, 2=4B
                     [27]      src_fixed: source address does not increment
                     [28]      dst_fixed: destination address does not increment
                     [29]      poll_en: enable flow-control polling
                     [31:30]   Reserved
0x0C    next_desc    [31:0]    Next descriptor address (0 = end of chain)
0x10    poll_addr    [31:0]    Address to poll before each beat
0x14    poll_mask    [31:0]    Bitmask for poll comparison
0x18    poll_value   [31:0]    Expected value after masking
0x1C    reserved     [31:0]    Must be 0
```

### C Descriptor Structure

```c
struct __attribute__((packed, aligned(32))) dma_descriptor {
  uint32_t src_addr;
  uint32_t dst_addr;
  uint32_t len_flags;
  uint32_t next_desc;
  uint32_t poll_addr;
  uint32_t poll_mask;
  uint32_t poll_value;
  uint32_t reserved;
};
```

### Building len_flags

```c
static inline uint32_t make_len_flags(uint32_t len, uint32_t width_log2,
                                       int src_fixed, int dst_fixed,
                                       int poll_en) {
  return (len & 0xFFFFFF) | ((width_log2 & 0x7) << 24) |
         ((src_fixed ? 1u : 0u) << 27) | ((dst_fixed ? 1u : 0u) << 28) |
         ((poll_en ? 1u : 0u) << 29);
}
```

## Programming Sequence

```c
#define REG32(addr) (*(volatile uint32_t*)(addr))
#define DMA_BASE       0x40050000
#define DMA_CTRL       (DMA_BASE + 0x00)
#define DMA_STATUS     (DMA_BASE + 0x04)
#define DMA_DESC_ADDR  (DMA_BASE + 0x08)

// 1. Build descriptor(s) in memory
desc->src_addr  = src;
desc->dst_addr  = dst;
desc->len_flags = make_len_flags(nbytes, 2 /* 4-byte beats */, 0, 0, 0);
desc->next_desc = 0;  // single descriptor

// 2. Program and start DMA
REG32(DMA_DESC_ADDR) = (uint32_t)desc;
REG32(DMA_CTRL) = 0x3;  // enable + start

// 3. Wait for completion
while (!(REG32(DMA_STATUS) & 0x2)) {}  // poll done bit

// 4. Check for errors
if (REG32(DMA_STATUS) & 0x4) {
  // error occurred, check error_code in bits [7:4]
}
```

## Transfer Modes

### Memory-to-Memory

Standard bulk copy. Both source and destination addresses increment.

```c
desc->len_flags = make_len_flags(nbytes, 2, 0, 0, 0);
```

### Memory-to-Peripheral (Fixed Destination)

Source increments, destination stays fixed. Useful for writing to a peripheral
FIFO or data register.

```c
desc->src_addr  = (uint32_t)sram_buffer;
desc->dst_addr  = 0x40020008;  // e.g., SPI TXDATA register
desc->len_flags = make_len_flags(nbytes, 2, 0, 1, 0);  // dst_fixed=1
```

### Peripheral-to-Memory (Fixed Source)

Source stays fixed, destination increments. Useful for draining a peripheral
receive register into a buffer.

```c
desc->src_addr  = 0x40040008;  // e.g., I2C RXDATA register
desc->dst_addr  = (uint32_t)sram_buffer;
desc->len_flags = make_len_flags(nbytes, 2, 1, 0, 0);  // src_fixed=1
```

## Descriptor Chaining

Multiple descriptors can be linked via `next_desc`. The DMA automatically
fetches and executes each descriptor in sequence. Set `next_desc = 0` on the
last descriptor.

```c
desc0->next_desc = (uint32_t)desc1;
desc1->next_desc = 0;  // end of chain

REG32(DMA_DESC_ADDR) = (uint32_t)desc0;
REG32(DMA_CTRL) = 0x3;
```

STATUS.done is set only after the entire chain completes.

## Flow Control with Polling

For peripheral transfers, the DMA can poll a status register before each data
beat to avoid overrunning a FIFO. Set `poll_en=1` and configure the poll
fields:

```c
// Wait until SPI TX FIFO is not full before each write
desc->len_flags = make_len_flags(nbytes, 0, 0, 1, 1);  // poll_en=1, dst_fixed=1
desc->poll_addr  = 0x40020000;  // SPI STATUS register
desc->poll_mask  = 0x00000004;  // bit 2 = TX Full
desc->poll_value = 0x00000000;  // proceed when TX not full
```

The DMA reads `poll_addr` and checks `(read_data & poll_mask) == poll_value`.
If the condition is not met, it retries until it is. This provides
hardware-managed flow control without modifying peripheral designs.

## Aborting a Transfer

Write bit 2 of CTRL to abort an in-progress transfer:

```c
REG32(DMA_CTRL) = 0x4;  // abort
```

After abort, STATUS will show `done=1, error=1, error_code=5`.

## Constraints

- Descriptors must be 32-byte aligned
- Maximum transfer per descriptor: 16 MB (24-bit length field)
- Beat width must not exceed the bus width (128 bits / 16 bytes)
- The DMA issues one outstanding transaction at a time (no burst pipelining)
- No interrupt support; use polling on STATUS.done
