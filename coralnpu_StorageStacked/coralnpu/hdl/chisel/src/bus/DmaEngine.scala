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
import common.{MakeInvalid, MakeValid, MuBi4}

class DmaEngine(hostParams: TLULParameters, deviceParams: TLULParameters) extends Module {
  val hostTlulP   = hostParams
  val deviceTlulP = deviceParams

  val io = IO(new Bundle {
    val tl_host   = new OpenTitanTileLink.Host2Device(hostTlulP)
    val tl_device = Flipped(new OpenTitanTileLink.Host2Device(deviceTlulP))
  })

  // --- Internal Queued Signals ---
  val host_a_internal = Wire(Decoupled(new OpenTitanTileLink.A_Channel(hostTlulP)))
  val host_d_internal = Wire(Flipped(Decoupled(new OpenTitanTileLink.D_Channel(hostTlulP))))
  val dev_a_internal  = Wire(Flipped(Decoupled(new OpenTitanTileLink.A_Channel(deviceTlulP))))
  val dev_d_internal  = Wire(Decoupled(new OpenTitanTileLink.D_Channel(deviceTlulP)))

  // --- CSR Register Map ---
  object DmaReg extends ChiselEnum {
    val CTRL        = Value(0x00.U(12.W))
    val STATUS      = Value(0x04.U(12.W))
    val DESC_ADDR   = Value(0x08.U(12.W))
    val CUR_DESC    = Value(0x0c.U(12.W))
    val XFER_REMAIN = Value(0x10.U(12.W))
    // NB: Without a value using all 12 bits,
    // the ChiselEnum collapses to the bit-width of the largest
    // declared Value.
    val RSVD = Value(0xfff.U(12.W))
  }
  import DmaReg._

  // --- FSM States ---
  object State extends ChiselEnum {
    val sIdle, sFetchDesc0, sFetchDesc0Resp, sFetchDesc1, sFetchDesc1Resp, sPollCheck, sPollReq,
      sPollResp, sXferReadReq, sXferReadResp, sXferWriteReq, sXferWriteResp, sDone = Value
  }
  import State._

  // --- Bundles ---
  class DmaCtrl extends Bundle {
    val enable = Bool()
    val start  = Bool()
    val abort  = Bool()
  }

  class DmaStatus extends Bundle {
    val busy       = Bool()
    val done       = Bool()
    val error      = Bool()
    val error_code = UInt(4.W)
  }

  class DmaDescriptor extends Bundle {
    val src_addr   = UInt(32.W)
    val dst_addr   = UInt(32.W)
    val xfer_len   = UInt(24.W)
    val xfer_width = UInt(3.W)
    val src_fixed  = Bool()
    val dst_fixed  = Bool()
    val poll_en    = Bool()
    val next_desc  = UInt(32.W)
    val poll_addr  = UInt(32.W)
    val poll_mask  = UInt(32.W)
    val poll_value = UInt(32.W)
  }

  class DmaDescriptorPart0 extends Bundle {
    val next_desc  = UInt(32.W)
    val reserved   = UInt(2.W)
    val poll_en    = Bool()
    val dst_fixed  = Bool()
    val src_fixed  = Bool()
    val xfer_width = UInt(3.W)
    val xfer_len   = UInt(24.W)
    val dst_addr   = UInt(32.W)
    val src_addr   = UInt(32.W)
  }

  class DmaDescriptorPart1 extends Bundle {
    val padding    = UInt(32.W)
    val poll_value = UInt(32.W)
    val poll_mask  = UInt(32.W)
    val poll_addr  = UInt(32.W)
  }

  class DmaXferState extends Bundle {
    val src_addr  = UInt(32.W)
    val dst_addr  = UInt(32.W)
    val remaining = UInt(24.W)
    val desc_addr = UInt(32.W)
    val data_buf  = UInt((hostTlulP.w * 8).W)
  }

  class DmaDevD extends Bundle {
    val opcode = UInt(3.W)
    val data   = UInt(32.W)
    val size   = UInt(deviceTlulP.z.W)
    val source = UInt(deviceTlulP.o.W)
    val error  = Bool()
  }

  // --- Registers (Sinks) ---
  val state = RegInit(sIdle)

  // Register Bundles
  val ctrl      = RegInit(0.U.asTypeOf(new DmaCtrl))
  val status    = RegInit(0.U.asTypeOf(new DmaStatus))
  val desc      = RegInit(0.U.asTypeOf(new DmaDescriptor))
  val xfer      = RegInit(0.U.asTypeOf(new DmaXferState))
  val dev_d_reg = RegInit(MakeInvalid(new DmaDevD))

