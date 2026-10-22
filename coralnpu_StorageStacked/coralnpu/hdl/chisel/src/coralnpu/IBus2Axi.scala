// Copyright 2024 Google LLC
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

package coralnpu

import chisel3._
import chisel3.util._

import bus.AxiMasterReadIO

object IBus2Axi {
  def apply(p: Parameters, id: Int = 0): IBus2Axi = {
    return Module(new IBus2Axi(p, id))
  }
}

class IBus2Axi(p: Parameters, id: Int = 0) extends Module {
  val io = IO(new Bundle {
    val ibus = Flipped(new IBusIO(p))
    val axi  = new AxiMasterReadIO(p.axi2AddrBits, p.axi2DataBits, p.axi2IdBits)
  })
  io.axi.defaults()

  val linebit = log2Ceil(p.lsuDataBits / 8)

  val sraddrActive = RegInit(false.B)
  val saddrReg     = RegInit(0.U(p.axi2AddrBits.W))
  val sdata        = RegInit(0.U(p.axi2DataBits.W))
  val sresp        = RegInit(0.U(2.W))
  val sdataValid   = RegInit(false.B)

  val saddr     = Cat(io.ibus.addr(p.fetchAddrBits - 1, linebit), 0.U(linebit.W))
  val addrMatch = saddr === saddrReg

  // Handshake logic: we are ready if AXI just responded or we have buffered data,
  // provided the address still matches what we fetched.
  io.ibus.ready := (io.axi.data.valid && sraddrActive || sdataValid) && addrMatch
  io.ibus.rdata := sdata // Fetch unit expects data cycle after ready

  // AXI Read Address Channel
  // Can start next fetch if not busy and (no valid data or address changed)
  val canStartNext = !sraddrActive && (!sdataValid || !addrMatch)
  io.axi.addr.valid     := io.ibus.valid && canStartNext
  io.axi.addr.bits.addr := saddr
  io.axi.addr.bits.id   := id.U
  io.axi.addr.bits.prot := 2.U

  // State machine
  sraddrActive := Mux(io.axi.data.fire, false.B, Mux(io.axi.addr.fire, true.B, sraddrActive))
  sdata        := Mux(io.axi.data.fire, io.axi.data.bits.data, sdata)
  sresp        := Mux(io.axi.data.fire, io.axi.data.bits.resp, sresp)
  sdataValid   := Mux(
    io.axi.data.fire,
    !io.ibus.ready,
    Mux((io.ibus.ready && io.ibus.valid) || io.axi.addr.fire, false.B, sdataValid)
  )
  saddrReg := Mux(io.axi.addr.fire, io.axi.addr.bits.addr, saddrReg)

  assert(!io.axi.data.fire || sraddrActive)

  // AXI Read Data Channel
  io.axi.data.ready := true.B

  // Fault reporting
  io.ibus.fault.valid := io.ibus.ready && (Mux(
    io.axi.data.valid,
    io.axi.data.bits.resp,
    sresp
  ) =/= 0.U)
  io.ibus.fault.bits.write := false.B
  io.ibus.fault.bits.addr  := saddrReg
  io.ibus.fault.bits.epc   := io.ibus.addr
}
