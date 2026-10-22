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

object ClintRegister extends ChiselEnum {
  val MSIP        = Value("h0000".U(16.W))
  val MTIMECMP_LO = Value("h4000".U(16.W))
  val MTIMECMP_HI = Value("h4004".U(16.W))
  val MTIME_LO    = Value("hBFF8".U(16.W))
  val MTIME_HI    = Value("hBFFC".U(16.W))
}

class Clint(p: TLULParameters) extends Module {
  val tlul_p = p
  val io     = IO(new Bundle {
    val tl   = Flipped(new OpenTitanTileLink.Host2Device(tlul_p))
    val mtip = Output(Bool())
    val msip = Output(Bool())
  })

  // Standard SiFive CLINT register offsets
  import ClintRegister._

  val msip     = RegInit(0.U(32.W))
  val mtime    = RegInit(0.U(64.W))
  val mtimecmp = RegInit("xFFFFFFFFFFFFFFFF".U(64.W))

  // Interrupts
  io.mtip := mtime >= mtimecmp
  io.msip := msip(0)

  // TileLink Interface
  val tl_a = io.tl.a
  val tl_d = io.tl.d

  val addr_offset = tl_a.bits.address(15, 0)

  // Response logic
  val tl_d_valid  = RegInit(false.B)
  val tl_d_opcode = RegInit(0.U(3.W))
  val tl_d_data   = RegInit(0.U(32.W))
  val tl_d_size   = RegInit(0.U(tlul_p.z.W))
  val tl_d_source = RegInit(0.U(tlul_p.a.W))
  val tl_d_error  = RegInit(false.B)

  val is_write = (tl_a.bits.opcode === TLULOpcodesA.PutFullData.asUInt ||
    tl_a.bits.opcode === TLULOpcodesA.PutPartialData.asUInt)
  val tl_a_write_fire = tl_a.fire && is_write

  msip := Mux(
    tl_a_write_fire && addr_offset === MSIP.asUInt,
    Cat(0.U(31.W), tl_a.bits.data(0)),
    msip
  )

  // mtime increments every cycle, but can be overwritten by TLUL writes.
  // Increment is suppressed if any part of mtime or mtimecmp is being written.
  mtime := MuxCase(
    mtime + 1.U,
    Seq(
      (tl_a_write_fire && addr_offset === MTIME_LO.asUInt) -> Cat(mtime(63, 32), tl_a.bits.data),
      (tl_a_write_fire && addr_offset === MTIME_HI.asUInt) -> Cat(tl_a.bits.data, mtime(31, 0)),
      (tl_a_write_fire && (addr_offset === MTIMECMP_LO.asUInt || addr_offset === MTIMECMP_HI.asUInt)) -> mtime
    )
  )

  mtimecmp := MuxCase(
    mtimecmp,
    Seq(
      (tl_a_write_fire && addr_offset === MTIMECMP_LO.asUInt) -> Cat(
        mtimecmp(63, 32),
        tl_a.bits.data
      ),
      (tl_a_write_fire && addr_offset === MTIMECMP_HI.asUInt) -> Cat(
        tl_a.bits.data,
        mtimecmp(31, 0)
      )
    )
  )

  val read_data = MuxLookup(addr_offset, 0.U)(
    Seq(
      MSIP.asUInt        -> msip,
      MTIMECMP_LO.asUInt -> mtimecmp(31, 0),
      MTIMECMP_HI.asUInt -> mtimecmp(63, 32),
      MTIME_LO.asUInt    -> mtime(31, 0),
      MTIME_HI.asUInt    -> mtime(63, 32)
    )
  )

  tl_a.ready := !tl_d_valid

  tl_d_valid  := Mux(tl_d.fire, false.B, Mux(tl_a.fire, true.B, tl_d_valid))
  tl_d_source := Mux(tl_a.fire, tl_a.bits.source, tl_d_source)
  tl_d_size   := Mux(tl_a.fire, tl_a.bits.size, tl_d_size)
  tl_d_error  := Mux(tl_a.fire, false.B, tl_d_error)
  tl_d_opcode := Mux(
    tl_a.fire,
    Mux(is_write, TLULOpcodesD.AccessAck.asUInt, TLULOpcodesD.AccessAckData.asUInt),
    tl_d_opcode
  )
  tl_d_data := Mux(tl_a.fire, Mux(is_write, 0.U, read_data), tl_d_data)

  tl_d.valid       := tl_d_valid
  tl_d.bits.opcode := tl_d_opcode
  tl_d.bits.data   := tl_d_data
  tl_d.bits.size   := tl_d_size
  tl_d.bits.source := tl_d_source
  tl_d.bits.error  := tl_d_error
  tl_d.bits.sink   := 0.U
  tl_d.bits.param  := 0.U
  tl_d.bits.user   := 0.U.asTypeOf(tl_d.bits.user)
}
