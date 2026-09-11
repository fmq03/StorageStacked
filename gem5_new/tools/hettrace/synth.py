"""合成 trace 生成器。

用途有两个，都不是玩具：

1. 让 Python 工具链（归并 / 校验 / 统计 / 转换）在三个仿真器都还没编出来时
   就能被测试。工具链的 bug 若等到真 trace 出来才发现，排查成本高得多。
2. 造出各种失效形态（tick 回退、seq 断裂、无共享、串行不重叠），验证
   validate 真的能抓住它们 —— 一个从不报错的校验器没有价值。

写出的文件与 libhettrace 的二进制格式逐字节兼容，由 tools/tests 交叉验证。
"""

from __future__ import annotations

import os
import struct

from . import addrmap
from .reader import (
    CHAN_AR,
    CHAN_AW,
    CHAN_B,
    CHAN_R,
    CHAN_W,
    FLAG_BURST_BEAT,
    FLAG_LAST,
    FLAG_SYNTH,
    FLAG_UNMAPPED,
    FORMAT_VERSION,
    HDR_FILTERED_DRAM,
    MAGIC,
    OP_READ,
    OP_WRITE,
    RECORD_SIZE,
    _HEADER_STRUCT,
    _RECORD_STRUCT,
    is_data_chan,
)

_BURST_INCR = 1


def _full_strb(nbytes):
    return (1 << 64) - 1 if nbytes >= 64 else (1 << nbytes) - 1


def _log2_size(nbytes):
    s = 0
    while (1 << (s + 1)) <= nbytes and s < 7:
        s += 1
    return s