  val desc_addr_reg = RegInit(0.U(32.W))

  // --- Device Port CSR Logic (Sinks) ---
  val dev_addr_offset                    = dev_a_internal.bits.address(11, 0)
  val (dev_addr_reg, dev_addr_reg_valid) = DmaReg.safe(dev_addr_offset)
  val dev_is_known_addr                  = dev_addr_reg_valid && (dev_addr_reg =/= RSVD)
  val (dev_a_internal_opcode, dev_a_internal_opcode_valid) =
    TLULOpcodesA.safe(dev_a_internal.bits.opcode)
  val dev_is_write =
    dev_a_internal_opcode_valid && dev_a_internal_opcode.isOneOf(
      TLULOpcodesA.PutFullData,
      TLULOpcodesA.PutPartialData
    )

  // --- Host Port Logic (Sinks) ---

  // Intermediate signals
  val start_condition = ctrl.start && ctrl.enable
  val abort_condition = ctrl.abort
  val host_a_fire     = host_a_internal.fire
  val host_d_fire     = host_d_internal.fire
  val host_d_err      = host_d_internal.bits.error
  val beat_bytes      = 1.U << desc.xfer_width
  val new_remaining   = xfer.remaining - beat_bytes

  // Helper: compute mask for a given size (log2 bytes)
  def sizeMask(size: UInt): UInt = {
    val maxBytes = hostTlulP.w
    MuxLookup(size, ((1 << maxBytes) - 1).U)(
      (0 until log2Ceil(maxBytes) + 1).map(i => i.U -> ((1 << (1 << i)) - 1).U)
    )
  }

  // --- State Machine Next State ---
  state := MuxCase(
    state,
    Seq(
      abort_condition                            -> sIdle,
      (state === sIdle && start_condition)       -> sFetchDesc0,
      (state === sFetchDesc0 && host_a_fire)     -> sFetchDesc0Resp,
      (state === sFetchDesc0Resp && host_d_fire) -> Mux(host_d_err, sDone, sFetchDesc1),
      (state === sFetchDesc1 && host_a_fire)     -> sFetchDesc1Resp,
      (state === sFetchDesc1Resp && host_d_fire) -> Mux(host_d_err, sDone, sPollCheck),
      (state === sPollCheck) -> Mux(desc.poll_en && desc.poll_addr =/= 0.U, sPollReq, sXferReadReq),
      (state === sPollReq && host_a_fire)  -> sPollResp,
      (state === sPollResp && host_d_fire) -> Mux(
        host_d_err,
        sDone,
        Mux(
          ((host_d_internal.bits.data >> (desc.poll_addr(log2Ceil(hostTlulP.w) - 1, 0) << 3))(
            31,
            0
          ) & desc.poll_mask) === desc.poll_value,
          sXferReadReq,
          sPollReq
        )
      ),
      (state === sXferReadReq && host_a_fire)   -> sXferReadResp,
      (state === sXferReadResp && host_d_fire)  -> Mux(host_d_err, sDone, sXferWriteReq),
      (state === sXferWriteReq && host_a_fire)  -> sXferWriteResp,
      (state === sXferWriteResp && host_d_fire) -> Mux(
        host_d_err,
        sDone,
        Mux(new_remaining === 0.U, Mux(desc.next_desc =/= 0.U, sFetchDesc0, sDone), sPollCheck)
      ),
      (state === sDone) -> sIdle
    )
  )

  // --- Register Assignments ---

  ctrl := MuxCase(
    ctrl,
    Seq(
      abort_condition -> {
        val b = WireInit(ctrl)
        b.abort := false.B
        b
      },
      (state === sIdle && start_condition || ctrl.start && !ctrl.enable) -> {
        val b = WireInit(ctrl)
        b.start := false.B
        b
      },
      (dev_a_internal.fire && dev_is_write && dev_addr_reg === CTRL) -> {
        val b = Wire(new DmaCtrl)
        b.enable := dev_a_internal.bits.data(0)
        b.start  := dev_a_internal.bits.data(1)
        b.abort  := dev_a_internal.bits.data(2)
        b
      }
    )
  )

