"""把 HETTrace v2 投影为下游工具可读的文本流。

HETTrace 是 AXI4 五通道真值源；外部 ``mem_sim``/``hbm_sim`` 的 trace 前端只
接受内存请求。因此转换必须是显式降级：完整 trace 的读从 AR 按
AxLEN 展开，写从 W 逐拍投影（才能保留 WSTRB）；设备级 trace（只有
W/R）则逐条保留。AXI ID、RESP、USER 等未进入 mem_sim 请求的字段仍可
通过映射 sidecar 回到原始 HETTrace 记录。

mem_sim 在普通 ``R/W address`` 行上会默认使用其 line_size，那会把 4/8/16B
的 AXI beat 静默放大成 64B。这里给 W 写入等长的全零 ``data=`` 与逐字节
``mask=``，给 R 写入等长的全零 ``expect=``，借用上游已有语法精确携带
请求大小。这些全零只是粒度载体，不是原始 AXI WDATA/RDATA 的功能回放。
在 mem_sim 内部它们仍会作为真实零值 payload/expected 执行，因此 response 的
``status``/``data_mismatches`` 只描述这个零值替身，不能用于判断原 trace 的数据正确性。

所有接口都是流式的，真实运行产生几十 GB trace 时不会把记录读进内存。
"""

from __future__ import annotations

import csv

from . import addrmap
from .reader import (
    BURST_NAMES,
    CHAN_AR,
    CHAN_AW,
    CHAN_R,
    CHAN_W,
    CHAN_NAMES,
    FLAG_LAST,
    FLAG_UNMAPPED,
    OP_WRITE,
    RESP_NAMES,
    is_data_chan,
)

BURST_FIXED = 0
BURST_INCR = 1
BURST_WRAP = 2


class ConvertError(ValueError):
    """输入 trace 无法无歧义地投影到目标格式。"""


PRESETS = {
    "readwrite": {
        "template": "{addr:#x} {rw}",
        "example": "0x90000040 R",
        "note": "十六进制地址 + R/W；默认只取 W/R 数据通道",
    },
    "dec_readwrite": {
        "template": "{addr} {rw}",
        "example": "2415919168 R",
        "note": "十进制地址 + R/W；默认只取 W/R 数据通道",
    },
    "timed": {
        "template": "{tick} {src} {rw} {addr:#x} {size}",
        "example": "1000 1 R 0x90000040 64",
        "note": "保留 HETTrace tick 与源 id；默认只取 W/R 数据通道",
    },
    "memsim": {
        "template": "<special: exact-size data/expect + write mask>",
        "example": "1200 W 0x90000040 data=0000 mask=ffff",
        "note": "GuXing25/mem_sim 精确字节粒度；需 --ticks-per-cycle",
    },
}


def list_presets():
    lines = []
    for name, item in PRESETS.items():
        lines.append(
            "%-14s %-28s %s" % (name, item["example"], item["note"])
        )
    return "\n".join(lines)


def _format_fields(record, cycle):
    return {
        "tick": record.tick,
        "cycle": cycle,
        "addr": record.addr,
        "size": record.size,
        "ctx": record.ctx,
        "seq": record.seq,
        "src": record.src_id,
        "src_name": addrmap.SRC_NAME_BY_ID.get(
            record.src_id, "src%d" % record.src_id
        ),
        "rw": "W" if record.op == OP_WRITE else "R",
        "op": record.op,
        "chan": CHAN_NAMES.get(record.chan, "??"),
        "chan_id": record.chan,
        "strb": record.strb,
        "txn": record.txn,
        "axi_id": record.axi_id,
        "axi_len": record.axi_len,
        "axi_size": record.axi_size,
        "burst": BURST_NAMES.get(record.burst, "UNKNOWN"),
        "burst_id": record.burst,
        "resp": RESP_NAMES.get(record.resp, "UNKNOWN"),
        "resp_id": record.resp,
        "user": record.user,
        "flags": record.flags,
    }


