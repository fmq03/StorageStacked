// Copyright 2026 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

package bus

import chisel3._
import chisel3.util._
import chisel3.simulator.scalatest.ChiselSim
import coralnpu.Parameters
import org.scalatest.freespec.AnyFreeSpec

// ---------------------------------------------------------------------------
// Test wrapper module — replicates spi_v2_tb_top.sv in Chisel
// ---------------------------------------------------------------------------

class Spi2TLULV2TestWrapper extends Module {
  val io = IO(new Bundle {
    val spi_clk_in = Input(Bool())
    val spi_csb    = Input(Bool())
    val spi_mosi   = Input(Bool())
    val spi_miso   = Output(Bool())

    // TL-A capture outputs (last beat)
    val tl_a_captured    = Output(Bool())
    val tl_a_cap_opcode  = Output(UInt(3.W))
    val tl_a_cap_address = Output(UInt(32.W))
    val tl_a_cap_data    = Output(UInt(128.W))
    val tl_a_cap_count   = Output(UInt(32.W))

    // TL-A first-beat capture + reset
    val tl_a_first_address = Output(UInt(32.W))
    val tl_a_first_data    = Output(UInt(128.W))
    val tl_a_first_reset   = Input(Bool())
  })

  var p = new Parameters()
  p.lsuDataBits = 128
  val tlul_p = p.toTLUL()
  val v2     = Module(new Spi2TLULV2(tlul_p))

  // Clock and reset wiring
  v2.io.spi_clk   := io.spi_clk_in.asClock
  v2.io.spi_rst_n := !io.spi_csb

  // MOSI: always valid, data from spi_mosi
  v2.io.q_mosi_pin.valid := true.B
  v2.io.q_mosi_pin.bits  := io.spi_mosi

  // MISO: always ready, output gated by valid
  v2.io.q_miso_pin.ready := true.B
  io.spi_miso            := Mux(v2.io.q_miso_pin.valid, v2.io.q_miso_pin.bits, false.B)

  // -------------------------------------------------------------------------
  // TileLink auto-responder (mirrors spi_v2_tb_top.sv)
  // -------------------------------------------------------------------------
  // V2 TL-A is 217 bits: Cat(opcode(3), param(3), size(4), source(8),
  //   addr(32), mask(16), data(128), user(23))
  // V2 TL-D is 162 bits: Cat(opcode(3), param(3), size(4), source(8),
  //   sink(1), data(128), user(14), error(1))
  // -------------------------------------------------------------------------
  val TL_GET             = 4.U(3.W)
  val TL_ACCESS_ACK      = 0.U(3.W)
  val TL_ACCESS_ACK_DATA = 1.U(3.W)

  val respPending = RegInit(false.B)
  val respReq     = RegInit(0.U.asTypeOf(new OpenTitanTileLink.A_Channel(tlul_p)))
  val tlAReady    = RegInit(true.B)

  v2.io.q_tl_a.ready := tlAReady

  val aValid = v2.io.q_tl_a.valid
  val aData  = v2.io.q_tl_a.bits

  // Single-driver style: accept A → latch + deassert ready; D handshake → clear + reassert
  val acceptA   = aValid && tlAReady && !respPending
  val completeD = respPending && v2.io.q_tl_d.ready

  respPending := Mux(acceptA, true.B, Mux(completeD, false.B, respPending))
  respReq     := Mux(acceptA, aData, respReq)
  tlAReady    := Mux(acceptA, false.B, Mux(completeD, true.B, tlAReady))

  // Decode latched request (217-bit positions)
  val reqOpcode = respReq.opcode
  val reqParam  = respReq.param
  val reqSize   = respReq.size
  val reqSource = respReq.source
  val reqAddr   = respReq.address

  val respOpcode =
    Mux(reqOpcode === TLULOpcodesA.Get.asUInt, TLULOpcodesD.AccessAckData, TLULOpcodesD.AccessAck)
  val respData = Cat(0.U(96.W), reqAddr)

