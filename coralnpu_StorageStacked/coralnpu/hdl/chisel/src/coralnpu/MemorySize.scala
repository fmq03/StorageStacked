package coralnpu

case class MemorySize(bytes: Int) {
  def kBytes: Int = bytes / 1024
  def KBytes: Int = kBytes // Alias as requested
}

object MemorySize {
  def fromBytes(bytes: Int): MemorySize   = MemorySize(bytes)
  def fromKBytes(kBytes: Int): MemorySize = MemorySize(kBytes * 1024)
  def fromMBytes(mBytes: Int): MemorySize = MemorySize(mBytes * 1024 * 1024)
}