def convert(
    records,
    out_fh,
    template,
    sources=None,
    *,
    include_control=False,
    ticks_per_cycle=1,
    excluded_flags=0,
):
    """按 ``template`` 逐条格式化写出。

    默认只输出 W/R 数据通道，保持 v1 的“一条记录就是一次数据访问”语义。
    ``include_control=True`` 才会把 AW/B/AR 一并交给自定义模板。

    可用字段：tick、cycle、addr、size、ctx、seq、src、src_name、rw、op、
    chan、chan_id、strb、txn、axi_id、axi_len、axi_size、burst、burst_id、
    resp、resp_id、user、flags。
    """
    if ticks_per_cycle <= 0:
        raise ConvertError("ticks_per_cycle 必须为正整数")

    count = 0
    last_cycle = 0
    for record in records:
        if sources is not None and record.src_id not in sources:
            continue
        if excluded_flags and record.flags & excluded_flags:
            continue
        if not include_control and not is_data_chan(record.chan):
            continue
        cycle = record.tick // ticks_per_cycle
        # 正常 merge 流本来就是单调的；夹紧是为了让自定义调用者即使传入同 tick
        # 多源流或轻微回退，也不会生成 mem_sim 明确拒绝的降序 inject_cycle。
        cycle = max(last_cycle, cycle)
        out_fh.write(template.format(**_format_fields(record, cycle)))
        out_fh.write("\n")
        last_cycle = cycle
        count += 1
    return count


