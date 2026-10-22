# DMA Engine

The DMA engine is a single-channel, linked-list descriptor DMA controller that
offloads bulk data movement from the CPU. It connects to the existing TileLink-UL
crossbar as both a host (master, for read/write transactions) and a device
(slave, for CPU programming via CSRs).

Without a DMA engine, all memory transfers (loading model weights from SRAM/DDR
into TCMs, streaming peripheral data) must be performed by the CPU, stalling
execution.

## Architecture

The DMA engine has two TileLink-UL ports:

- **Host port** (128-bit): Issues Get/PutFullData transactions on the crossbar to
  read from source addresses and write to destination addresses.
- **Device port** (32-bit): Accepts CSR read/write transactions from the CPU to
  program and monitor the DMA.

The engine processes a linked list of descriptors stored in memory. Each
descriptor defines a single transfer: source address, destination address,
length, beat size, and optional peripheral flow control parameters. Descriptors
are chained via a `next_desc` pointer; a value of 0 signals end of chain.

### Transfer Modes

| Mode | Source Addr | Dest Addr | Use Case |
|------|-----------|----------|----------|
| Mem→Mem | Incrementing | Incrementing | SRAM↔DDR, SRAM→ITCM/DTCM |
| Mem→Periph | Incrementing | Fixed | SRAM→SPI TX FIFO |
| Periph→Mem | Fixed | Incrementing | I2C RX→SRAM |

### Key Parameters

- **Host port width**: 128-bit (matches crossbar common width)
- **Device port width**: 32-bit (CSR access, like GPIO/SPI)
- **Max transfer per descriptor**: 16 MB (24-bit length field)
- **Outstanding transactions**: 1 (single source ID, read-one-write-one)
- **Interrupt**: None for v1 (CPU polls STATUS register)

## Register Map

Base address: `0x40050000` (4 KB region, after I2C at `0x40040000`)

| Offset | Name | Access | Bits | Description |
|--------|------|--------|------|-------------|
| `0x00` | CTRL | RW | [0] enable, [1] start (W1S, self-clearing), [2] abort | Control |
| `0x04` | STATUS | RO | [0] busy, [1] done, [2] error, [7:4] error_code | Status |
| `0x08` | DESC_ADDR | RW | [31:0] | Address of first descriptor in memory |
| `0x0C` | CUR_DESC | RO | [31:0] | Address of currently executing descriptor |
| `0x10` | XFER_REMAIN | RO | [23:0] | Bytes remaining in current transfer |

### Programming Sequence

```text
1. Build descriptor chain in memory (SRAM or DDR)
2. Write DESC_ADDR with address of first descriptor
3. Write CTRL with enable=1, start=1
4. Poll STATUS.done until set
5. Check STATUS.error
```

## Descriptor Format

Descriptors are 32 bytes (two 128-bit TL-UL beats) and must be 32-byte aligned
in memory. The DMA fetches them via its host port.

```text
Offset  Field         Bits     Description
0x00    src_addr      [31:0]   Source address
0x04    dst_addr      [31:0]   Destination address
0x08    xfer_len      [23:0]   Transfer length in bytes
        xfer_width    [26:24]  Beat size: log2(bytes). 0=1B, 1=2B, 2=4B, 3=8B, 4=16B
        flags         [31:27]  [27] src_fixed, [28] dst_fixed, [29] poll_en, [30:31] reserved
0x0C    next_desc     [31:0]   Address of next descriptor (0 = end of chain)
0x10    poll_addr     [31:0]   Status register address to poll (0 = no polling)
0x14    poll_mask     [31:0]   Bitmask applied to polled value
0x18    poll_value    [31:0]   Expected value after masking
0x1C    reserved      [31:0]
```

## Peripheral Flow Control

Peripherals like SPI master expose status registers (TX Full, RX Empty flags).
The DMA uses **descriptor-level status polling** to pace transfers without
requiring any peripheral modifications.

Each descriptor includes an optional `poll_addr` / `poll_mask` / `poll_value`
triplet. When configured (`poll_en` set and `poll_addr != 0`), the DMA reads
`poll_addr` before each data beat and waits until
`(read_data & poll_mask) == poll_value`.

### Example: DMA → SPI TX

```text
Descriptor:
  src_addr   = 0x20000000  (SRAM buffer)
  dst_addr   = 0x40020008  (SPI TXDATA register)
  dst_fixed  = 1
  poll_addr  = 0x40020000  (SPI STATUS register)
  poll_mask  = 0x00000004  (bit 2 = TX Full)
  poll_value = 0x00000000  (wait until TX not full)
```

The DMA reads SPI STATUS, checks `(status & 0x4) == 0`, and only then reads the
next source byte and writes it to TXDATA. This naturally paces the DMA to the
SPI clock rate.

### Example: I2C RX → DMA

```text
Descriptor:
  src_addr   = 0x40040008  (I2C RXDATA register)
  dst_addr   = 0x20001000  (SRAM buffer)
  src_fixed  = 1
  poll_addr  = 0x40040000  (I2C STATUS register)
  poll_mask  = 0x00000002  (bit 1 = RX available)
  poll_value = 0x00000002  (wait until RX data ready)
```

## State Machine

