"""基本统计 —— 只做 trace-driven 前提下站得住的那些量。

刻意不做的事：不算延迟、不算 IPC、不算加速比。抓到的 trace 是"无争抢延迟时"
的访问模式（docs/03-limitations.md），任何依赖请求间时间差反馈的量都是错的。
这里只出与时序反馈无关的量：带宽、footprint、局部性、区域分布、跨源交叠。

一律走 read_data_records：这里所有的量都是"搬了多少字节到哪些地址"，
AW/AR/B 不搬字节，算进来会让带宽和 footprint 同时虚高。
"""

from __future__ import annotations

from collections import namedtuple

from . import addrmap
from .reader import OP_WRITE, discover, read_data_records

WindowStat = namedtuple("WindowStat", "start end per_src_bytes per_src_count")


def bandwidth_timeline(directory, window_ticks):
    """按固定 tick 窗口统计各源的字节数。

    带宽是 trace-driven 下最可信的量：它只依赖"哪些地址被访问了多少字节"，
    不依赖请求发出的精确时刻是否受争抢影响。
    """
    # window_ticks<=0 时下面的 start = end 永远推不动窗口 —— 表现是静默卡死，
    # 不是异常。CLI 已在参数层挡了一道，这里再挡一道给直接调库的人。
    if window_ticks <= 0:
        raise ValueError("window_ticks 必须为正，收到 %r" % (window_ticks,))
    entries = discover(directory)
    if not entries:
        return [], []
    frequencies = {hdr.ticks_per_second for _, hdr in entries}
    if len(frequencies) != 1 or next(iter(frequencies)) <= 0:
        raise ValueError("trace 必须使用相同且为正的 ticks_per_second")

    src_names = []
    all_recs = []
    for path, hdr in entries:
        name = addrmap.SRC_NAME_BY_ID.get(hdr.src_id, hdr.name)
        src_names.append(name)
        for r in read_data_records(path):
            all_recs.append((r.tick, name, r.size))

    if not all_recs:
        return [], src_names
    all_recs.sort(key=lambda t: t[0])

    t0 = all_recs[0][0]
    t_end = all_recs[-1][0]
    windows = []
    idx = 0
    start = t0
    while start <= t_end:
        end = start + window_ticks
        per_bytes = dict((n, 0) for n in src_names)
        per_count = dict((n, 0) for n in src_names)
        while idx < len(all_recs) and all_recs[idx][0] < end:
            _tick, name, size = all_recs[idx]
            per_bytes[name] += size
            per_count[name] += 1
            idx += 1
        windows.append(WindowStat(start, end, per_bytes, per_count))
        start = end
    return windows, src_names


def footprint(directory, line_bytes=64):
    """各源触及的唯一 cache line 数，以及两两之间的共享 line 数。

    共享 line 数是"异构协同是否真实发生"的定量证据 —— 比看区域分布更硬。
    """
    if line_bytes <= 0:
        raise ValueError("line_bytes 必须为正，收到 %r" % (line_bytes,))
    entries = discover(directory)
    lines = {}
    for path, hdr in entries:
        name = addrmap.SRC_NAME_BY_ID.get(hdr.src_id, hdr.name)
        s = set()
        for r in read_data_records(path):
            lo = r.addr // line_bytes
            hi = (r.addr + max(r.size, 1) - 1) // line_bytes
            for ln in range(lo, hi + 1):
                s.add(ln)
        lines[name] = s

    names = sorted(lines)
    pairwise = {}
    for i in range(len(names)):
        for j in range(i + 1, len(names)):
            a, b = names[i], names[j]
            pairwise[(a, b)] = len(lines[a] & lines[b])
    sizes = dict((n, len(s)) for n, s in lines.items())
    return sizes, pairwise, line_bytes


def rw_and_region_breakdown(directory):
    entries = discover(directory)
    out = {}
    for path, hdr in entries:
        name = addrmap.SRC_NAME_BY_ID.get(hdr.src_id, hdr.name)
        reads = writes = rbytes = wbytes = 0
        regions = {}
        for r in read_data_records(path):
            reg = addrmap.region_of(r.addr) or "<unmapped>"
            d = regions.setdefault(reg, [0, 0, 0, 0])
            if r.op == OP_WRITE:
                writes += 1
                wbytes += r.size
                d[1] += 1
                d[3] += r.size
            else:
                reads += 1
                rbytes += r.size
                d[0] += 1
                d[2] += r.size
        out[name] = {
            "reads": reads,
            "writes": writes,
            "read_bytes": rbytes,
            "write_bytes": wbytes,
            "regions": regions,
        }
    return out


def format_report(directory, window_ticks=1000000, line_bytes=64):
    entries = discover(directory)
    frequency = entries[0][1].ticks_per_second if entries else addrmap.TICKS_PER_SECOND
    L = []
    a = L.append
    a("=" * 72)
    a("hettrace 统计")
    a("=" * 72)
    a("")

    br = rw_and_region_breakdown(directory)
    if not br:
        a("目录下无 trace。")
        return "\n".join(L)

    a("读写构成:")
    a("  %-10s %10s %10s %14s %14s" % ("源", "读", "写", "读字节", "写字节"))
    for name, d in sorted(br.items()):
        a(
            "  %-10s %10d %10d %14d %14d"
            % (name, d["reads"], d["writes"], d["read_bytes"], d["write_bytes"])
        )
    a("")

    sizes, pairwise, lb = footprint(directory, line_bytes)
    a("Footprint (%d 字节 line):" % lb)
    for name, n in sorted(sizes.items()):
        a("  %-10s %10d 条唯一 line  (%.1f KiB)" % (name, n, n * lb / 1024.0))
    a("")
    a("两两共享 line 数 —— 异构协同真实发生的定量证据:")
    if not pairwise:
        a("  (源不足两个)")
    for (x, y), n in sorted(pairwise.items()):
        tag = "" if n else "   <-- 无共享"
        a("  %-10s <-> %-10s %10d%s" % (x, y, n, tag))
    a("")

    windows, src_names = bandwidth_timeline(directory, window_ticks)
    a(
        "带宽时间线 (窗口 %d tick = %.3f us, 共 %d 窗):"
        % (window_ticks, window_ticks / (frequency / 1e6), len(windows))
    )
    if windows:
        a("  %-16s %s" % ("窗口起始", " ".join("%12s" % n for n in src_names)))
        shown = windows if len(windows) <= 20 else windows[:10] + windows[-10:]
        prev_end = None
        for w in shown:
            if prev_end is not None and w.start != prev_end:
                a("  ... 略 ...")
            a(
                "  %-16d %s"
                % (
                    w.start,
                    " ".join("%12d" % w.per_src_bytes.get(n, 0) for n in src_names),
                )
            )
            prev_end = w.end
        # 争抢窗口数：同一窗口内 ≥2 个源都有流量
        overlap = sum(
            1
            for w in windows
            if sum(1 for n in src_names if w.per_src_count.get(n, 0) > 0) >= 2
        )
        a("")
        a(
            "  多源并发窗口: %d / %d (%.1f%%)"
            % (overlap, len(windows), 100.0 * overlap / max(len(windows), 1))
        )
        a("  ↑ 该比例是共享存储争抢研究的有效样本占比；过低说明负载并发度不够。")
    a("=" * 72)
    return "\n".join(L)
