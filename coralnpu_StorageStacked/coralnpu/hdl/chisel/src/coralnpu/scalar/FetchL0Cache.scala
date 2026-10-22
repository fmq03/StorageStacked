package coralnpu

import chisel3._
import chisel3.util._
import common._

/** 为当前取指控制器提供直接映射 L0，保持 IBus 接受后一周期返回的契约。 */
class FetchL0Cache(p: Parameters) extends Module {
  val io = IO(new Bundle {
    val cpu = Flipped(new IBusIO(p))
    val memory = new IBusIO(p)
    val flush = Input(Bool())
  })
  private val lineBits = log2Ceil(p.fetchDataBits / 8)
  private val entries = p.fetchCacheBytes * 8 / p.fetchDataBits
  private val indexBits = log2Ceil(entries)
  private val tagBits = p.fetchAddrBits - lineBits - indexBits
  require(isPow2(entries) && entries > 1)

  val valid = RegInit(0.U(entries.W))
  val tags = Reg(Vec(entries, UInt(tagBits.W)))
  val data = Reg(Vec(entries, UInt(p.fetchDataBits.W)))
  val index = io.cpu.addr(lineBits + indexBits - 1, lineBits)
  val tag = io.cpu.addr(p.fetchAddrBits - 1, lineBits + indexBits)
  val hit = valid(index) && VecAt(tags, index) === tag

  // 总线请求一旦发出就保持到接受；刷新只丢弃回填，不撤销请求。
  val requesting = RegInit(false.B)
  val missAddress = Reg(UInt(p.fetchAddrBits.W))
  val requestFault = Reg(Valid(new FaultInfo(p)))
  val response = RegNext(io.memory.fire, false.B)
  val busy = requesting || response
  val discard = RegInit(false.B)
  io.memory.valid := requesting
  io.memory.addr := missAddress
  when(io.memory.fire) {
    requesting := false.B
    requestFault := io.memory.fault
  }

  // 故障也必须应答给取指器，但不把故障行写入缓存。
  val returned = RegInit(false.B)
  val returnedAddress = Reg(UInt(p.fetchAddrBits.W))
  val returnedData = Reg(UInt(p.fetchDataBits.W))
  val returnedFault = Reg(Valid(new FaultInfo(p)))
  val returnedHit = returned && io.cpu.addr === returnedAddress
  io.cpu.ready := !io.flush && !busy && (hit || returnedHit)
  io.cpu.fault := Mux(returnedHit, returnedFault, MakeInvalid(new FaultInfo(p)))
  val readData = RegInit(0.U(p.fetchDataBits.W))
  io.cpu.rdata := readData
  when(io.cpu.fire) {
    readData := Mux(returnedHit, returnedData, VecAt(data, index))
    returned := false.B
  }

  when(io.cpu.valid && !hit && !returnedHit && !busy && !io.flush) {
    missAddress := Cat(io.cpu.addr(p.fetchAddrBits - 1, lineBits), 0.U(lineBits.W))
    requesting := true.B
    discard := false.B
    returned := false.B
  }
  when(response && !discard && !io.flush) {
    val fillIndex = missAddress(lineBits + indexBits - 1, lineBits)
    returned := true.B
    returnedAddress := missAddress
    returnedData := io.memory.rdata
    returnedFault := requestFault
    when(!requestFault.valid) {
      tags(fillIndex) := missAddress(p.fetchAddrBits - 1, lineBits + indexBits)
      data(fillIndex) := io.memory.rdata
      valid := valid | UIntToOH(fillIndex, entries)
    }
  }
  when(io.flush) {
    valid := 0.U
    returned := false.B
    when(busy) { discard := true.B }
  }
}