```text
IDLE ──[start]──► FETCH_DESC_0 ──[d.fire]──► FETCH_DESC_1 ──[d.fire]──► POLL_CHECK
  ▲                                                                          │
  │                                                          [no poll or match]
  │                                                                          ▼
  │                                                                   XFER_READ_REQ
  │                                                                          │
  │                  [poll_en &&                                         [a.fire]
  │                   mismatch]                                              ▼
  │                       │                                          XFER_READ_RESP
  │                  POLL_REQ ◄── POLL_RESP                                  │
  │                       │          ▲  │                                [d.fire]
  │                  [a.fire]        │  │                                     ▼
  │                       ▼          │  [match]                       XFER_WRITE_REQ
  │                  POLL_RESP ──────┘     │                                 │
  │                                        ▼                            [a.fire]
  │                                  XFER_READ_REQ                           ▼
  │                                                                   XFER_WRITE_RESP
  │                                                                          │
  │                                                               [d.fire, remaining>0]
  │                                                                     ──► POLL_CHECK
  │                                                               [d.fire, remaining==0,
  │                                                                next!=0]
  │                                                                     ──► FETCH_DESC_0
  │                                                               [d.fire, remaining==0,
  │                                                                next==0]
  │                                                                          │
  └────────────────────────── DONE ◄─────────────────────────────────────────┘
```

### State Descriptions

- **IDLE**: Waits for `CTRL.start`. Latches `DESC_ADDR`.
- **FETCH_DESC_0**: Issues TL-UL Get (128-bit) for descriptor bytes 0–15
  (src_addr, dst_addr, len/flags, next_desc).
- **FETCH_DESC_1**: Issues TL-UL Get (128-bit) for descriptor bytes 16–31
  (poll_addr, poll_mask, poll_value).
- **POLL_CHECK**: If `poll_en` is set and `poll_addr != 0`, go to POLL_REQ.
  Otherwise skip to XFER_READ_REQ.
- **POLL_REQ**: Issues TL-UL Get (32-bit) at `poll_addr`.
- **POLL_RESP**: Captures D channel data. If `(data & poll_mask) == poll_value`,
  proceed to XFER_READ_REQ. Otherwise loop back to POLL_REQ.
- **XFER_READ_REQ**: Issues TL-UL Get at current source address with configured
  beat size.
- **XFER_READ_RESP**: Captures D channel data into buffer register.
- **XFER_WRITE_REQ**: Issues TL-UL PutFullData with buffered data to dest
  address.
- **XFER_WRITE_RESP**: On D ack, updates addresses (unless fixed) and remaining
  length. If remaining > 0, loop to POLL_CHECK. If remaining == 0 and
  `next_desc != 0`, go to FETCH_DESC_0. Otherwise DONE.
- **DONE**: Sets `STATUS.done`, returns to IDLE.

Abort from any state transitions to IDLE with error flag set.
TL-UL D channel error transitions to DONE with error code.

## TileLink Host Interface

Single 128-bit TL-UL master port. Generates:

- **Get** (read): opcode=4, size=`xfer_width`, address=`src_addr`, mask=all-ones
  for size
- **PutFullData** (write): opcode=0, size=`xfer_width`, address=`dst_addr`,
  data=buffer
- **Poll Get**: opcode=4, size=2 (32-bit), address=`poll_addr`
- **Descriptor Get**: opcode=4, size=4 (16 bytes), address=`desc_addr` /
  `desc_addr+16`

Source ID always 0 (single outstanding).

The host A channel is shared between descriptor fetch, poll reads, data reads,
and data writes. The FSM drives a mux selecting the appropriate
opcode/address/data/size based on current state.

## TileLink Device Interface

Follows the GPIO pattern (`hdl/chisel/src/bus/GPIO.scala`):

- `tl_a.ready := !tl_d_valid`
- On `tl_a.fire`: decode `address[11:0]`, read/write CSRs
- Start bit triggers state machine kick

## Crossbar Integration

### Address Map

The DMA occupies `0x40050000–0x40050FFF` (4 KB), after I2C at `0x40040000`.

### Host Connectivity

The DMA host port connects to all memory and peripheral devices it may need to
access:

```scala
"dma" -> Seq("sram", "coralnpu_device", "rom", "ddr_ctrl", "ddr_mem",
             "spi_master", "gpio", "i2c_master", "uart0", "uart1")
```

The CPU must also be able to program the DMA:

```scala
"coralnpu_core" -> Seq(...existing..., "dma")
```

## Implementation

Single new file: `hdl/chisel/src/bus/DmaEngine.scala`

Configuration changes in:

- `hdl/chisel/src/soc/CrossbarConfig.scala` — host, device, address range,
  connections
- `hdl/chisel/src/soc/SoCChiselConfig.scala` — `DmaParameters`, module config
- `hdl/chisel/src/soc/CoralNPUChiselSubsystem.scala` — instantiation case

### Module IO

```scala
class DmaEngine(hostParams: Parameters, deviceParams: Parameters) extends Module {
  val hostTlulP = new TLULParameters(hostParams)
  val deviceTlulP = new TLULParameters(deviceParams)
  val io = IO(new Bundle {
    val tl_host   = new OpenTitanTileLink.Host2Device(hostTlulP)
    val tl_device = Flipped(new OpenTitanTileLink.Host2Device(deviceTlulP))
  })
}
```

### Internal Structure

- **CSR register file**: Regs for CTRL, STATUS, DESC_ADDR, following GPIO pattern
- **Descriptor latch**: Registers for all descriptor fields, loaded during FETCH
  states
- **Data buffer**: 128-bit register for read-then-write pipeline
- **Address counters**: Current src/dst addresses, remaining byte count
- **FSM**: ChiselEnum with states listed above
- **Integrity**: `RequestIntegrityGen` for host A channel,
  `ResponseIntegrityGen` for device D channel