def _burst_address(record, beat, beat_bytes, beats):
    if record.addr % beat_bytes:
        raise ConvertError(
            "src=%d seq=%d 的地址 0x%x 未按 AxSIZE=%d 对齐；"
            "HETTrace 当前无 RSTRB，不能无损投影 unaligned burst"
            % (record.src_id, record.seq, record.addr, record.axi_size)
        )
    if record.burst == BURST_FIXED:
        if beats > 16:
            raise ConvertError(
                "src=%d seq=%d 的 FIXED burst 有 %d 拍，AXI4 上限为 16"
                % (record.src_id, record.seq, beats)
            )
        return record.addr
    if record.burst == BURST_INCR:
        return record.addr + beat * beat_bytes
    if record.burst == BURST_WRAP:
        if beats not in (2, 4, 8, 16):
            raise ConvertError(
                "src=%d seq=%d 的 WRAP burst 有 %d 拍，必须为 2/4/8/16"
                % (record.src_id, record.seq, beats)
            )
        span = beats * beat_bytes
        boundary = (record.addr // span) * span
        return boundary + ((record.addr - boundary + beat * beat_bytes) % span)
    raise ConvertError(
        "src=%d seq=%d 的 burst=%d 不是合法 AXI 类型"
        % (record.src_id, record.seq, record.burst)
    )


def _memsim_requests(records, sources, excluded_flags, allow_unmapped):
    """产出 ``(record, address, beat, projection, size)``。

    每个源第一次看到 AW/AR 后便确定为完整五通道流。完整流从地址通道投影，
    这样读请求使用 AR 的发射 tick，而不会错误地使用 R 返回时刻；设备级流没有
    地址通道，只能逐条使用 W/R。一个源内混用两种层级本身就是无效 trace，
    ``validate`` 会先把它拦住。
    """
    full_sources = set()
    write_beats = {}
    data_beats = {}

    def take_beat(counters, record):
        key = (record.src_id, record.txn)
        beat = counters.get(key, 0)
        # AxLEN 是最终真值；LAST 若损坏不应让转换器状态随文件无限增长。
        # validate 会单独把两者不一致报为协议错误。
        if beat >= record.axi_len or record.flags & FLAG_LAST:
            counters.pop(key, None)
        else:
            counters[key] = beat + 1
        return beat

    for record in records:
        if sources is not None and record.src_id not in sources:
            continue
        if excluded_flags and record.flags & excluded_flags:
            continue
        if record.flags & FLAG_UNMAPPED and not allow_unmapped:
            raise ConvertError(
                "src=%d seq=%d 地址 0x%x 未映射；先修正 trace，或显式使用 "
                "--allow-unmapped" % (record.src_id, record.seq, record.addr)
            )

        if record.chan in (CHAN_AW, CHAN_AR):
            full_sources.add(record.src_id)
            # 写必须等 W 记录：AW 不带 WSTRB，从 AW 展开会把部分
            # 写错误地变成全字节写。读则必须用 AR 的发射 tick，不能用
            # 晚到的 R 返回 tick。
            beat_bytes = 1 << record.axi_size
            beats = record.axi_len + 1
            # 写虽然不从 AW 发射请求，也要在这里拒绝无法无损转换的
            # unaligned/非法 burst，不能等到后面的 W 后静默放行。
            _burst_address(record, 0, beat_bytes, beats)
            if record.chan == CHAN_AW:
                continue
            for beat in range(beats):
                yield (
                    record,
                    _burst_address(record, beat, beat_bytes, beats),
                    beat,
                    "axi_read_address",
                    beat_bytes,
                )
            continue

        if not is_data_chan(record.chan):
            continue
        if record.src_id in full_sources:
            if record.chan == CHAN_W:
                yield (
                    record,
                    record.addr,
                    take_beat(write_beats, record),
                    "axi_write_data",
                    record.size,
                )
            # 完整 trace 的 R 已由 AR 投影，否则会把一笔读算两次。
            continue
        yield (
            record,
            record.addr,
            take_beat(data_beats, record),
            "data",
            record.size,
        )


def _write_mask(record, address, size, projection, axi_data_bytes):
    """把 HETTrace uint64 WSTRB 投影为 mem_sim 的相对字节 mask。"""
    if size <= 0:
        raise ConvertError(
            "src=%d seq=%d 的写数据大小为 %d" %
            (record.src_id, record.seq, size)
        )

    if projection == "axi_write_data":
        width = axi_data_bytes
        if width is None:
            # 直接调 Python API 时可能没有 header。CLI 总会传入真实宽度；
            # 这个保守推导只是为了保持 API 向后兼容。
            needed = max(size, record.strb.bit_length(), 1)
            width = 1 << (needed - 1).bit_length()
        if width <= 0 or width > 64:
            raise ConvertError(
                "src=%d 的 axi_data_bytes=%r 超出 HETTrace WSTRB 宽度"
                % (record.src_id, width)
            )
        if record.strb >> width:
            raise ConvertError(
                "src=%d seq=%d 的 WSTRB=0x%x 超出 %dB 数据总线"
                % (record.src_id, record.seq, record.strb, width)
            )
        enabled = [
            bool(record.strb & (1 << ((address + i) % width)))
            for i in range(size)
        ]
    else:
        # 设备级 Emit() 是抽象访问投影，strb bit 0 对应记录的
        # addr，而不是物理 AXI lane 0。
        enabled = [bool(record.strb & (1 << i)) for i in range(size)]
    return "".join("ff" if byte_enabled else "00" for byte_enabled in enabled)


def convert_memsim(
    records,
    out_fh,
    ticks_per_cycle,
    sources=None,
    *,
    map_fh=None,
    excluded_flags=0,
    allow_unmapped=False,
    axi_data_bytes_by_src=None,
):
    """写出 GuXing25/mem_sim 的精确字节粒度请求输入。

    ``map_fh`` 若给出则写 CSV sidecar。它的 ``host_request_id`` 与 mem_sim
    TrafficStream 的零起始 ``next_id_`` 一致，可直接关联 ``--response-trace``。
    """
    if ticks_per_cycle is None or ticks_per_cycle <= 0:
        raise ConvertError("memsim 预设需要正整数 --ticks-per-cycle")

    mapping = csv.writer(map_fh) if map_fh is not None else None
    if mapping is not None:
        mapping.writerow(
            (
                "host_request_id",
                "trace_line",
                "tick",
                "cycle",
                "src_name",
                "src_id",
                "seq",
                "txn",
                "chan",
                "axi_id",
                "beat",
                "addr",
                "size",
                "projection",
                "flags",
            )
        )

    count = 0
    last_cycle = 0
    for record, address, beat, projection, size in _memsim_requests(
        records, sources, excluded_flags, allow_unmapped
    ):
        if size <= 0:
            raise ConvertError(
                "src=%d seq=%d 的数据请求大小为 %d"
                % (record.src_id, record.seq, size)
            )
        cycle = max(last_cycle, record.tick // ticks_per_cycle)
        rw = "W" if record.op == OP_WRITE else "R"
        zero_payload = "00" * size
        if rw == "W":
            width = None if axi_data_bytes_by_src is None else \
                axi_data_bytes_by_src.get(record.src_id)
            mask = _write_mask(record, address, size, projection, width)
            out_fh.write(
                "%d W 0x%x data=%s mask=%s\n"
                % (cycle, address, zero_payload, mask)
            )
        else:
            out_fh.write(
                "%d R 0x%x expect=%s\n" % (cycle, address, zero_payload)
            )
        if mapping is not None:
            mapping.writerow(
                (
                    count,
                    count + 1,
                    record.tick,
                    cycle,
                    addrmap.SRC_NAME_BY_ID.get(
                        record.src_id, "src%d" % record.src_id
                    ),
                    record.src_id,
                    record.seq,
                    record.txn,
                    CHAN_NAMES.get(record.chan, "??"),
                    record.axi_id,
                    beat,
                    "0x%x" % address,
                    size,
                    projection,
                    "0x%02x" % record.flags,
                )
            )
        last_cycle = cycle
        count += 1
    return count
