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
import chisel3.simulator.scalatest.ChiselSim
import coralnpu.Parameters
import org.scalatest.freespec.AnyFreeSpec
class TlulToSramTest extends AnyFreeSpec with ChiselSim with TLULTestUtils {
  val p = new Parameters
  p.lsuDataBits = 128
  val tlul_p = p.toTLUL()

  "TlulToSram should handle basic read/write" in {
    simulate(new Module {
      val io = IO(new Bundle {
        val tl = Flipped(new OpenTitanTileLink.Host2Device(tlul_p))
      })
      val dut = Module(new TlulToSram(tlul_p, 10))
      io.tl <> dut.io.tl

      // Mock SRAM (registered memory)
      val mem = SyncReadMem(1024, Vec(16, UInt(8.W)))

      // Simple 1-cycle latency mock
      dut.io.sram.rvalid := RegNext(dut.io.sram.enable)
      val memReadData = mem.read(dut.io.sram.addr, dut.io.sram.enable && !dut.io.sram.write)
      dut.io.sram.rdata := memReadData.asUInt

      when(dut.io.sram.enable && dut.io.sram.write) {
        mem.write(
          dut.io.sram.addr,
          dut.io.sram.wdata.asTypeOf(Vec(16, UInt(8.W))),
          dut.io.sram.wmask.asBools
        )
      }
    }) { dut =>
      dut.reset.poke(true.B)
      dut.clock.step()
      dut.reset.poke(false.B)

      val testData = BigInt("DEADBEEFCAFEBABE1234567887654321", 16)
      // Write to address 0x10 (word address 1)
      // mask=0xffff, size=4 (16 bytes), source=1
      tlWrite(dut.io.tl, dut.clock, 0x10.U, testData.U, 0xffff.U, 4.U, 1.U)

      // Read back from address 0x10
      // size=4 (16 bytes), source=2
      val rdata = tlReadData(dut.io.tl, dut.clock, 0x10.U, 4.U, 2.U)
      assert(rdata == testData, s"Expected 0x${testData.toString(16)}, got 0x${rdata.toString(16)}")

      // Write another value
      val testData2 = BigInt("1234567890ABCDEF1234567890ABCDEF", 16)
      tlWrite(dut.io.tl, dut.clock, 0x20.U, testData2.U, 0xffff.U, 4.U, 1.U)
      val rdata2 = tlReadData(dut.io.tl, dut.clock, 0x20.U, 4.U, 2.U)
      assert(rdata2 == testData2)
    }
  }

  "TlulToSram should handle backpressure" in {
    simulate(new Module {
      val io = IO(new Bundle {
        val tl = Flipped(new OpenTitanTileLink.Host2Device(tlul_p))
      })
      val dut = Module(new TlulToSram(tlul_p, 10))
      io.tl <> dut.io.tl

      // Mock SRAM
      val rvalid = RegInit(false.B)
      val rdata  = RegInit(0.U(128.W))
      dut.io.sram.rvalid := rvalid
      dut.io.sram.rdata  := rdata
      rvalid             := dut.io.sram.enable
    }) { dut =>
      dut.reset.poke(true.B)
      dut.clock.step()
      dut.reset.poke(false.B)

      // Stall the D channel
      dut.io.tl.d.ready.poke(false.B)

      // Send a request
      dut.io.tl.a.valid.poke(true.B)
      dut.io.tl.a.bits.opcode.poke(TLULOpcodesA.Get.asUInt)
      dut.io.tl.a.bits.address.poke(0x10.U)

      while (dut.io.tl.a.ready.peek().litValue == 0) dut.clock.step()
      dut.clock.step()
      dut.io.tl.a.valid.poke(false.B)

      // Request should be in the pipeline, but D channel is stalled.
      // Wait a few cycles.
      dut.clock.step(5)

      // D channel should have a valid response waiting
      assert(dut.io.tl.d.valid.peek().litValue == 1)

      // Now release the stall
      dut.io.tl.d.ready.poke(true.B)
      dut.clock.step()
      assert(dut.io.tl.d.valid.peek().litValue == 0)
    }
  }
}
