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

package common

import chisel3._
import chisel3.util._

class IndexAllocator(capacity: Int) extends Module {
  val width    = log2Ceil(capacity)
  val ctrWidth = log2Ceil(capacity + 1)

  val io = IO(new Bundle {
    val alloc = Decoupled(UInt(width.W))
    val free  = Flipped(Decoupled(UInt(width.W)))
  })
  // pipe is not needed because it doesn't make sense to allocate txid or
  // reg just to free it in the same cycle.
  // flow is always enabled so low-bit index could have better throughput.

  // It does not make sense to free anything that we did not allocate.
  assert(!io.free.valid | io.free.ready)
}

class IndexAllocatorShifting(capacity: Int) extends IndexAllocator(capacity) {
  class State extends Bundle {
    val regs       = Vec(capacity, UInt(width.W))
    val nAvail     = UInt(ctrWidth.W)
    val isNonEmpty = Bool()

    def valids = VecInit.tabulate(capacity)(_.U < nAvail)

    def alloc(): State = {
      val precondition = isNonEmpty && nAvail <= capacity.U

      Mux(
        precondition,
        MakeWireBundle[State](
          new State,
          _.regs -> VecInit.tabulate(capacity) { x =>
            if (x + 1 < capacity)
              Mux(valids(x + 1), regs(x + 1), regs(x))
            else regs(x)
          },
          _.nAvail     -> (nAvail - 1.U),
          _.isNonEmpty -> (nAvail > 1.U)
        ),
        State.unreachable()
      )
    }

    def free(bits: UInt): State = {
      val precondition = !valids(capacity - 1)

      Mux(
        precondition,
        MakeWireBundle[State](
          new State,
          _                            -> this,
          _.regs(nAvail(width - 1, 0)) -> bits,
          _.nAvail                     -> (nAvail + 1.U),
          _.isNonEmpty                 -> true.B
        ),
        State.unreachable()
      )
    }

    def allocAndFree(bits: UInt): State = {
      val precondition = isNonEmpty && !valids(capacity - 1)

      Mux(
        precondition,
        MakeWireBundle[State](
          new State,
          _         -> this,
          _.regs(0) -> bits
        ),
        State.unreachable()
      )
    }
  }

  object State {
    def apply() = MakeWireBundle[State](
      new State,
      _.regs       -> VecInit.tabulate(capacity)(_.U),
      _.nAvail     -> capacity.U(ctrWidth.W),
      _.isNonEmpty -> true.B
    )

    def unreachable() = MakeWireBundle[State](
      new State,
      _.regs       -> DontCare,
      _.nAvail     -> DontCare,
      _.isNonEmpty -> DontCare
    )
  }

  val state = RegInit(State())

  // Elements in the allocator must always be unique.
  val badFree = VecInit.tabulate(capacity) { x =>
    io.free.valid & state.valids(x) & state.regs(x) === io.free.bits
  }
  assert(!badFree.exists(x => x))

  io.alloc.valid := state.isNonEmpty | io.free.valid
  io.alloc.bits  := Mux(state.isNonEmpty, state.regs(0), io.free.bits)
  io.free.ready  := !state.valids(capacity - 1)

  state := MuxUpTo1H(
    state,
    Seq(
      (io.alloc.fire && !io.free.fire) -> state.alloc(),
      (!io.alloc.fire && io.free.fire) -> state.free(io.free.bits),
      // Alloc and free, overwriting slot 0.
      (io.alloc.fire && io.free.fire && state.isNonEmpty) -> state.allocAndFree(io.free.bits),
      // Alloc and free with nAvail==0 does not change internal state.
      // Illegal free is unreachable
      (!io.free.ready && io.free.valid) -> State.unreachable()
    )
  )
}
