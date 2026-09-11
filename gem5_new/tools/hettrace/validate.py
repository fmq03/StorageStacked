"""trace 校验。

这个工具的存在理由：trace-driven 研究里最贵的错误是拿到一批"看起来正常"的
trace，分析了两周才发现时间基准接错了 / 地址没对齐到同一映射 / 三个源其实是
串行跑的。这里把那些失效模式全部前置成显式检查。

严重级别：
    ERROR — trace 不可用于分析，必须修
    WARN  — 可用但结论受限，必须在报告里写明
"""

from __future__ import annotations

from collections import deque, namedtuple

from . import addrmap
from .reader import (
    CHAN_AR,
    CHAN_AW,
    CHAN_B,
    CHAN_NAMES,
    CHAN_R,
    CHAN_W,
    FLAG_BURST_BEAT,
    FLAG_LAST,
    FLAG_SYNTH,
    FLAG_UNMAPPED,
    OP_WRITE,
    TraceError,
    discover,
    is_data_chan,
    read_meta,
    read_records,
)

Issue = namedtuple("Issue", "level source message")

SrcSummary = namedtuple(
    "SrcSummary",
    "name src_id level count reads writes bytes first_tick last_tick "
    "regions unmapped burst_beats data_records transactions chan_counts "
    "resp_errors synth ticks_per_second",
)

AxiCheck = namedtuple(
    "AxiCheck",
    "unmatched_data unmatched_resp duplicate_addr leftover_open protocol_errors",
)


_AXI_ERROR_TEXT = {
    "source_id": "记录 src_id 与文件头不一致",
    "unknown_channel": "出现未定义的 AXI 通道编号",
    "channel_op": "通道与 R/W op 不一致",
    "control_shape": "控制/响应通道的 size/strb/LAST/RESP 形状非法",
    "address_geometry": "AxLEN/AxSIZE 与事务字节数不一致",
    "address_alignment": "AW/AR 首地址未按 AxSIZE 对齐（当前可转换契约不支持 unaligned burst）",
    "beat_width": "AxSIZE 超出文件头声明的 AXI 数据总线宽度",
    "bad_burst": "BURST 编码或 WRAP 几何非法",
    "bad_resp": "RESP 不是 AXI4 定义的 OKAY/EXOKAY/SLVERR/DECERR",
    "cross_4k": "AXI burst 跨越 4KiB 边界",
    "field_mismatch": "数据/响应通道的 ID/op/LEN/SIZE/BURST/USER/ctx 与 AW/AR 不一致",
    "beat_size": "W/R 数据拍大小与 AxSIZE 不一致",
    "beat_address": "W/R 数据拍地址不符合 FIXED/INCR/WRAP 次序",
    "beat_flags": "LAST 或 BURST_BEAT 不在正确的数据拍上",
    "write_strobe": "WSTRB 超出总线宽或本拍合法 lane",
    "read_strobe": "R 通道的 strb 必须为 0",
    "extra_data": "事务的 W/R 数据拍数超过 AxLEN+1",
    "missing_data": "B 响应到达时 W 数据拍数不是 AxLEN+1",
    "write_data_order": "AXI4 W 数据没有遵守 AW 的全局发出顺序",
    "response_order": "同一 AXI ID 的 B/R 响应发生重排",
    "address_span": "记录的地址范围溢出或跨越已声明 region",
    "unmapped_flag": "UNMAPPED flag 与 addrmap 分类不一致",
    "mixed_synth": "同一源文件混用 SYNTH 与非 SYNTH 记录",
}