  v2.io.q_tl_d.valid       := respPending
  v2.io.q_tl_d.bits.opcode := respOpcode.asUInt
  v2.io.q_tl_d.bits.param  := 0.U(3.W)
  v2.io.q_tl_d.bits.size   := reqSize
  v2.io.q_tl_d.bits.source := reqSource
  v2.io.q_tl_d.bits.sink   := 0.U(1.W)
  v2.io.q_tl_d.bits.data   := respData
  v2.io.q_tl_d.bits.user   := 0.U.asTypeOf(new OpenTitanTileLink_D_User)
  v2.io.q_tl_d.bits.error  := 0.U(1.W)

  // -------------------------------------------------------------------------
  // TL-A capture registers (latch on A-channel handshake)
  // -------------------------------------------------------------------------
  val aHandshake = aValid && tlAReady
  val captured   = RegInit(false.B)
  val capOpcode  = RegInit(0.U(3.W))
  val capAddress = RegInit(0.U(32.W))
  val capData    = RegInit(0.U(128.W))
  val capCount   = RegInit(0.U(32.W))

  // First-beat capture: latched on the first handshake after count was last
  // snapshot by the test (i.e., the first beat of each multi-beat burst).
  val firstCapAddress = RegInit(0.U(32.W))
  val firstCapData    = RegInit(0.U(128.W))
  val firstCapDone    = RegInit(false.B)

  captured   := Mux(aHandshake, true.B, captured)
  capOpcode  := Mux(aHandshake, aData.opcode, capOpcode)
  capAddress := Mux(aHandshake, aData.address, capAddress)
  capData    := Mux(aHandshake, aData.data, capData)
  capCount   := Mux(aHandshake, capCount + 1.U, capCount)

  when(io.tl_a_first_reset) {
    firstCapDone    := false.B
    firstCapAddress := 0.U
    firstCapData    := 0.U
  }.elsewhen(aHandshake && !firstCapDone) {
    firstCapAddress := aData.address
    firstCapData    := aData.data
    firstCapDone    := true.B
  }

  io.tl_a_captured      := captured
  io.tl_a_cap_opcode    := capOpcode
  io.tl_a_cap_address   := capAddress
  io.tl_a_cap_data      := capData
  io.tl_a_cap_count     := capCount
  io.tl_a_first_address := firstCapAddress
  io.tl_a_first_data    := firstCapData
}

// ---------------------------------------------------------------------------
// Test suite — ports all 6 C++ tests
// ---------------------------------------------------------------------------

class Spi2TLULV2Spec extends AnyFreeSpec with ChiselSim {

  // Sys clock period = 10 units (SYS_HALF=5), SPI period = 13 units (SPI_HALF=6.5).
  // We use integer ratio: SYS_HALF=10, SPI_HALF=13.
  // Each clock.step(1) is one "sim unit". SPI toggles when the phase counter says so.
  val SYS_HALF = 10
  val SPI_HALF = 13

  /** Run the simulation with dual-clock stepping. The main clock.step() drives the sys domain. SPI
    * clock is toggled as a Bool input at a 10:13 ratio to approximate truly async clocks.
    */
  def withDualClock(body: Spi2TLULV2TestWrapper => Unit): Unit = {
    simulate(new Spi2TLULV2TestWrapper) { dut =>
      // Reset — toggle SPI clock during reset so system-reset-only
      // CDC FIFOs (clocked by spi_clk) see the reset posedge.
      dut.reset.poke(true.B)
      dut.io.spi_csb.poke(true.B)
      dut.io.spi_mosi.poke(false.B)
      dut.io.spi_clk_in.poke(false.B)
      dut.io.tl_a_first_reset.poke(false.B)
      for (_ <- 0 until 5) {
        dut.io.spi_clk_in.poke(true.B)
        dut.clock.step(1)
        dut.io.spi_clk_in.poke(false.B)
        dut.clock.step(1)
      }
      dut.reset.poke(false.B)
      dut.clock.step(5)

      body(dut)
    }
  }