  status := MuxCase(
    status,
    Seq(
      abort_condition -> {
        val b = Wire(new DmaStatus)
        b.busy       := false.B
        b.done       := false.B
        b.error      := true.B
        b.error_code := 5.U
        b
      },
      (state === sIdle && start_condition) -> {
        val b = Wire(new DmaStatus)
        b.busy       := true.B
        b.done       := false.B
        b.error      := false.B
        b.error_code := 0.U
        b
      },
      (state === sDone) -> {
        val b = WireInit(status)
        b.busy := false.B
        b.done := true.B
        b
      },
      (host_d_fire && host_d_err) -> {
        val b = WireInit(status)
        b.error      := true.B
        b.error_code := MuxLookup(state, 0.U)(
          Seq(
            sFetchDesc0Resp -> 1.U,
            sFetchDesc1Resp -> 1.U,
            sPollResp       -> 2.U,
            sXferReadResp   -> 3.U,
            sXferWriteResp  -> 4.U
          )
        )
        b
      }
    )
  )

  desc_addr_reg := Mux(
    dev_a_internal.fire && dev_is_write && dev_addr_reg === DESC_ADDR,
    dev_a_internal.bits.data,
    desc_addr_reg
  )

  desc := MuxCase(
    desc,
    Seq(
      (state === sFetchDesc0Resp && host_d_fire && !host_d_err) -> {
        val b  = WireInit(desc)
        val d0 = host_d_internal.bits.data.asTypeOf(new DmaDescriptorPart0)
        b.src_addr   := d0.src_addr
        b.dst_addr   := d0.dst_addr
        b.xfer_len   := d0.xfer_len
        b.xfer_width := d0.xfer_width
        b.src_fixed  := d0.src_fixed
        b.dst_fixed  := d0.dst_fixed
        b.poll_en    := d0.poll_en
        b.next_desc  := d0.next_desc
        b
      },
      (state === sFetchDesc1Resp && host_d_fire && !host_d_err) -> {
        val b  = WireInit(desc)
        val d1 = host_d_internal.bits.data.asTypeOf(new DmaDescriptorPart1)
        b.poll_addr  := d1.poll_addr
        b.poll_mask  := d1.poll_mask
        b.poll_value := d1.poll_value
        b
      }
    )
  )

  xfer := MuxCase(
    xfer,
    Seq(
      (state === sIdle && start_condition) -> {
        val b = WireInit(xfer)
        b.desc_addr := desc_addr_reg
        b
      },
      (state === sFetchDesc1Resp && host_d_fire && !host_d_err) -> {
        val b = WireInit(xfer)
        b.src_addr  := desc.src_addr
        b.dst_addr  := desc.dst_addr
        b.remaining := desc.xfer_len
        b
      },
      (state === sXferReadResp && host_d_fire && !host_d_err) -> {
        val b = WireInit(xfer)
        b.data_buf := host_d_internal.bits.data >> (xfer
          .src_addr(log2Ceil(hostTlulP.w) - 1, 0) << 3)
        b
      },
      (state === sXferWriteResp && host_d_fire && !host_d_err) -> {
        val b = WireInit(xfer)
        b.src_addr  := Mux(desc.src_fixed, xfer.src_addr, xfer.src_addr + beat_bytes)
        b.dst_addr  := Mux(desc.dst_fixed, xfer.dst_addr, xfer.dst_addr + beat_bytes)
        b.remaining := new_remaining
        b.desc_addr := Mux(
          new_remaining === 0.U && desc.next_desc =/= 0.U,
          desc.next_desc,
          xfer.desc_addr
        )
        b
      }
    )
  )

  // CSR Result Latches
  val is_ro_reg = dev_addr_reg.isOneOf(STATUS, CUR_DESC, XFER_REMAIN)

  val ctrl_reg_val   = Cat(0.U(29.W), 0.U(2.W), ctrl.enable)
  val status_reg_val =
    Cat(0.U(24.W), status.error_code, 0.U(1.W), status.error, status.done, status.busy)
  val xfer_remain_val = Cat(0.U(8.W), xfer.remaining)