def _summarize(path, hdr):
    count = reads = writes = nbytes = 0
    data_records = 0
    first_tick = last_tick = None
    non_monotonic = 0
    seq_gaps = 0
    expected_seq = 0
    regions = {}
    unmapped = 0
    burst_beats = 0
    bad_accessor = {}
    chan_counts = dict((c, 0) for c in CHAN_NAMES)
    resp_errors = 0
    synth_records = 0

    # AXI 结构检查。open_txn 只装"已开始未完成"的事务，规模等于 outstanding
    # 深度，不随 trace 长度增长 —— validate 必须能跑在几十 GB 的 trace 上。
    open_txn = {}
    write_data_order = deque()
    write_resp_order = {}
    read_resp_order = {}
    # 设备级 tap 只有 W/R，没有 AW/AR 可提供几何真值。仍按 txn/len 重建
    # 数据拍组，至少保证每条记录的宽度、strobe 和 LAST/BURST_BEAT 自洽。
    # 合法 trace 中这里只保存尚未收齐的数据拍，空间随 outstanding 深度增长，
    # 不随文件总长度增长。
    device_txn = {}
    saw_addr_channel = False
    unbound_data = 0
    unmatched_resp = 0
    duplicate_addr = 0
    protocol_errors = {}

    def bad(category, count_=1):
        protocol_errors[category] = protocol_errors.get(category, 0) + count_

    def expected_beat_addr(state, index):
        if state["burst"] == 0:  # FIXED
            return state["addr"]
        if state["burst"] == 1:  # INCR
            return state["addr"] + index * state["beat_bytes"]
        if state["burst"] == 2:  # WRAP
            span = state["expected"] * state["beat_bytes"]
            boundary = (state["addr"] // span) * span
            return boundary + (
                (state["addr"] - boundary + index * state["beat_bytes"])
                % span
            )
        return state["addr"]

    def common_fields_match(r, state):
        return (
            r.op == state["op"]
            and r.axi_id == state["axi_id"]
            and r.axi_len == state["axi_len"]
            and r.axi_size == state["axi_size"]
            and r.burst == state["burst"]
            and r.user == state["user"]
            and r.ctx == state["ctx"]
        )

    def id_queue(table, axi_id):
        queue = table.get(axi_id)
        if queue is None:
            queue = deque()
            table[axi_id] = queue
        return queue

    def remove_queued(queue, txn):
        if queue and queue[0] == txn:
            queue.popleft()
            return
        try:
            queue.remove(txn)
        except ValueError:
            pass

    src_name = hdr.name
    # 权限始终以 src_id 反查；validate_dir 还会单独要求 name 与规范名一致。
    canonical = addrmap.SRC_NAME_BY_ID.get(hdr.src_id)

    for r in read_records(path):
        count += 1
        chan_counts[r.chan] = chan_counts.get(r.chan, 0) + 1

        if r.src_id != hdr.src_id:
            bad("source_id")
        if r.chan not in CHAN_NAMES:
            bad("unknown_channel")
            continue

        expected_op = OP_WRITE if r.chan in (CHAN_AW, CHAN_W, CHAN_B) else 0
        if r.op != expected_op:
            bad("channel_op")

        if first_tick is None:
            first_tick = r.tick
        elif r.tick < last_tick:
            non_monotonic += 1
        last_tick = r.tick

        if r.seq != expected_seq:
            seq_gaps += 1
            expected_seq = r.seq
        expected_seq += 1

        actual_region = addrmap.region_of(r.addr)
        if r.flags & FLAG_UNMAPPED:
            unmapped += 1
        if bool(r.flags & FLAG_UNMAPPED) != (actual_region is None):
            bad("unmapped_flag")
        if r.flags & FLAG_BURST_BEAT:
            burst_beats += 1
        if r.flags & FLAG_SYNTH:
            synth_records += 1

        # AXI 通道结构。地址通道开事务，B / 最后一拍 R 关事务。
        if r.resp not in (0, 1, 2, 3):
            bad("bad_resp")

        if r.chan == CHAN_AW or r.chan == CHAN_AR:
            saw_addr_channel = True
            if r.txn in open_txn:
                duplicate_addr += 1
            beat_bytes = 1 << r.axi_size
            beats = r.axi_len + 1
            if beat_bytes > hdr.axi_data_bytes or hdr.axi_data_bytes <= 0:
                bad("beat_width")
            if r.addr % beat_bytes != 0:
                # HETTrace 当前没有 RSTRB，无法无损表达 unaligned 读的
                # 首拍有效 lane。统一 monitor 会先缩小 AxSIZE 直至地址对齐；
                # 其他生产者也必须遵守这一可转换子集。
                bad("address_alignment")
            if r.size != beat_bytes * beats:
                bad("address_geometry")
            if r.strb != 0 or r.resp != 0 or (r.flags & FLAG_LAST):
                bad("control_shape")
            if r.burst not in (0, 1, 2):
                bad("bad_burst")
            if r.burst == 0 and beats > 16:
                bad("bad_burst")
            if r.burst == 2 and (
                beats not in (2, 4, 8, 16) or r.addr % beat_bytes != 0
            ):
                bad("bad_burst")

            state = {
                "addr_chan": r.chan,
                "data_chan": CHAN_W if r.chan == CHAN_AW else CHAN_R,
                "op": r.op,
                "addr": r.addr,
                "axi_id": r.axi_id,
                "axi_len": r.axi_len,
                "axi_size": r.axi_size,
                "burst": r.burst,
                "user": r.user,
                "ctx": r.ctx,
                "beat_bytes": beat_bytes,
                "expected": beats,
                "seen": 0,
            }
            # AXI 不允许一笔 burst 跨 4KiB；按每拍的真实地址判定，
            # 同时涵盖 WRAP/FIXED，不用只对 INCR 成立的 end-start 捷径。
            span_bad = False
            for beat_index in range(beats):
                beat_addr = expected_beat_addr(state, beat_index)
                if (beat_addr >> 12) != ((beat_addr + beat_bytes - 1) >> 12):
                    bad("cross_4k")
                    break
                if (beat_addr >> 12) != (r.addr >> 12):
                    bad("cross_4k")
                    break
                address_limit = (
                    1 << hdr.axi_addr_bits
                    if 0 < hdr.axi_addr_bits <= 64 else 0
                )
                beat_end = beat_addr + beat_bytes - 1
                if (
                    not address_limit
                    or beat_end >= address_limit
                    or addrmap.region_of(beat_addr) != actual_region
                    or addrmap.region_of(beat_end) != actual_region
                ):
                    span_bad = True
            if span_bad:
                bad("address_span")
            open_txn[r.txn] = state
            if r.chan == CHAN_AW:
                write_data_order.append(r.txn)
                id_queue(write_resp_order, r.axi_id).append(r.txn)
            else:
                id_queue(read_resp_order, r.axi_id).append(r.txn)
        elif r.chan == CHAN_B:
            state = open_txn.get(r.txn)
            if state is None or state["addr_chan"] != CHAN_AW:
                unmatched_resp += 1
            else:
                if not common_fields_match(r, state) or r.addr != state["addr"]:
                    bad("field_mismatch")
                if r.size != 0 or r.strb != 0 or not (r.flags & FLAG_LAST):
                    bad("control_shape")
                if state["seen"] != state["expected"]:
                    bad("missing_data")
                response_queue = id_queue(write_resp_order, r.axi_id)
                if not response_queue or response_queue[0] != r.txn:
                    bad("response_order")
                remove_queued(response_queue, r.txn)
                # 缺 W 就收到 B 时，把残留的数据顺序状态也移除，避免一个
                # 已经报错的事务让后续所有合法 W 都产生级联误报。
                remove_queued(write_data_order, r.txn)
                open_txn.pop(r.txn, None)
            if r.resp in (2, 3):
                resp_errors += 1

        if not is_data_chan(r.chan):
            continue

        # ---- 以下只对数据通道成立，即旧格式语义的投影 ----
        data_records += 1
        if r.op == OP_WRITE:
            writes += 1
        else:
            reads += 1
        nbytes += r.size
        if r.resp in (2, 3):
            resp_errors += 1

        state = open_txn.get(r.txn)
        if state is None:
            # 到文件结尾才知道它是纯 W/R 设备投影还是混入了完整
            # 五通道事务。先记账；纯设备级的文件不报孤儿，但也不能因此
            # 跳过数据拍自身的 AXI/投影形状检查。
            unbound_data += 1
            state = device_txn.get(r.txn)
            if state is None:
                beat_bytes = 1 << r.axi_size
                state = {
                    "addr_chan": None,
                    "data_chan": r.chan,
                    "op": r.op,
                    "addr": r.addr,
                    "axi_id": r.axi_id,
                    "axi_len": r.axi_len,
                    "axi_size": r.axi_size,
                    "burst": r.burst,
                    "user": r.user,
                    "ctx": r.ctx,
                    "beat_bytes": beat_bytes,
                    "expected": r.axi_len + 1,
                    "seen": 0,
                }
                device_txn[r.txn] = state

                if r.burst not in (0, 1, 2):
                    bad("bad_burst")
                if r.burst == 0 and state["expected"] > 16:
                    bad("bad_burst")
                if r.burst == 2 and (
                    state["expected"] not in (2, 4, 8, 16)
                    or r.addr % beat_bytes != 0
                ):
                    bad("bad_burst")

            if r.chan != state["data_chan"] or not common_fields_match(r, state):
                bad("field_mismatch")

            index = state["seen"]
            if index >= state["expected"]:
                bad("extra_data")
            else:
                width = hdr.axi_data_bytes
                beat_bytes = state["beat_bytes"]
                if r.size <= 0:
                    bad("beat_size")
                if width <= 0 or r.size > width or beat_bytes > width:
                    bad("beat_width")

                # TraceWriter::EmitBurst 是真 burst 投影，故每拍必须严格等于
                # 2**AxSIZE。Emit() 的 axi_len=0 兼容旧的“一次访问一条”语义；
                # CoralNPU 会把稀疏 WSTRB 拆成 3B/5B 等连续区间，此时 AxSIZE
                # 明确定义为 floor(log2(size))，而 size 仍保存真实字节数。
                if r.axi_len:
                    if r.size != beat_bytes:
                        bad("beat_size")
                elif r.size > 0 and r.axi_size != r.size.bit_length() - 1:
                    bad("beat_size")

                if r.addr != expected_beat_addr(state, index):
                    bad("beat_address")
                final = index + 1 == state["expected"]
                if bool(r.flags & FLAG_LAST) != final:
                    bad("beat_flags")
                if bool(r.flags & FLAG_BURST_BEAT) != (index != 0):
                    bad("beat_flags")

                if r.chan == CHAN_W:
                    if (
                        width <= 0
                        or width > 64
                        or r.strb >> width
                        or (0 < r.size < 64 and r.strb >> r.size)
                    ):
                        bad("write_strobe")
                    if r.resp != 0:
                        bad("control_shape")
                elif r.strb != 0:
                    bad("read_strobe")

                state["seen"] += 1
                # 即使 LAST 写错，也按声明的拍数关闭，既能报错又不会让一份
                # 长的坏 trace 把校验器状态无限撑大。
                if state["seen"] == state["expected"]:
                    device_txn.pop(r.txn, None)
        elif r.chan != state["data_chan"]:
            unbound_data += 1
        else:
            if not common_fields_match(r, state):
                bad("field_mismatch")
            index = state["seen"]
            if r.chan == CHAN_W and (
                not write_data_order or write_data_order[0] != r.txn
            ):
                bad("write_data_order")
            if r.chan == CHAN_R:
                response_queue = id_queue(read_resp_order, r.axi_id)
                if not response_queue or response_queue[0] != r.txn:
                    bad("response_order")
            if index >= state["expected"]:
                bad("extra_data")
            else:
                if r.size != state["beat_bytes"]:
                    bad("beat_size")
                if r.addr != expected_beat_addr(state, index):
                    bad("beat_address")
                final = index + 1 == state["expected"]
                if bool(r.flags & FLAG_LAST) != final:
                    bad("beat_flags")
                if bool(r.flags & FLAG_BURST_BEAT) != (index != 0):
                    bad("beat_flags")
                if r.chan == CHAN_W:
                    width = hdr.axi_data_bytes
                    if (
                        width <= 0
                        or width > 64
                        or state["beat_bytes"] > width
                        or r.strb >> width
                    ):
                        bad("write_strobe")
                    else:
                        allowed = 0
                        for byte in range(state["beat_bytes"]):
                            allowed |= 1 << ((r.addr + byte) % width)
                        if r.strb & ~allowed:
                            bad("write_strobe")
                    if r.resp != 0:
                        bad("control_shape")
                elif r.strb != 0:
                    bad("read_strobe")
                state["seen"] += 1
                if r.chan == CHAN_W and state["seen"] == state["expected"]:
                    remove_queued(write_data_order, r.txn)
                if r.chan == CHAN_R and state["seen"] == state["expected"]:
                    remove_queued(
                        id_queue(read_resp_order, r.axi_id), r.txn
                    )
                    open_txn.pop(r.txn, None)

        reg = actual_region
        key = reg or "<unmapped>"
        regions[key] = regions.get(key, 0) + 1

        if r.size > 0:
            end_addr = r.addr + r.size - 1
            address_limit = 1 << hdr.axi_addr_bits if 0 < hdr.axi_addr_bits <= 64 else 0
            if (
                not address_limit
                or end_addr >= address_limit
                or addrmap.region_of(end_addr) != reg
            ):
                bad("address_span")

        if reg is not None and canonical is not None:
            if canonical not in addrmap.REGIONS[reg][3]:
                bad_accessor[reg] = bad_accessor.get(reg, 0) + 1

    if not saw_addr_channel and device_txn:
        bad("missing_data", len(device_txn))

    if 0 < synth_records < count:
        bad("mixed_synth")

    summary = SrcSummary(
        name=src_name,
        src_id=hdr.src_id,
        level=hdr.level,
        count=count,
        reads=reads,
        writes=writes,
        bytes=nbytes,
        first_tick=first_tick or 0,
        last_tick=last_tick or 0,
        regions=regions,
        unmapped=unmapped,
        burst_beats=burst_beats,
        data_records=data_records,
        transactions=chan_counts.get(CHAN_AW, 0) + chan_counts.get(CHAN_AR, 0),
        chan_counts=chan_counts,
        resp_errors=resp_errors,
        synth=synth_records == count and count > 0,
        ticks_per_second=hdr.ticks_per_second,
    )
    axi = AxiCheck(
        unmatched_data=unbound_data if saw_addr_channel else 0,
        unmatched_resp=unmatched_resp,
        duplicate_addr=duplicate_addr,
        leftover_open=len(open_txn),
        protocol_errors=protocol_errors,
    )
    return summary, non_monotonic, seq_gaps, bad_accessor, axi


def validate_dir(directory, require_heterogeneous=True,
                 ticks_per_second=addrmap.TICKS_PER_SECOND):
    """返回 ``(issues, summaries)``。

    默认要求至少两个来源和真实交接区，用于正式异构分析。独立设备 bring-up
    可显式传 ``require_heterogeneous=False``；单源本身不再报错，但其文件内的
    格式、时钟、地址、序号和 AXI 因果检查完全相同。
    """
    issues = []
    summaries = []

    try:
        entries = discover(directory)
    except (OSError, TraceError) as e:
        return [Issue("ERROR", "-", "无法读取目录 %s: %s" % (directory, e))], []

    if not entries:
        return [Issue("ERROR", "-", "%s 下没有 hettrace 文件" % directory)], []

    seen_src_ids = {}
    for path, hdr in entries:
        previous = seen_src_ids.get(hdr.src_id)
        if previous is not None:
            issues.append(
                Issue(
                    "ERROR",
                    hdr.name,
                    "src_id=%d 与 %s 重复（当前文件 %s）—— "
                    "来源身份不唯一，无法安全归并"
                    % (hdr.src_id, previous, path),
                )
            )
        else:
            seen_src_ids[hdr.src_id] = path

    for path, hdr in entries:
        src = hdr.name
        if (
            hdr.axi_data_bytes <= 0
            or hdr.axi_data_bytes > 64
            or hdr.axi_data_bytes & (hdr.axi_data_bytes - 1)
        ):
            issues.append(
                Issue(
                    "ERROR",
                    src,
                    "文件头 axi_data_bytes=%d，必须是 [1,64] 内的 2 次幂"
                    % hdr.axi_data_bytes,
                )
            )
        if hdr.axi_addr_bits <= 0 or hdr.axi_addr_bits > 64:
            issues.append(
                Issue(
                    "ERROR",
                    src,
                    "文件头 axi_addr_bits=%d，必须在 [1,64]"
                    % hdr.axi_addr_bits,
                )
            )
        if (
            ticks_per_second <= 0
            or hdr.ticks_per_second != ticks_per_second
            or hdr.clock_period_ticks <= 0
        ):
            issues.append(
                Issue(
                    "ERROR",
                    src,
                    "文件头时钟基准非法：ticks_per_second=%d（应为 %d），"
                    "clock_period_ticks=%d"
                    % (
                        hdr.ticks_per_second,
                        ticks_per_second,
                        hdr.clock_period_ticks,
                    ),
                )
            )
        try:
            summary, non_mono, seq_gaps, bad_accessor, axi = _summarize(path, hdr)
        except TraceError as e:
            issues.append(Issue("ERROR", src, str(e)))
            continue
        summaries.append(summary)

        if summary.count == 0:
            issues.append(
                Issue("ERROR", src, "trace 为空 —— tap 没接上，或负载没触及该源")
            )
            continue

        # 时间基准接错的直接证据。
        if non_mono:
            issues.append(
                Issue(
                    "ERROR",
                    src,
                    "%d 次 tick 回退 —— 时间戳没有取自 gem5 全局 tick，"
                    "很可能误用了源内 cycle 计数" % non_mono,
                )
            )

        if seq_gaps:
            issues.append(
                Issue(
                    "ERROR",
                    src,
                    "seq 有 %d 处断裂 —— 记录丢失（缓冲未刷出 / 进程被杀）" % seq_gaps,
                )
            )

        # 与 meta 侧车交叉核对，捕捉截断和“记录被过滤后
        # 只在 meta 里留下”的地址错误。
        meta_parse_error = False
        try:
            meta = read_meta(path)
        except (OSError, ValueError) as exc:
            issues.append(
                Issue("ERROR", src, "meta.json 无法解析：%s" % exc)
            )
            meta = None
            meta_parse_error = True
        if meta is None:
            if not meta_parse_error:
                issues.append(
                    Issue("WARN", src, "缺少 meta.json 侧车，无法交叉核对记录数")
                )
        else:
            exact_meta = {
                "emitted": summary.count,
                "data_records": summary.data_records,
                "transactions": summary.transactions,
                "bytes": summary.bytes,
                "non_monotonic": non_mono,
                "first_tick": summary.first_tick,
                "last_tick": summary.last_tick,
            }
            for field, observed in exact_meta.items():
                if meta.get(field) != observed:
                    issues.append(
                        Issue(
                            "ERROR",
                            src,
                            "meta.%s=%r，文件重算为 %d —— "
                            "trace 可能被截断，或侧车与 trace 不一致"
                            % (field, meta.get(field), observed),
                        )
                    )

            meta_unmapped = meta.get("unmapped")
            meta_filtered = meta.get("filtered")
            if not isinstance(meta_filtered, int) or meta_filtered < 0:
                issues.append(
                    Issue(
                        "ERROR", src,
                        "meta.filtered=%r，必须是非负整数" % meta_filtered,
                    )
                )
            if not isinstance(meta_unmapped, int) or meta_unmapped < summary.unmapped:
                issues.append(
                    Issue(
                        "ERROR",
                        src,
                        "meta.unmapped=%r，文件内已有 %d 条 UNMAPPED 记录"
                        % (meta_unmapped, summary.unmapped),
                    )
                )
            elif meta_unmapped > summary.unmapped:
                hidden = meta_unmapped - summary.unmapped
                if not hdr.filtered_dram:
                    issues.append(
                        Issue(
                            "ERROR", src,
                            "filter=all 时 meta.unmapped=%d 不应大于文件内的 %d"
                            % (meta_unmapped, summary.unmapped),
                        )
                    )
                elif isinstance(meta_filtered, int) and meta_filtered < hidden:
                    issues.append(
                        Issue(
                            "ERROR", src,
                            "meta.filtered=%d 小于被过滤的未映射记录 %d"
                            % (meta_filtered, hidden),
                        )
                    )
                issues.append(
                    Issue(
                        "ERROR",
                        src,
                        "meta 记录 %d 次未映射访问（其中 %d 条被 "
                        "HETTRACE_FILTER 拦下、只留在 meta）—— "
                        "地址映射与实际负载不符"
                        % (meta_unmapped, hidden),
                    )
                )

            header_meta = {
                "src_id": hdr.src_id,
                "name": hdr.name,
                "level": hdr.level,
                "axi_data_bytes": hdr.axi_data_bytes,
                "axi_addr_bits": hdr.axi_addr_bits,
                "ticks_per_second": hdr.ticks_per_second,
                "clock_period_ticks": hdr.clock_period_ticks,
                "filter": "dram" if hdr.filtered_dram else "all",
            }
            for field, observed in header_meta.items():
                if meta.get(field) != observed:
                    issues.append(
                        Issue(
                            "ERROR",
                            src,
                            "meta.%s=%r，文件头为 %r"
                            % (field, meta.get(field), observed),
                        )
                    )
            if meta.get("synth") != summary.synth:
                issues.append(
                    Issue(
                        "ERROR",
                        src,
                        "meta.synth=%r，记录的 SYNTH flag 归纳为 %r"
                        % (meta.get("synth"), summary.synth),
                    )
                )

        # 时钟域：头部声明必须与 addrmap.json 一致，否则 tick 折算是错的。
        canonical = addrmap.SRC_NAME_BY_ID.get(hdr.src_id)
        if canonical is None:
            issues.append(
                Issue("WARN", src, "src_id=%d 不在 addrmap.json 中" % hdr.src_id)
            )
        else:
            if hdr.name != canonical:
                issues.append(
                    Issue(
                        "ERROR",
                        src,
                        "文件头 name=%r 与 src_id=%d 的规范名 %r 不一致"
                        % (hdr.name, hdr.src_id, canonical),
                    )
                )
            _sid, exp_level, _mhz, exp_period = addrmap.SOURCES[canonical]
            if (hdr.clock_period_ticks * addrmap.TICKS_PER_SECOND !=
                    exp_period * hdr.ticks_per_second):
                issues.append(
                    Issue(
                        "ERROR",
                        src,
                        "clock_period_ticks=%d，addrmap.json 声明为 %d —— "
                        "时钟域配置不一致"
                        % (hdr.clock_period_ticks,
                           exp_period * hdr.ticks_per_second // addrmap.TICKS_PER_SECOND),
                    )
                )
            if hdr.level != addrmap.LEVELS[exp_level]:
                # interconnect 是例外：HetAxiMonitor 挂在互连边界上，一个点同时
                # 看见三个源，所以它写出的每个源文件都标 interconnect，而
                # addrmap.json 里每个源声明的是它自己那个设备 tap 的位置。两者
                # 都合法，只是不能混在一批里比对 —— 那才是这条检查真正要防的。
                if hdr.level == addrmap.LEVELS["interconnect"]:
                    issues.append(
                        Issue(
                            "INFO",
                            src,
                            "在互连边界观测（addrmap.json 里该源的设备 tap 为 %s）"
                            % exp_level,
                        )
                    )
                else:
                    issues.append(
                        Issue(
                            "ERROR",
                            src,
                            "tap 层级为 %s，addrmap.json 声明为 %s —— "
                            "混用不同层级的 trace 无法互相比对"
                            % (
                                addrmap.LEVEL_NAMES.get(hdr.level, hdr.level),
                                exp_level,
                            ),
                        )
                    )

        # AXI 结构。只在这份 trace 带地址通道时才有意义 —— 设备 tap 写的是
        # 数据通道投影，没有 AW/AR 可配对，那不是缺陷。
        for category, count in sorted(axi.protocol_errors.items()):
            issues.append(
                Issue(
                    "ERROR",
                    src,
                    "%d 处 AXI 协议/格式违约：%s（%s）"
                    % (
                        count,
                        _AXI_ERROR_TEXT.get(category, category),
                        category,
                    ),
                )
            )

        if summary.transactions:
            if axi.unmatched_data:
                issues.append(
                    Issue(
                        "ERROR",
                        src,
                        "%d 个数据拍找不到对应的地址通道记录 —— AW/AR 与 W/R 的 "
                        "txn 没对上，事务无法重建" % axi.unmatched_data,
                    )
                )
            if axi.duplicate_addr:
                issues.append(
                    Issue(
                        "ERROR",
                        src,
                        "%d 个 txn 在完成前重复出现 AW/AR —— 事务号被复用"
                        % axi.duplicate_addr,
                    )
                )
            if axi.leftover_open:
                issues.append(
                    Issue(
                        "WARN",
                        src,
                        "%d 笔事务只有请求没有完成 —— 仿真在事务在途时结束，"
                        "尾部这些事务的延迟不可用" % axi.leftover_open,
                    )
                )
        if axi.unmatched_resp:
            issues.append(
                Issue(
                    "ERROR",
                    src,
                    "%d 条 B 响应没有对应的 AW —— 响应通道与请求通道错位"
                    % axi.unmatched_resp,
                )
            )
        if summary.resp_errors:
            issues.append(
                Issue(
                    "WARN",
                    src,
                    "%d 条记录带 SLVERR/DECERR —— 有访问被 slave 拒绝或译码失败"
                    % summary.resp_errors,
                )
            )

        if summary.unmapped:
            issues.append(
                Issue(
                    "ERROR",
                    src,
                    "%d 条记录落在所有已声明区域之外 —— 地址映射与实际负载不符"
                    % summary.unmapped,
                )
            )

        for reg, n in sorted(bad_accessor.items()):
            issues.append(
                Issue(
                    "WARN",
                    src,
                    "%d 次访问 region %s，但 addrmap.json 未把 %s 列为其 accessor"
                    % (n, reg, canonical),
                )
            )

    issues.extend(_cross_source_checks(summaries, require_heterogeneous))
    return issues, summaries


def _cross_source_checks(summaries, require_heterogeneous=True):
    """跨源检查 —— 决定这批 trace 到底有没有"异构"信息量。"""
    issues = []
    # src_id 重复已在 validate_dir 报 ERROR；这里再去重，避免两份
    # 同源文件被误算成“两个异构源”并通过交接检查。
    by_src_id = {}
    for summary in summaries:
        if summary.count > 0 and summary.src_id not in by_src_id:
            by_src_id[summary.src_id] = summary
    live = list(by_src_id.values())

    # interconnect 是逐源 level 契约的合法例外，但一批用于互相比较的 trace
    # 仍必须来自同一个观察层。否则 host@interconnect 与
    # vortex@post_llc 看见的流量集合不同，时间/带宽对比没有共同口径。
    levels = {summary.level for summary in live}
    if len(levels) > 1:
        detail = ", ".join(
            "%s=%s" % (
                summary.name,
                addrmap.LEVEL_NAMES.get(summary.level, summary.level),
            )
            for summary in live
        )
        issues.append(
            Issue(
                "ERROR",
                "-",
                "同一批 trace 混用了不同观察层级（%s）—— 无法互相比对"
                % detail,
            )
        )

    if len(live) < 2:
        level = "ERROR" if require_heterogeneous else "INFO"
        suffix = ("这不是异构 trace" if require_heterogeneous else
                  "已按显式单源诊断模式跳过跨源交接检查")
        issues.append(Issue(
            level, "-", "只有 %d 个源产生了记录 —— %s" % (len(live), suffix)
        ))
        return issues

    # 1. 至少有一个交接区被两个源真实触及，否则这几条 trace 互不相关，
    #    归并出来也看不到任何交接行为。
    #
    #    这里遍历 HANDOFF_REGIONS（accessor >= 2）。源两两配对时交接区并不是
    #    同一个：host+CoralNPU 在 shared_buffer / npu_work 交接，host+Vortex
    #    在 vortex_bar 交接。受 NPU 32 位地址与 Vortex BAR 位置约束，当前没有
    #    三方以同一物理地址直连共享的区域。
    found = []
    for reg in addrmap.HANDOFF_REGIONS:
        touchers = [s.name for s in live if s.regions.get(reg, 0) > 0]
        if len(touchers) >= 2:
            found.append(reg)
            issues.append(
                Issue("INFO", "-", "交接区 %s 被 %s 共同访问" % (reg, ", ".join(touchers)))
            )
    if not found:
        # 候选区里有一部分在默认过滤器（HETTRACE_FILTER=dram，判据是
        # trace_windows）下压根不可能出现 —— npu_mailbox 就是：它是控制面寄存器，
        # 落在窗口之外，writer 从不记它。把它当"你该去看看那儿"的线索列出来，只会
        # 让人白查一圈，所以显式标注出来。
        cands = []
        for reg in addrmap.HANDOFF_REGIONS:
            base = addrmap.REGIONS[reg][0]
            cands.append(reg if addrmap.is_traced(base) else reg + "(不在过滤窗口内)")
        issues.append(
            Issue(
                "ERROR",
                "-",
                "没有任何交接区被两个以上的源触及（候选：%s）—— 负载没有真正的"
                "跨设备数据交接，trace 退化为几条不相干的流" % ", ".join(cands),
            )
        )

    # 2. 时间区间必须重叠。不重叠说明三者是串行跑的，没有任何争抢可分析。
    for i in range(len(live)):
        for j in range(i + 1, len(live)):
            a, b = live[i], live[j]
            lo = max(a.first_tick, b.first_tick)
            hi = min(a.last_tick, b.last_tick)
            if lo >= hi:
                issues.append(
                    Issue(
                        "WARN",
                        "-",
                        "%s [%d,%d] 与 %s [%d,%d] 时间区间不重叠 —— "
                        "两者串行执行，这段 trace 无法用于争抢分析"
                        % (
                            a.name,
                            a.first_tick,
                            a.last_tick,
                            b.name,
                            b.first_tick,
                            b.last_tick,
                        ),
                    )
                )

    return issues


def format_report(issues, summaries):
    L = []
    a = L.append

    a("=" * 72)
    a("hettrace 校验报告")
    a("=" * 72)
    a("")
    a("每源统计:")
    a(
        "  %-10s %-12s %10s %10s %10s %10s %12s"
        % ("源", "层级", "记录数", "数据拍", "读", "写", "字节")
    )
    for s in summaries:
        a(
            "  %-10s %-12s %10d %10d %10d %10d %12d"
            % (
                s.name,
                addrmap.LEVEL_NAMES.get(s.level, "?"),
                s.count,
                s.data_records,
                s.reads,
                s.writes,
                s.bytes,
            )
        )
    a("")
    # 带地址通道的 trace 才有"事务"可言。设备 tap 写的是数据通道投影，这一节
    # 会整节略过 —— 而不是显示一排 0，那会让人以为事务丢了。
    axi_srcs = [s for s in summaries if s.transactions]
    if axi_srcs:
        a("AXI 通道构成 (记录数 = 各通道之和):")
        a(
            "  %-10s %10s %10s %10s %10s %10s %10s"
            % ("源", "AW", "W", "B", "AR", "R", "事务数")
        )
        for s in axi_srcs:
            a(
                "  %-10s %10d %10d %10d %10d %10d %10d"
                % (
                    s.name,
                    s.chan_counts.get(CHAN_AW, 0),
                    s.chan_counts.get(CHAN_W, 0),
                    s.chan_counts.get(CHAN_B, 0),
                    s.chan_counts.get(CHAN_AR, 0),
                    s.chan_counts.get(CHAN_R, 0),
                    s.transactions,
                )
            )
        a("")
    synth = [s.name for s in summaries if s.synth]
    if synth:
        a(
            "  注: %s 的 AXI 字段是按契约推导的，不是从真 AXI 信号读到的；"
            "不可当协议证据用。" % ", ".join(synth)
        )
        a("")
    a("时间区间 (gem5 tick):")
    for s in summaries:
        span = s.last_tick - s.first_tick
        a(
            "  %-10s [%14d, %14d]  跨度 %d tick (%.3f us)"
            % (
                s.name,
                s.first_tick,
                s.last_tick,
                span,
                span / (s.ticks_per_second / 1e6) if s.ticks_per_second > 0 else 0,
            )
        )
    a("")
    a("区域分布:")
    for s in summaries:
        a("  %s:" % s.name)
        # regions 只统计 W/R 数据拍；分母也必须是 data_records，否则
        # 五通道 trace 的 AW/AR/B 会让所有百分比之和远小于 100%。
        total = max(s.data_records, 1)
        for reg, n in sorted(s.regions.items(), key=lambda kv: -kv[1]):
            a("    %-16s %10d  (%5.1f%%)" % (reg, n, 100.0 * n / total))
    a("")

    errors = [i for i in issues if i.level == "ERROR"]
    warns = [i for i in issues if i.level == "WARN"]
    infos = [i for i in issues if i.level == "INFO"]

    for label, group in (("ERROR", errors), ("WARN", warns), ("INFO", infos)):
        if not group:
            continue
        a("%s (%d):" % (label, len(group)))
        for i in group:
            a("  [%s] %s" % (i.source, i.message))
        a("")

    if errors:
        a("结论: 不可用。先修掉 %d 个 ERROR。" % len(errors))
    elif warns:
        a("结论: 可用，但有 %d 项限制必须写进报告。" % len(warns))
    else:
        a("结论: 通过。")
    a("=" * 72)
    return "\n".join(L)