  /** Dual-clock tick state — tracks when to toggle SPI clock relative to sys clock. */
  class DualClockState(dut: Spi2TLULV2TestWrapper) {
    var sysTime: Long     = 0
    var nextSysEdge: Long = SYS_HALF
    var nextSpiEdge: Long = SPI_HALF
    var spiLevel: Boolean = false

    /** Advance one sim time unit, toggling whichever clock(s) fire. Returns 's', 'p', or 'b'. */
    def tick(): Char = {
      if (nextSysEdge < nextSpiEdge) {
        sysTime = nextSysEdge
        nextSysEdge += SYS_HALF
        dut.clock.step(1)
        's'
      } else if (nextSpiEdge < nextSysEdge) {
        sysTime = nextSpiEdge
        spiLevel = !spiLevel
        dut.io.spi_clk_in.poke(spiLevel.B)
        nextSpiEdge += SPI_HALF
        // Don't step sys clock — just toggle SPI
        // But we need at least one eval to propagate. Step sys clock to advance simulation.
        dut.clock.step(1)
        'p'
      } else {
        // Both edges coincide
        sysTime = nextSysEdge
        spiLevel = !spiLevel
        dut.io.spi_clk_in.poke(spiLevel.B)
        nextSysEdge += SYS_HALF
        nextSpiEdge += SPI_HALF
        dut.clock.step(1)
        'b'
      }
    }

    /** Advance until n SPI posedges have occurred. */
    def spiCycles(n: Int): Unit = {
      var count = 0
      while (count < n) {
        val w = tick()
        if ((w == 'p' || w == 'b') && spiLevel) count += 1
      }
    }

    /** Advance until n sys posedges have occurred. */
    def bothCycles(n: Int): Unit = {
      var count = 0
      while (count < n) {
        val w = tick()
        if (w == 's' || w == 'b') count += 1
      }
    }

    // SPI protocol helpers

    def spiXferByte(tx: Int): Unit = {
      for (bit <- 7 to 0 by -1) {
        dut.io.spi_mosi.poke((((tx >> bit) & 1) != 0).B)
        spiCycles(1)
      }
    }

    def spiRecvByte(): Int = {
      var rx = 0
      for (_ <- 7 to 0 by -1) {
        dut.io.spi_mosi.poke(false.B)
        spiCycles(1)
        rx = (rx << 1) | (if (dut.io.spi_miso.peek().litValue != 0) 1 else 0)
      }
      rx
    }

    def csbAssert(): Unit = {
      dut.io.spi_csb.poke(false.B)
      dut.io.spi_mosi.poke(false.B)
    }

    def csbDeassert(): Unit = {
      dut.io.spi_mosi.poke(false.B)
      // Let DmaEngine finish draining CDC FIFOs before CSB deasserts
      // (AsyncQueue safe mode coordinates reset on CSB deassert)
      bothCycles(500)
      dut.io.spi_csb.poke(true.B)
      bothCycles(50)
    }

    def spiSendHeader(op: Int, addr: Long, len: Int): Unit = {
      spiXferByte(op & 0xff)
      spiXferByte(((addr >> 24) & 0xff).toInt)
      spiXferByte(((addr >> 16) & 0xff).toInt)
      spiXferByte(((addr >> 8) & 0xff).toInt)
      spiXferByte((addr & 0xff).toInt)
      spiXferByte((len >> 8) & 0xff)
      spiXferByte(len & 0xff)
    }

    /** Collect n raw MISO bits. Returns array of bytes packed MSB-first per byte. */
    def collectMisoBits(nbits: Int): Array[Int] = {
      val buf = new Array[Int]((nbits + 7) / 8)
      for (i <- 0 until nbits) {
        dut.io.spi_mosi.poke(false.B)
        spiCycles(1)
        val byteIdx = i / 8
        val bitIdx  = 7 - (i % 8)
        if (dut.io.spi_miso.peek().litValue != 0) {
          buf(byteIdx) |= (1 << bitIdx)
        }
      }
      buf
    }