def _axi_beat_address(start, beat_bytes, beats, burst, beat):
    if burst == 0:
        return start
    if burst == 1:
        return start + beat * beat_bytes
    if burst == 2:
        span = beats * beat_bytes
        boundary = (start // span) * span
        return boundary + ((start - boundary + beat * beat_bytes) % span)
    raise ValueError("非法 AXI burst=%d" % burst)


class SynthWriter:
    """按 libhettrace 二进制格式写出，字段语义与 C++ TraceWriter 一致。

    emit() 与 C++ 的 Emit() 一样只写数据通道 —— 它是"每次访问一条记录"这层
    语义。要造带地址/响应通道的 trace（HetAxiMonitor 那一级）用 emit_txn()。
    """

    def __init__(self, directory, src_name, filtered_dram=True,
                 level=None, axi_data_bytes=None, synth_flag=False):
        if src_name not in addrmap.SOURCES:
            raise ValueError("未知源 %r" % src_name)
        self.src_id, dev_level, _mhz, self.period = addrmap.SOURCES[src_name]
        self.level = addrmap.LEVELS[level or dev_level]
        self.name = src_name
        self.path = os.path.join(directory, "%s.hettrace" % src_name)
        self.filtered_dram = filtered_dram
        self.axi_data_bytes = (
            axi_data_bytes
            if axi_data_bytes is not None
            else (16 if src_name == "coralnpu" else 64)
        )
        self.synth_flag = synth_flag
        self._recs = []
        self._seq = 0
        self._txn = 0
        self._stats = {
            "emitted": 0, "data_records": 0, "transactions": 0,
            "filtered": 0, "unmapped": 0,
            "non_monotonic": 0, "bytes": 0, "first_tick": 0, "last_tick": 0,
        }

    def _push(self, tick, addr, strb, size, ctx, txn, axi_id, op, chan,
              axi_len, axi_size, resp, flags, seq=None, burst=_BURST_INCR):
        if addrmap.region_of(addr) is None:
            flags |= FLAG_UNMAPPED
            self._stats["unmapped"] += 1
        if self.filtered_dram and not addrmap.is_traced(addr):
            self._stats["filtered"] += 1
            return
        if self.synth_flag:
            flags |= FLAG_SYNTH
        if self._stats["emitted"] == 0:
            self._stats["first_tick"] = tick
        elif tick < self._stats["last_tick"]:
            self._stats["non_monotonic"] += 1
        self._stats["last_tick"] = tick
        self._stats["emitted"] += 1
        if is_data_chan(chan):
            self._stats["data_records"] += 1
            self._stats["bytes"] += size
        if chan in (CHAN_AW, CHAN_AR):
            self._stats["transactions"] += 1

        s = self._seq if seq is None else seq
        self._seq = s + 1
        self._recs.append(_RECORD_STRUCT.pack(
            tick, addr, strb, size, ctx, s, txn, self.src_id, axi_id,
            op, chan, axi_len, axi_size, burst, resp, self.src_id,
            flags, 0,
        ))

    def emit(self, tick, addr, size, op, ctx=0, flags=0, seq=None):
        """一次访问 = 一条数据通道记录。与 C++ TraceWriter::Emit 逐字段等价。"""
        self._push(
            tick, addr,
            _full_strb(size) if op == OP_WRITE else 0,
            size, ctx, self._txn, ctx & 0xFFFF, op,
            CHAN_W if op == OP_WRITE else CHAN_R,
            0, _log2_size(size), 0, flags | FLAG_LAST, seq,
        )
        self._txn += 1

    def emit_txn(self, req_tick, resp_tick, addr, nbytes, op, ctx=0,
                 axi_id=0, resp=0, flags=0, complete=True,
                 burst=_BURST_INCR):
        """一笔完整 AXI 事务：写 AW+W…+B，读 AR+…+R。

        读的 R 记在 resp_tick 而不是 req_tick —— 这正是 monitor 那一级比设备
        tap 多出来的信息，也是唯一能让下游看出读延迟的地方。

        complete=False 只写地址通道，模拟"仿真在事务在途时结束"。
        """
        beat = self.axi_data_bytes
        if (
            nbytes <= 0 or nbytes % beat or addr % beat
            or burst not in (0, 1, 2)
        ):
            raise ValueError(
                "完整 AXI 合成事务必须地址对齐且为整拍: "
                "addr=0x%x bytes=%d beat=%d burst=%d"
                % (addr, nbytes, beat, burst)
            )
        beats = nbytes // beat
        if beats > 256 or (burst == 0 and beats > 16) or (
            burst == 2 and beats not in (2, 4, 8, 16)
        ):
            raise ValueError("AXI burst 拍数/类型非法: beats=%d burst=%d" % (
                beats, burst
            ))
        axi_len = beats - 1
        axi_size = _log2_size(beat)
        txn = self._txn
        self._txn += 1
        write = op == OP_WRITE

        self._push(req_tick, addr, 0, nbytes, ctx, txn, axi_id, op,
                   CHAN_AW if write else CHAN_AR, axi_len, axi_size, 0,
                   flags, burst=burst)
        if not complete:
            return txn
        data_tick = req_tick if write else resp_tick
        for i in range(beats):
            self._push(
                data_tick,
                _axi_beat_address(addr, beat, beats, burst, i),
                _full_strb(beat) if write else 0,
                beat, ctx, txn, axi_id, op, CHAN_W if write else CHAN_R,
                axi_len, axi_size, 0 if write else resp,
                flags
                | (FLAG_BURST_BEAT if i else 0)
                | (FLAG_LAST if i + 1 == beats else 0),
                burst=burst,
            )
        if write:
            self._push(resp_tick, addr, 0, 0, ctx, txn, axi_id, op, CHAN_B,
                       axi_len, axi_size, resp, flags | FLAG_LAST,
                       burst=burst)
        return txn

    def close(self, write_meta=True, truncate_records=0):
        """truncate_records>0 时故意少写若干条，模拟缓冲未刷出的截断 trace。"""
        hdr = _HEADER_STRUCT.pack(
            MAGIC,
            FORMAT_VERSION,
            RECORD_SIZE,
            addrmap.TICKS_PER_SECOND,
            self.period,
            self.src_id,
            self.level,
            HDR_FILTERED_DRAM if self.filtered_dram else 0,
            self.axi_data_bytes,
            addrmap.MAP_ADDR_BITS,
            self.name.encode()[:23],
        )
        recs = self._recs
        if truncate_records:
            recs = recs[:-truncate_records]
        with open(self.path, "wb") as f:
            f.write(hdr)
            f.write(b"".join(recs))
        if write_meta:
            with open(self.path + ".meta.json", "w") as f:
                f.write("{\n")
                f.write('  "src_id": %d,\n' % self.src_id)
                f.write('  "name": "%s",\n' % self.name)
                f.write('  "level": %d,\n' % self.level)
                f.write('  "format": "bin",\n')
                f.write('  "filter": "%s",\n' % ("dram" if self.filtered_dram else "all"))
                f.write('  "synth": %s,\n' % ("true" if self.synth_flag else "false"))
                f.write('  "axi_data_bytes": %d,\n' % self.axi_data_bytes)
                f.write('  "axi_addr_bits": %d,\n' % addrmap.MAP_ADDR_BITS)
                f.write('  "ticks_per_second": %d,\n' % addrmap.TICKS_PER_SECOND)
                f.write('  "clock_period_ticks": %d,\n' % self.period)
                for k in ("emitted", "data_records", "transactions", "filtered",
                          "unmapped", "non_monotonic", "bytes", "first_tick"):
                    f.write('  "%s": %d,\n' % (k, self._stats[k]))
                f.write('  "last_tick": %d\n' % self._stats["last_tick"])
                f.write("}\n")
        return self.path


def gen_cooperative(directory, n_per_src=200):
    """健康形态：host 分别经 shared buffer 与 BAR 向两个设备交接。

    三者时间区间重叠；host+NPU 共享 shared_buffer，host+Vortex 共享
    vortex_bar。受地址宽度约束，不虚构 NPU 与 Vortex 的三方直连共享地址。
    """
    shared = addrmap.REGIONS["shared_buffer"][0]
    bar = addrmap.REGIONS["vortex_bar"][0]
    period = {n: addrmap.SOURCES[n][3] for n in ("host", "vortex", "coralnpu")}

    # host：分别填充 NPU shared buffer 与 Vortex BAR
    h = SynthWriter(
        directory, "host", level="interconnect", synth_flag=True
    )
    for i in range(n_per_src):
        h.emit(1000 + i * 10 * period["host"], shared + i * 64, 64, OP_WRITE, ctx=0)
        h.emit(1000 + (i * 10 + 5) * period["host"],
               bar + 0x10000 + i * 64, 64, OP_WRITE, ctx=0)
    h.close()

    # vortex：经 BAR 读取 host 输入、访问 scratch，再写结果
    v = SynthWriter(
        directory, "vortex", level="interconnect", synth_flag=True
    )
    for i in range(n_per_src):
        base = 3000 + i * 12 * period["vortex"]
        v.emit(base, bar + 0x10000 + i * 64, 64, OP_READ, ctx=i % 8)
        v.emit(base + 2 * period["vortex"],
               bar + 0x30000 + i * 64, 64, OP_READ, ctx=i % 8)
        v.emit(base + 4 * period["vortex"],
               bar + 0x20000 + i * 64, 64, OP_WRITE, ctx=i % 8)
    v.close()

    # npu：读共享 buffer 的结果，写自己的工作区
    n = SynthWriter(
        directory, "coralnpu", level="interconnect", synth_flag=True
    )
    for i in range(n_per_src):
        base = 5000 + i * 15 * period["coralnpu"]
        n.emit(base, shared + i * 64, 16, OP_READ, ctx=1)
        n.emit(base + period["coralnpu"],
               addrmap.REGIONS["npu_work"][0] + i * 16, 16, OP_WRITE, ctx=1)
    n.close()
    return directory


def gen_bar_pair(directory, n=100):
    """健康形态，但只有两个源：host 与 Vortex 经 BAR 交接，不碰 shared_buffer。

    这是 run_vortex_shared.sh 的形态 —— vecadd 走 CP 提交路径，字节全在设备内存
    里，host 只能经 vortex_bar 碰到它们。validate 必须放行：交接区不是同一个，
    但交接是真的。
    """
    bar = addrmap.REGIONS["vortex_bar"][0]
    period = {n_: addrmap.SOURCES[n_][3] for n_ in ("host", "vortex")}

    # host 分两相：先经 BAR 把输入上传完，等设备跑完再经 BAR 读回结果。分相而不是
    # 交错，是因为记录必须按 tick 单调 —— 交错写法会让第 i+1 次上传的时间戳早于第
    # i 次读回，validate 会（正确地）报成 tick 回退。
    h = SynthWriter(directory, "host")
    for i in range(n):
        h.emit(1000 + i * 10 * period["host"], bar + 0x10000 + i * 64, 64, OP_WRITE)
    for i in range(n):
        h.emit(1000 + (n * 10 + i * 10) * period["host"],
               bar + 0x20000 + i * 64, 64, OP_READ)
    h.close()

    v = SynthWriter(directory, "vortex")
    for i in range(n):
        base = 3000 + i * 12 * period["vortex"]
        v.emit(base, bar + 0x10000 + i * 64, 64, OP_READ, ctx=i % 8)
        v.emit(base + 2 * period["vortex"], bar + 0x20000 + i * 64, 64, OP_WRITE,
               ctx=i % 8)
    v.close()
    return directory


def gen_axi_full(directory, n=20):
    """健康的互连级 AXI4 五通道 trace。

    这组数据专门覆盖设备 tap 看不到的那一层：AW/W/B 与 AR/R 由 txn 串起，
    读数据出现在响应 tick，而不是请求 tick。host/Vortex 的 AXI 字段来自 gem5
    packet 投影，所以都带 synth 标记。CoralNPU 的原生 seam 虽保留了
    地址/ID/WSTRB，但五通道事件、时序和其余属性仍是 monitor 重构。
    """
    shared = addrmap.REGIONS["shared_buffer"][0]
    bar = addrmap.REGIONS["vortex_bar"][0]
    npu_work = addrmap.REGIONS["npu_work"][0]

    host = SynthWriter(
        directory, "host", level="interconnect", axi_data_bytes=16,
        synth_flag=True
    )
    vortex = SynthWriter(
        directory, "vortex", level="interconnect", axi_data_bytes=16,
        synth_flag=True
    )
    npu = SynthWriter(
        directory, "coralnpu", level="interconnect", synth_flag=True
    )

    for i in range(n):
        base = 1000 + i * 2000
        # 偶数轮验证 host↔NPU shared_buffer，奇数轮验证
        # host↔Vortex BAR；每个源只访问其真实可达地址。
        host_addr = (shared + i * 64) if i % 2 == 0 else (bar + i * 64)
        vortex_addr = bar + i * 64
        npu_addr = (shared + i * 64) if i % 2 == 0 else (npu_work + i * 64)
        host.emit_txn(base, base + 300, host_addr, 64, OP_WRITE,
                      ctx=i % 4, axi_id=i % 16)
        vortex.emit_txn(base + 100, base + 700, vortex_addr, 64, OP_READ,
                        ctx=i % 8, axi_id=(i + 3) % 16)
        npu.emit_txn(base + 200, base + 600, npu_addr, 16, OP_READ,
                     ctx=i % 4, axi_id=i % 4)

    host.close()
    vortex.close()
    npu.close()
    return directory


def gen_broken_axi_orphan(directory):
    """失效形态：有地址通道，但一条 R 的 txn 找不到对应 AR。"""
    handoff = addrmap.REGIONS["vortex_bar"][0]

    host = SynthWriter(
        directory, "host", level="interconnect", axi_data_bytes=16,
        synth_flag=True
    )
    host.emit_txn(1000, 1300, handoff, 16, OP_WRITE, axi_id=1)
    host.close()

    vortex = SynthWriter(
        directory, "vortex", level="interconnect", axi_data_bytes=16,
        synth_flag=True
    )
    open_txn = vortex.emit_txn(
        1100, 0, handoff, 16, OP_READ, axi_id=2, complete=False
    )
    # 故意用另一个 txn 写回 R；原 AR 因而保持 open，这条 R 则成为 orphan。
    vortex._push(
        1200, handoff, 0, 16, 0, open_txn + 99, 2, OP_READ, CHAN_R,
        0, 4, 0, FLAG_LAST,
    )
    vortex.close()
    return directory


def gen_broken_no_sharing(directory, n=100):
    """失效形态：三个源各干各的，共享区无人触及。validate 必须报 ERROR。"""
    for name, reg in (("host", "host_heap"), ("vortex", "vortex_vram"),
                      ("coralnpu", "npu_work")):
        w = SynthWriter(directory, name)
        base = addrmap.REGIONS[reg][0]
        for i in range(n):
            w.emit(1000 + i * 100, base + i * 64, 64, OP_READ)
        w.close()
    return directory


def gen_broken_non_monotonic(directory, n=50):
    """失效形态：Vortex 的时间戳用了源内 cycle 而非全局 tick，出现回退。"""
    bar = addrmap.REGIONS["vortex_bar"][0]
    h = SynthWriter(directory, "host")
    for i in range(n):
        h.emit(1000 + i * 100, bar + i * 64, 64, OP_WRITE)
    h.close()

    v = SynthWriter(directory, "vortex")
    for i in range(n):
        tick = 1000 + i * 100
        if i == n // 2:
            tick = 500          # 回退
        v.emit(tick, bar + i * 64, 64, OP_READ)
    v.close()
    return directory


def gen_broken_serial(directory, n=50):
    """失效形态：三者串行执行，时间区间不重叠 —— 无争抢可分析，应报 WARN。"""
    shared = addrmap.REGIONS["shared_buffer"][0]
    bar = addrmap.REGIONS["vortex_bar"][0]
    for idx, name in enumerate(("host", "vortex", "coralnpu")):
        w = SynthWriter(directory, name)
        t0 = 1000 + idx * 10 ** 7
        for i in range(n):
            address = (shared if name == "coralnpu" else bar) + i * 64
            # host 需同时与两个设备形成真实交接，故交替触及两块窗口。
            if name == "host" and i % 2 == 0:
                address = shared + i * 64
            w.emit(t0 + i * 100, address, 64,
                   OP_WRITE if idx == 0 else OP_READ)
        w.close()
    return directory


def gen_broken_truncated(directory, n=50):
    """失效形态：meta 说 n 条，文件里只有 n-10 条 —— 进程被杀，缓冲丢了。"""
    bar = addrmap.REGIONS["vortex_bar"][0]
    h = SynthWriter(directory, "host")
    for i in range(n):
        h.emit(1000 + i * 100, bar + i * 64, 64, OP_WRITE)
    h.close()

    v = SynthWriter(directory, "vortex")
    for i in range(n):
        v.emit(1000 + i * 100, bar + i * 64, 64, OP_READ)
    v.close(truncate_records=10)
    return directory


def gen_broken_seq_gap(directory, n=50):
    """失效形态：seq 有洞 —— 记录在中途被丢弃。"""
    bar = addrmap.REGIONS["vortex_bar"][0]
    h = SynthWriter(directory, "host")
    for i in range(n):
        h.emit(1000 + i * 100, bar + i * 64, 64, OP_WRITE)
    h.close()

    v = SynthWriter(directory, "vortex")
    for i in range(n):
        seq = i if i < n // 2 else i + 7   # 断裂
        v.emit(1000 + i * 100, bar + i * 64, 64, OP_READ, seq=seq)
    v.close()
    return directory


SCENARIOS = {
    "cooperative": gen_cooperative,
    "axi_full": gen_axi_full,
    "axi_orphan": gen_broken_axi_orphan,
    "no_sharing": gen_broken_no_sharing,
    "non_monotonic": gen_broken_non_monotonic,
    "serial": gen_broken_serial,
    "truncated": gen_broken_truncated,
    "seq_gap": gen_broken_seq_gap,
}
