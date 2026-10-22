package coralnpu

import chisel3._
import chisel3.simulator.scalatest.ChiselSim
import org.scalatest.freespec.AnyFreeSpec

class FetchL0Spec extends AnyFreeSpec with ChiselSim {
  for (width <- Seq(128, 256)) {
    s"L0 preserves instruction words across cache lines at $width bits" in {
      val p = new Parameters
      p.fetchDataBits = width
      simulate(new UncachedFetch(p, cacheL0 = true)) { dut =>
        for (i <- 0 until p.csrInCount) dut.io.csr.value(i).poke(0.U)
        dut.io.csr.value(0).poke(4.U)
        dut.io.debug_pc.valid.poke(false.B)
        dut.io.debug_pc.bits.poke(0.U)
        dut.io.iflush.valid.poke(false.B)
        dut.io.iflush.pcNext.poke(0.U)
        dut.io.linkPort.valid.poke(false.B)
        dut.io.linkPort.value.poke(0.U)
        dut.io.ibus.ready.poke(false.B)
        dut.io.ibus.rdata.poke(0.U)
        dut.io.ibus.fault.valid.poke(false.B)
        for (i <- 0 until p.instructionLanes) {
          dut.io.branch(i).valid.poke(false.B)
          dut.io.branch(i).value.poke(0.U)
          dut.io.inst.lanes(i).ready.poke(false.B)
        }
        dut.reset.poke(true.B)
        dut.clock.step(2)
        dut.reset.poke(false.B)

        // Address-dependent ADDI words distinguish stale lines and wrong lanes.
        def instruction(address: BigInt): BigInt =
          if (address == 0x20) BigInt(0x00008067) else ((address / 4 % 2048) << 20) | 0x13
        def line(address: BigInt): BigInt =
          (0 until width / 32).map(i => instruction(address + i * 4) << (32 * i)).reduce(_ | _)

        var response = BigInt(0)
        var expectedPc = BigInt(4)
        var dispatched = 0
        var returnRedirect = false
        dut.io.linkPort.valid.poke(true.B)
        dut.io.linkPort.value.poke(0.U)
        for (cycle <- 0 until 300) {
          dut.io.ibus.rdata.poke(response.U(width.W))
          val busReady = cycle % 5 != 2
          dut.io.ibus.ready.poke(busReady.B)
          val readyCount = math.min(Seq(1, 4, 4, 2, 3, 4)(cycle % 6),
            if (expectedPc <= 0x20) ((BigInt(0x20) - expectedPc) / 4 + 1).toInt else 4)
          val redirect = returnRedirect || (cycle > 0 && cycle % 29 == 0)
          val target = if (returnRedirect) 0x504 else
            Seq(0x3fc, 0x7f4, 0x104, 0x504, 0x908, 0xc0c)((cycle / 29 - 1 + 6) % 6)
          var returning = false
          dut.io.branch(0).valid.poke(redirect.B)
          dut.io.branch(0).value.poke(target.U)
          for (i <- 0 until p.instructionLanes) {
            val lane = dut.io.inst.lanes(i)
            lane.ready.poke((!redirect && i < readyCount).B)
            if (!redirect && i < readyCount && lane.valid.peek().litToBoolean) {
              lane.bits.addr.expect(expectedPc.U)
              lane.bits.inst.expect(instruction(expectedPc).U)
              if (expectedPc == 0x20) {
                lane.bits.brchFwd.expect(false.B)
                returning = true
              }
              expectedPc += 4
              dispatched += 1
            }
          }
          if (busReady && dut.io.ibus.valid.peek().litToBoolean)
            response = line(dut.io.ibus.addr.peek().litValue)
          dut.clock.step()
          if (redirect) expectedPc = BigInt(target)
          returnRedirect = returning
        }
        assert(dispatched >= 128, s"Only $dispatched instructions dispatched")
      }
    }
  }

  "L0 hits, tag replacement, faults and in-flight flush preserve the bus contract" in {
    val p = new Parameters
    p.fetchDataBits = 128
    simulate(new FetchL0Cache(p)) { dut =>
      dut.io.cpu.valid.poke(false.B)
      dut.io.cpu.addr.poke(0x100.U)
      dut.io.memory.ready.poke(false.B)
      dut.io.memory.rdata.poke(0.U)
      dut.io.memory.fault.valid.poke(false.B)
      dut.io.flush.poke(false.B)
      dut.reset.poke(true.B)
      dut.clock.step(2)
      dut.reset.poke(false.B)
      var requests = 0
      var version = 0
      var response = BigInt(0)
      def contents(address: BigInt): BigInt = address | (BigInt(version) << 32)
      def tick(): Unit = {
        dut.io.memory.rdata.poke(response.U)
        val fire = dut.io.memory.valid.peek().litToBoolean && dut.io.memory.ready.peek().litToBoolean
        val next = if (fire) contents(dut.io.memory.addr.peek().litValue) else response
        if (fire) requests += 1
        dut.clock.step()
        response = next
      }
      def read(address: Int, fault: Boolean = false): Unit = {
        dut.io.cpu.addr.poke(address.U)
        dut.io.cpu.valid.poke(true.B)
        dut.io.memory.fault.valid.poke(fault.B)
        var cycles = 0
        while (!dut.io.cpu.ready.peek().litToBoolean && cycles < 20) {
          tick()
          cycles += 1
        }
        assert(cycles < 20, "Cache request did not complete")
        dut.io.cpu.fault.valid.expect(fault.B)
        tick()
        dut.io.cpu.rdata.expect(contents(BigInt(address)).U)
        dut.io.cpu.valid.poke(false.B)
      }
      dut.io.cpu.valid.poke(true.B)
      tick()
      for (_ <- 0 until 3) {
        dut.io.memory.valid.expect(true.B)
        dut.io.memory.addr.expect(0x100.U)
        tick()
      }
      dut.io.memory.ready.poke(true.B)
      read(0x100)
      assert(requests == 1)
      read(0x100)
      assert(requests == 1, "Cache hit issued another memory request")
      read(0x500)
      read(0x100)
      assert(requests == 3, "Conflicting tags were not replaced")
      dut.io.flush.poke(true.B)
      tick()
      dut.io.flush.poke(false.B)
      read(0x100)
      assert(requests == 4, "Flush retained a cached line")
      read(0x300, fault = true)
      read(0x300)
      assert(requests == 6, "Fault response was cached")
      dut.io.cpu.addr.poke(0x200.U)
      dut.io.cpu.valid.poke(true.B)
      tick() // Latch miss.
      tick() // Memory accepts the old request.
      dut.io.flush.poke(true.B)
      tick() // Discard its response during the flush.
      dut.io.flush.poke(false.B)
      version = 1
      read(0x200)
      assert(requests == 8, "In-flight flush did not refetch the line")
    }
  }
}