    /** Wait for TL-A capture count to reach expected value. */
    def waitTlACapture(expectedCount: Long, timeoutSys: Int = 200): Boolean = {
      var sc = 0
      while (sc < timeoutSys) {
        val w = tick()
        if (w == 's' || w == 'b') {
          sc += 1
          if (dut.io.tl_a_cap_count.peek().litValue >= expectedCount) return true
        }
      }
      false
    }

    /** Reset the first-beat capture registers so the next A-channel handshake is captured. */
    def resetFirstCapture(): Unit = {
      dut.io.tl_a_first_reset.poke(true.B)
      dut.clock.step(1)
      dut.io.tl_a_first_reset.poke(false.B)
    }

    /** Send a multi-beat write frame: header(op=0x02, addr, len=numBeats-1) + numBeats*16 payload
      * bytes.
      */
    def spiWriteMulti(addr: Long, beats: Seq[Array[Int]]): Unit = {
      csbAssert()
      spiSendHeader(0x02, addr, beats.length - 1)
      for {
        beat <- beats
        byte <- beat
      } spiXferByte(byte)
      csbDeassert()
    }

  }

  /** Search for a bit pattern (needle) in a bit buffer (haystack). Both are packed MSB-first per
    * byte. Returns bit offset or -1.
    */
  def findBitPattern(
    haystack: Array[Int],
    hayBits: Int,
    needle: Array[Int],
    needleBits: Int
  ): Int = {
    for (off <- 0 to (hayBits - needleBits)) {
      var matched = true
      var b       = 0
      while (b < needleBits && matched) {
        val hi     = off + b
        val hayBit = (haystack(hi / 8) >> (7 - (hi % 8))) & 1
        val ndlBit = (needle(b / 8) >> (7 - (b     % 8))) & 1
        if (hayBit != ndlBit) matched = false
        b += 1
      }
      if (matched) return off
    }
    -1
  }

  // =========================================================================
  // Tests
  // =========================================================================

  "async clocks" in {
    withDualClock { dut =>
      val dc         = new DualClockState(dut)
      var coincident = 0
      var spiPos     = 0
      var sysPos     = 0
      for (_ <- 0 until 2000) {
        val w = dc.tick()
        if (w == 'b' && dc.spiLevel) { coincident += 1; spiPos += 1; sysPos += 1 }
        else if (w == 'p' && dc.spiLevel) spiPos += 1
        else if (w == 's') sysPos += 1
      }
      println(s"    sys=$sysPos spi=$spiPos coincident=$coincident")
      assert(spiPos != sysPos, "frequencies should differ")
      assert(coincident < spiPos / 2, "clocks should not be locked")
    }
  }

  "read data on MISO" in {
    withDualClock { dut =>
      val dc = new DualClockState(dut)

      dc.csbAssert()
      dc.spiSendHeader(0x01, 0x0000aa55L, 0x0000)

      val misoRaw = dc.collectMisoBits(384)
      dc.csbDeassert()

      // Expected: 0x55 then 0xAA (LSB-first byte emission, MSB-first shift per byte)
      val pattern = Array(0x55, 0xaa)
      val offset  = findBitPattern(misoRaw, 384, pattern, 16)

      if (offset >= 0) println(s"    found 0x55_0xAA at bit offset $offset")
      else {
        val hex = misoRaw.take(8).map(b => f"$b%02X").mkString(" ")
        println(s"    MISO raw (first 64 bits): $hex")
      }
      assert(offset >= 0, "exact 0x55AA pattern found in MISO bit stream")
    }
  }

  "read different address" in {
    withDualClock { dut =>
      val dc = new DualClockState(dut)

      dc.csbAssert()
      dc.spiSendHeader(0x01, 0x0000c3e7L, 0x0000)

      val raw = dc.collectMisoBits(384)
      dc.csbDeassert()

      val pattern = Array(0xe7, 0xc3)
      val offset  = findBitPattern(raw, 384, pattern, 16)

      if (offset >= 0) println(s"    found 0xE7_0xC3 at bit offset $offset")
      assert(offset >= 0, "exact 0xE7C3 pattern found in MISO")
    }
  }

