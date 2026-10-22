package coralnpu.soc

import chisel3._
import bus._

/** A generic Record that maps names to Data elements.
  */
class DataRecord(val portMap: Seq[(String, Data)]) extends Record {
  val elements            = scala.collection.immutable.ListMap(portMap: _*)
  def apply(name: String) = elements(name)
}

/** A specialized Record for TileLink ports.
  */
class TLBundleMap(val portMap: Seq[(String, TLULParameters)]) extends Record {
  val elements = scala.collection.immutable.ListMap(portMap.map { case (name, p) =>
    name -> new OpenTitanTileLink.Host2Device(p)
  }: _*)
  def apply(name: String) = elements(name).asInstanceOf[OpenTitanTileLink.Host2Device]
}

/** A simple bundle containing a clock and an asynchronous reset.
  */
class ClockResetBundle extends Bundle {
  val clock = Input(Clock())
  val reset = Input(AsyncReset())
}
