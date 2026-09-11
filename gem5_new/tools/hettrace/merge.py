"""按全局 tick 归并多源 trace。

每个源写独立文件、归并留到离线做，是刻意的设计（docs/02-trace-format.md）：
仿真过程中三个源在不同时钟域上被 gem5 事件队列交替唤醒，同一次 tick 内的写入
顺序取决于事件调度细节，在线交织写出的顺序是不可复现的。离线按
(tick, src_id, seq) 全序归并则是确定性的。

流式实现：heapq.merge 在 k 个已排序迭代器上做 k 路归并，内存占用 O(k)。
"""

from __future__ import annotations

import heapq

from . import addrmap
from .reader import CHAN_NAMES, OP_WRITE, discover, read_records


def _key(rec):
    # tick 为主序；同 tick 内按 src_id 再按源内 seq，保证全序且可复现。
    return (rec.tick, rec.src_id, rec.seq)


def merge_streams(paths):
    """把多个 trace 文件按全局 tick 归并成单个流。"""
    streams = [read_records(p) for p in paths]
    return heapq.merge(*streams, key=_key)


def merge_dir(directory):
    entries = discover(directory)
    return merge_streams([p for p, _h in entries]), entries


def write_text(records, out_fh, header_comment=True):
    """把归并后的流写成文本。列与单源文本格式一致，额外加源名与区域列便于阅读。

    归并保留全部五个通道。这是唯一一处"全量"视图 —— 下游要做访存分析时自己
    按 chan 投影（reader.is_data_chan），而不是让归并替它决定看得见什么。
    """
    if header_comment:
        out_fh.write("# hettrace merged\n")
        out_fh.write(
            "# tick src_name chan op addr size axi_id txn ctx seq flags region\n"
        )
    n = 0
    for r in records:
        src = addrmap.SRC_NAME_BY_ID.get(r.src_id, "src%d" % r.src_id)
        region = addrmap.region_of(r.addr) or "-"
        out_fh.write(
            "%d %s %s %s 0x%x %d %d %d %d %d 0x%02x %s\n"
            % (
                r.tick,
                src,
                CHAN_NAMES.get(r.chan, "??"),
                "W" if r.op == OP_WRITE else "R",
                r.addr,
                r.size,
                r.axi_id,
                r.txn,
                r.ctx,
                r.seq,
                r.flags,
                region,
            )
        )
        n += 1
    return n
