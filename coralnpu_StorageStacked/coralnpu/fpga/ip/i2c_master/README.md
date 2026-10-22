# I2C Master

This directory contains the I2C Master IP block. It implements a standard I2C Master with a TileLink-UL (TL-UL) interface.

## Registers

| Offset | Name        | Description                                       |
|--------|-------------|---------------------------------------------------|
| 0x000  | INTR_STATE  | Interrupt state register (W1C). Bit 0 is `tx_idle`|
| 0x004  | INTR_ENABLE | Interrupt enable register.                        |
| 0x008  | CTRL        | Control register. Bit 0 enables the I2C Master.   |
| 0x00C  | STATUS      | Status register.                                  |
| 0x010  | FDATA       | FIFO Data and Command register.                   |
| 0x014  | FIFO_CTRL   | FIFO control register (currently reserved).       |
| 0x018  | CLK_DIV     | Clock divider. Specifies half-period of I2C clk.  |

### STATUS Register (0x00C)

* **Bit 0**: `busy` - I2C FSM is not idle.
* **Bit 1**: `!fifo_empty` - TX FIFO is not empty.
* **Bit 2**: `rx_fifo_valid` - RX FIFO has valid data.

### FDATA Register (0x010)

Writes to this register push a command and data to the TX FIFO.
Reads from this register pop data from the RX FIFO.

**Write Format:**

* **Bits 7:0**: Data to transmit.
* **Bit 8**: `START` - Issue a START (or Repeated START) condition before sending the byte.
* **Bit 9**: `STOP` - Issue a STOP condition after sending/receiving the byte.
* **Bit 10**: `READ` - Perform an I2C Read instead of a Write.

**Read Format:**

* **Bits 7:0**: Received data.

## Programming Model

1. Configure the `CLK_DIV` register.
   The I2C bit period is 4 × `CLK_DIV` system clock cycles.
2. Set bit 0 of the `CTRL` register to enable the I2C Master.

### Example: I2C Write

To write data `0xDE` to register `0x02` of a slave at address `0x55`:

1. Write `FDATA` with `START`=1, `Data`=`(0x55 << 1) | 0`.
2. Write `FDATA` with `START`=0, `Data`=`0x02`.
3. Write `FDATA` with `STOP`=1, `Data`=`0xDE`.

### Example: I2C Read

To read a register `0x02` from a slave at address `0x55`:

1. Write `FDATA` with `START`=1, `Data`=`(0x55 << 1) | 0`.
2. Write `FDATA` with `START`=0, `Data`=`0x02`.
3. Write `FDATA` with `START`=1, `Data`=`(0x55 << 1) | 1` (Repeated Start, Read).
4. Write `FDATA` with `READ`=1, `STOP`=1.
5. Wait for the transaction to complete (check `STATUS` busy/fifo_empty bits).
6. Read `FDATA` to get the received byte.