  dev_d_reg := Mux(
    dev_a_internal.fire, {
      val b = Wire(new DmaDevD)
      b.source := dev_a_internal.bits.source
      b.size   := dev_a_internal.bits.size
      b.opcode := Mux(
        dev_is_write,
        TLULOpcodesD.AccessAck.asUInt,
        TLULOpcodesD.AccessAckData.asUInt
      )
      b.error := !dev_is_known_addr || (dev_is_write && is_ro_reg)
      b.data  := Mux(
        !dev_is_write,
        MuxLookup(dev_addr_reg, 0.U)(
          Seq(
            CTRL        -> ctrl_reg_val,
            STATUS      -> status_reg_val,
            DESC_ADDR   -> desc_addr_reg,
            CUR_DESC    -> xfer.desc_addr,
            XFER_REMAIN -> xfer_remain_val
          )
        ),
        dev_d_reg.bits.data
      )
      MakeValid(b)
    },
    Mux(io.tl_device.d.fire, MakeInvalid(new DmaDevD), dev_d_reg)
  )

  // --- Output Logic (Sinks) ---

  // Host A Channel Logic
  host_a_internal.valid := state.isOneOf(
    sFetchDesc0,
    sFetchDesc1,
    sPollReq,
    sXferReadReq,
    sXferWriteReq
  )

  val host_a_bits = Wire(new OpenTitanTileLink.A_Channel(hostTlulP))
  host_a_bits.opcode := Mux(
    state === sXferWriteReq,
    TLULOpcodesA.PutFullData.asUInt,
    TLULOpcodesA.Get.asUInt
  )
  host_a_bits.param := 0.U
  host_a_bits.size  := MuxCase(
    0.U,
    Seq(
      state.isOneOf(sFetchDesc0, sFetchDesc1)    -> 4.U, // 16 bytes
      (state === sPollReq)                       -> 2.U, // 4 bytes
      state.isOneOf(sXferReadReq, sXferWriteReq) -> desc.xfer_width
    )
  )
  host_a_bits.source  := 0x0.U
  host_a_bits.address := MuxLookup(state, 0.U)(
    Seq(
      sFetchDesc0   -> xfer.desc_addr,
      sFetchDesc1   -> (xfer.desc_addr + 16.U),
      sPollReq      -> desc.poll_addr,
      sXferReadReq  -> xfer.src_addr,
      sXferWriteReq -> xfer.dst_addr
    )
  )
  host_a_bits.mask := MuxLookup(state, 0.U)(
    Seq(
      sFetchDesc0  -> Fill(hostTlulP.w, 1.U(1.W)),
      sFetchDesc1  -> Fill(hostTlulP.w, 1.U(1.W)),
      sPollReq     -> ("hf".U << desc.poll_addr(log2Ceil(hostTlulP.w) - 1, 0)),
      sXferReadReq -> (sizeMask(desc.xfer_width) << xfer.src_addr(
        log2Ceil(hostTlulP.w) - 1,
        0
      )),
      sXferWriteReq -> (sizeMask(desc.xfer_width) << xfer.dst_addr(
        log2Ceil(hostTlulP.w) - 1,
        0
      ))
    )
  )
  host_a_bits.data := (xfer.data_buf << (xfer.dst_addr(log2Ceil(hostTlulP.w) - 1, 0) << 3))
  host_a_bits.user := 0.U.asTypeOf(host_a_bits.user)
  host_a_bits.user.instr_type := MuBi4.False.asUInt

  host_a_internal.bits := host_a_bits

  // Host A Queue
  io.tl_host.a <> Queue(host_a_internal, 1)

  // Host D Queue
  host_d_internal <> Queue(io.tl_host.d, 1)

  // Host D Channel Logic
  host_d_internal.ready := state.isOneOf(
    sFetchDesc0Resp,
    sFetchDesc1Resp,
    sPollResp,
    sXferReadResp,
    sXferWriteResp
  )

  // Device A Queue
  dev_a_internal <> Queue(io.tl_device.a, 1)

  // Device A Channel Logic
  dev_a_internal.ready := !dev_d_reg.valid

  // Device D Channel Logic
  dev_d_internal.valid       := dev_d_reg.valid
  dev_d_internal.bits.opcode := dev_d_reg.bits.opcode
  dev_d_internal.bits.data   := dev_d_reg.bits.data
  dev_d_internal.bits.size   := dev_d_reg.bits.size
  dev_d_internal.bits.source := dev_d_reg.bits.source
  dev_d_internal.bits.error  := dev_d_reg.bits.error
  dev_d_internal.bits.sink   := 0.U
  dev_d_internal.bits.param  := 0.U
  dev_d_internal.bits.user   := 0.U.asTypeOf(dev_d_internal.bits.user)

  // Device D Queue
  io.tl_device.d <> Queue(dev_d_internal, 1)
}
