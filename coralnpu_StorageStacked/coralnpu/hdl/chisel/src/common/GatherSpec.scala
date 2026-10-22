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
import chisel3.simulator.scalatest.ChiselSim
import org.scalatest.freespec.AnyFreeSpec
import scala.util.Random

class GatherTester extends Module {
  val io = IO(new Bundle {
    val indices = Input(Vec(16, UInt(4.W)))
    val data    = Input(Vec(16, UInt(8.W)))
    val out     = Output(Vec(16, UInt(8.W)))
  })

  io.out := Gather(io.indices, io.data)
}

class GatherSpec extends AnyFreeSpec with ChiselSim {
  "Random Test" in {
    simulate(new GatherTester) { dut =>
      for (_ <- 0 until 100) {
        // Set inputs
        val indices = Seq.fill(16)(Random.between(0, 16))
        val data    = Seq.fill(16)(Random.between(0, 256))
        for (i <- 0 until 16) {
          dut.io.indices(i).poke(indices(i))
          dut.io.data(i).poke(data(i))
        }

        // Check results
        for (i <- 0 until 16) {
          dut.io.out(i).expect(data(indices(i)))
        }
      }
    }
  }
}
