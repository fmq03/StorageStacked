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
import org.scalatest.TestSuite

/** Helper trait for TileLink-UL test transactions. */
trait TLULTestUtils extends ChiselSim { this: TestSuite =>

  /** Helper to perform a TileLink Put transaction. */
  def tlWrite(
    tl: OpenTitanTileLink.Host2Device,
    clock: Clock,
    addr: UInt,
    data: UInt,
    mask: UInt = 0xf.U,
    size: UInt = 2.U,
    source: UInt = 0.U
  ): Unit = {
    tl.a.valid.poke(true.B)
    tl.a.bits.opcode.poke(TLULOpcodesA.PutFullData.asUInt)
    tl.a.bits.address.poke(addr)
    tl.a.bits.data.poke(data)
    tl.a.bits.mask.poke(mask)
    tl.a.bits.size.poke(size)
    tl.a.bits.source.poke(source)
    while (tl.a.ready.peek().litValue == 0) clock.step()
    clock.step()
    tl.a.valid.poke(false.B)
    while (tl.d.valid.peek().litValue == 0) clock.step()
    tl.d.ready.poke(true.B)
    clock.step()
    tl.d.ready.poke(false.B)
  }

  /** Helper to perform a TileLink Get transaction. Returns (data, error). */
  def tlRead(
    tl: OpenTitanTileLink.Host2Device,
    clock: Clock,
    addr: UInt,
    size: UInt = 2.U,
    source: UInt = 0.U
  ): (BigInt, Boolean) = {
    tl.a.valid.poke(true.B)
    tl.a.bits.opcode.poke(TLULOpcodesA.Get.asUInt)
    tl.a.bits.address.poke(addr)
    tl.a.bits.size.poke(size)
    tl.a.bits.source.poke(source)
    while (tl.a.ready.peek().litValue == 0) clock.step()
    clock.step()
    tl.a.valid.poke(false.B)
    while (tl.d.valid.peek().litValue == 0) clock.step()
    val data  = tl.d.bits.data.peek().litValue
    val error = tl.d.bits.error.peek().litValue != 0
    tl.d.ready.poke(true.B)
    clock.step()
    tl.d.ready.poke(false.B)
    (data, error)
  }

  /** Wrapper for tlRead to maintain compatibility with existing tests that only expect data. */
  def tlReadData(
    tl: OpenTitanTileLink.Host2Device,
    clock: Clock,
    addr: UInt,
    size: UInt = 2.U,
    source: UInt = 0.U
  ): BigInt = {
    tlRead(tl, clock, addr, size, source)._1
  }
}