  "read address" in {
    withDualClock { dut =>
      val dc          = new DualClockState(dut)
      val countBefore = dut.io.tl_a_cap_count.peek().litValue.toLong

      dc.csbAssert()
      dc.spiSendHeader(0x01, 0xcafe0000L, 0x0000)
      for (_ <- 0 until 16) dc.spiXferByte(0x00)
      dc.csbDeassert()

      var got = dut.io.tl_a_cap_count.peek().litValue.toLong > countBefore
      if (!got) got = dc.waitTlACapture(countBefore + 1, 300)
      assert(got, "TL-A fired after read frame")
      assert(
        dut.io.tl_a_cap_opcode.peek().litValue == 4,
        s"TL-A opcode = Get (4), got ${dut.io.tl_a_cap_opcode.peek().litValue}"
      )
      assert(
        dut.io.tl_a_cap_address.peek().litValue == BigInt("CAFE0000", 16),
        s"TL-A address = 0xCAFE0000, got 0x${dut.io.tl_a_cap_address.peek().litValue.toString(16)}"
      )
    }
  }

  "write address and data" in {
    withDualClock { dut =>
      val dc          = new DualClockState(dut)
      val countBefore = dut.io.tl_a_cap_count.peek().litValue.toLong

      dc.csbAssert()
      dc.spiSendHeader(0x02, 0xdead0000L, 0x0000)

      // Send 16 bytes of write data: 0x01..0x10
      for (i <- 0 until 16) dc.spiXferByte((i + 1) & 0xff)
      dc.csbDeassert()

      val got = dc.waitTlACapture(countBefore + 1, 2000)
      assert(got, "TL-A fired after write frame")
      assert(
        dut.io.tl_a_cap_opcode.peek().litValue == 0,
        s"TL-A opcode = PutFullData (0), got ${dut.io.tl_a_cap_opcode.peek().litValue}"
      )
      assert(
        dut.io.tl_a_cap_address.peek().litValue == BigInt("DEAD0000", 16),
        s"TL-A address = 0xDEAD0000, got 0x${dut.io.tl_a_cap_address.peek().litValue.toString(16)}"
      )

      // Low 32 bits of write data: bytes 0x01,0x02,0x03,0x04 packed LE = 0x04030201
      val word0 = dut.io.tl_a_cap_data.peek().litValue & BigInt("FFFFFFFF", 16)
      assert(
        word0 == BigInt("04030201", 16),
        s"write payload low 32 bits = 0x04030201, got 0x${word0.toString(16)}"
      )
    }
  }

  "CSB toggle stability" in {
    withDualClock { dut =>
      val dc = new DualClockState(dut)
      for (i <- 0 until 10) {
        dc.csbAssert()
        dc.spiSendHeader(0x01, 0x10000000L + (i << 4), 0x0000)
        dc.csbDeassert()
        dc.bothCycles(20)
      }
      // If we got here without crashing, the test passes
      assert(true, "10 rapid frames without crash")
    }
  }

  "multi-beat write" in {
    withDualClock { dut =>
      val dc          = new DualClockState(dut)
      val countBefore = dut.io.tl_a_cap_count.peek().litValue.toLong
      dc.resetFirstCapture()

      // 2-beat write: len=1, base address 0xBEEF0000
      val beat0 = (0 until 16).map(i => (i + 0x10) & 0xff).toArray
      val beat1 = (0 until 16).map(i => (i + 0x20) & 0xff).toArray
      dc.spiWriteMulti(0xbeef0000L, Seq(beat0, beat1))

      val got = dc.waitTlACapture(countBefore + 2, 400)
      assert(got, "TL-A fired twice for 2-beat write")

      // First beat: address = 0xBEEF0000
      assert(
        dut.io.tl_a_first_address.peek().litValue == BigInt("BEEF0000", 16),
        s"first beat address should be 0xBEEF0000, got 0x${dut.io.tl_a_first_address.peek().litValue.toString(16)}"
      )
      // First beat data: 0x10,0x11,...,0x1F packed LE → low 32 bits = 0x13121110
      val firstWord0 = dut.io.tl_a_first_data.peek().litValue & BigInt("FFFFFFFF", 16)
      assert(
        firstWord0 == BigInt("13121110", 16),
        s"first beat low word = 0x13121110, got 0x${firstWord0.toString(16)}"
      )

      // Last beat: address = 0xBEEF0010
      assert(
        dut.io.tl_a_cap_address.peek().litValue == BigInt("BEEF0010", 16),
        s"last beat address should be 0xBEEF0010, got 0x${dut.io.tl_a_cap_address.peek().litValue.toString(16)}"
      )
      // Last beat data: 0x20,0x21,...,0x2F packed LE → low 32 bits = 0x23222120
      val lastWord0 = dut.io.tl_a_cap_data.peek().litValue & BigInt("FFFFFFFF", 16)
      assert(
        lastWord0 == BigInt("23222120", 16),
        s"last beat low word = 0x23222120, got 0x${lastWord0.toString(16)}"
      )
    }
  }

  "multi-beat read" in {
    withDualClock { dut =>
      val dc          = new DualClockState(dut)
      val countBefore = dut.io.tl_a_cap_count.peek().litValue.toLong
      dc.resetFirstCapture()

      // 2-beat read: len=1, base address 0x0000AA00
      // V2 issues 2 TL-A Gets with auto-incrementing addresses.
      dc.csbAssert()
      dc.spiSendHeader(0x01, 0x0000aa00L, 0x0001)
      // Clock enough SPI cycles for both beats to complete through CDC
      for (_ <- 0 until 32) dc.spiXferByte(0x00)
      dc.csbDeassert()

      val got = dc.waitTlACapture(countBefore + 2, 400)
      assert(got, "TL-A fired at least twice for 2-beat read")

      // Verify addresses
      val firstAddr = dut.io.tl_a_first_address.peek().litValue
      val lastAddr  = dut.io.tl_a_cap_address.peek().litValue
      val opcode    = dut.io.tl_a_cap_opcode.peek().litValue
      println(
        s"    2-beat read: first=0x${firstAddr.toString(16)}, last=0x${lastAddr.toString(16)}, opcode=$opcode"
      )

      assert(opcode == 4, s"opcode should be Get (4), got $opcode")
      assert(
        firstAddr == BigInt("AA00", 16),
        s"first beat address should be 0xAA00, got 0x${firstAddr.toString(16)}"
      )
      assert(
        lastAddr == BigInt("AA10", 16),
        s"second beat address should be 0xAA10, got 0x${lastAddr.toString(16)}"
      )
    }
  }

  "large multi-beat write (8 beats)" in {
    withDualClock { dut =>
      val dc          = new DualClockState(dut)
      val countBefore = dut.io.tl_a_cap_count.peek().litValue.toLong
      val numBeats    = 8
      val baseAddr    = 0x40000000L

      dc.resetFirstCapture()

      // Build 8 beats of distinctive data
      val beats = (0 until numBeats).map { b =>
        (0 until 16).map(i => ((b << 4) | i) & 0xff).toArray
      }
      dc.spiWriteMulti(baseAddr, beats)

      val got = dc.waitTlACapture(countBefore + numBeats, 800)
      assert(got, s"TL-A fired $numBeats times for $numBeats-beat write")

      // First beat address
      assert(
        dut.io.tl_a_first_address.peek().litValue == BigInt("40000000", 16),
        s"first beat addr, got 0x${dut.io.tl_a_first_address.peek().litValue.toString(16)}"
      )

      // Last beat address = base + (numBeats-1)*16 = 0x40000070
      val expectedLastAddr = baseAddr + (numBeats - 1) * 16
      assert(
        dut.io.tl_a_cap_address.peek().litValue == BigInt(expectedLastAddr.toHexString, 16),
        s"last beat addr = 0x${expectedLastAddr.toHexString}, got 0x${dut.io.tl_a_cap_address.peek().litValue.toString(16)}"
      )

      // Last beat data: beat 7 bytes are 0x70..0x7F, low 32 bits LE = 0x73727170
      val lastWord0 = dut.io.tl_a_cap_data.peek().litValue & BigInt("FFFFFFFF", 16)
      assert(
        lastWord0 == BigInt("73727170", 16),
        s"last beat low word = 0x73727170, got 0x${lastWord0.toString(16)}"
      )
    }
  }

  "large multi-beat read (8 beats)" in {
    withDualClock { dut =>
      val dc          = new DualClockState(dut)
      val countBefore = dut.io.tl_a_cap_count.peek().litValue.toLong
      val numBeats    = 8
      val baseAddr    = 0x0000bb00L
      dc.resetFirstCapture()

      // 8-beat read: V2 issues 8 TL-A Gets with auto-incrementing addresses.
      dc.csbAssert()
      dc.spiSendHeader(0x01, baseAddr, numBeats - 1)
      // Clock enough SPI cycles for all beats to complete through CDC
      for (_ <- 0 until 128) dc.spiXferByte(0x00)
      dc.csbDeassert()

      val got = dc.waitTlACapture(countBefore + numBeats, 800)
      assert(got, s"TL-A fired at least $numBeats times for $numBeats-beat read")

      val firstAddr        = dut.io.tl_a_first_address.peek().litValue
      val lastAddr         = dut.io.tl_a_cap_address.peek().litValue
      val opcode           = dut.io.tl_a_cap_opcode.peek().litValue
      val expectedLastAddr = baseAddr + (numBeats - 1) * 16
      println(
        s"    8-beat read: first=0x${firstAddr.toString(16)}, last=0x${lastAddr.toString(16)}, opcode=$opcode"
      )

      assert(opcode == 4, s"opcode should be Get (4), got $opcode")
      assert(
        firstAddr == BigInt("BB00", 16),
        s"first beat address should be 0xBB00, got 0x${firstAddr.toString(16)}"
      )
      assert(
        lastAddr == BigInt(expectedLastAddr.toHexString, 16),
        s"last beat address should be 0x${expectedLastAddr.toHexString}, got 0x${lastAddr.toString(16)}"
      )
    }
  }

  "regression - repeated 0xFE" in {
    withDualClock { dut =>
      val dc       = new DualClockState(dut)
      val baseAddr = 0x0000cc00L

      dc.csbAssert()
      dc.spiSendHeader(0x01, baseAddr, 0x0001) // 2-beat read

      // Wait for TL-A to fire
      dc.waitTlACapture(2, 1000)

      // Wait for 0xFE bit-by-bit to align with the stream
      var rxBuf   = 0
      var timeout = 0
      while (rxBuf != 0xfe && timeout < 2000) {
        dut.io.spi_mosi.poke(false.B)
        dc.spiCycles(1)
        val bit = if (dut.io.spi_miso.peek().litValue != 0) 1 else 0
        rxBuf = ((rxBuf << 1) & 0xff) | bit
        timeout += 1
      }
      assert(rxBuf == 0xfe, "Should have found 0xFE sync byte bit-by-bit")

      // Now we are bit-aligned. Read next 16 bytes (first beat data)
      val beat0 = (0 until 16).map(_ => dc.spiRecvByte())
      assert(beat0(0) == 0x00, s"beat0[0] should be 0x00, got 0x${beat0(0).toHexString}")
      assert(beat0(1) == 0xcc, s"beat0[1] should be 0xcc, got 0x${beat0(1).toHexString}")

      // Now check if next byte is 0xFE or something else
      val rx = dc.spiRecvByte()
      assert(rx == 0xfe, s"Should have received second 0xFE sync byte, got 0x${rx.toHexString}")

      // Read next 16 bytes (second beat data)
      val beat1 = (0 until 16).map(_ => dc.spiRecvByte())
      // Second beat address is 0x0000cc10
      assert(beat1(0) == 0x10, s"beat1[0] should be 0x10, got 0x${beat1(0).toHexString}")
      assert(beat1(1) == 0xcc, s"beat1[1] should be 0xcc, got 0x${beat1(1).toHexString}")

      dc.csbDeassert()
    }
  }
}
