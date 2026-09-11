#!/usr/bin/env python3
"""hettrace Python 工具链自测。

    python3 tools/tests/test_tools.py

重点覆盖两类：
  - 归并/读取的正确性（含与 C++ writer 产物的交叉验证，若已编译）
  - validate 对每种失效形态都真的报错 —— 一个从不报错的校验器没有价值
"""

from __future__ import annotations

import io
import csv
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))

from hettrace import (  # noqa: E402
    addrmap,
    convert,
    merge,
    reader,
    stats,
    synth,
    validate,
)
from hettrace.reader import (  # noqa: E402
    CHAN_AR,
    CHAN_AW,
    CHAN_B,
    CHAN_R,
    CHAN_W,
    FLAG_DMA,
    FLAG_BURST_BEAT,
    FLAG_LAST,
    FLAG_SYNTH,
    FORMAT_VERSION,
    OP_READ,
    OP_WRITE,
    RECORD_SIZE,
    TraceError,
    discover,
    read_header,
    read_data_records,
    read_records,
)

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

_failed = 0
_checks = 0


def check(cond, msg):
    global _failed, _checks
    _checks += 1
    if not cond:
        sys.stderr.write("FAIL: %s\n" % msg)
        _failed += 1


def tmpdir():
    return tempfile.mkdtemp(prefix="hettrace_py_")


def rewrite_header(path, **changes):
    """测试专用：同步改二进制 header 与 meta，构造一致但语义错误的输入。"""
    fields = (
        "magic", "version", "record_size", "ticks_per_second",
        "clock_period_ticks", "src_id", "level", "flags",
        "axi_data_bytes", "axi_addr_bits", "name",
    )
    with open(path, "r+b") as handle:
        raw = handle.read(reader.HEADER_SIZE)
        values = list(reader._HEADER_STRUCT.unpack(raw))
        for key, value in changes.items():
            index = fields.index(key)
            if key == "name":
                value = value.encode()[:23].ljust(24, b"\x00")
            values[index] = value
        handle.seek(0)
        handle.write(reader._HEADER_STRUCT.pack(*values))

    meta_path = path + ".meta.json"
    if os.path.exists(meta_path):
        with open(meta_path, encoding="utf-8") as handle:
            meta = json.load(handle)
        for key, value in changes.items():
            if key in meta:
                meta[key] = value
        with open(meta_path, "w", encoding="utf-8") as handle:
            json.dump(meta, handle, indent=2)
            handle.write("\n")


# ---------------------------------------------------------------------------
# 地址映射
# ---------------------------------------------------------------------------

def test_addrmap_generated_in_sync():
    r = subprocess.run(
        [sys.executable, os.path.join(ROOT, "scripts", "gen_addrmap.py"), "--check"],
        capture_output=True, text=True,
    )
    check(r.returncode == 0,
          "生成的 addrmap 应与 addrmap.json 同步:\n%s%s" % (r.stdout, r.stderr))


def test_addrmap_invariants():
    for name, (base, size, kind, acc) in addrmap.REGIONS.items():
        # CoralNPU 的 AXI 地址是 uint32_t：它够不到的区域不能列它作 accessor。
        # 这个检查按 accessor 而不是按 kind 来判 —— vortex_bar 就在 4GiB 之上，
        # 它是合法的，只是 NPU 碰不到。
        if "coralnpu" in acc:
            check(base + size <= (1 << addrmap.NPU_ADDR_BITS),
                  "%s 有 coralnpu accessor 但超出其 %d 位可寻址范围"
                  % (name, addrmap.NPU_ADDR_BITS))
        check(base + size <= (1 << addrmap.MAP_ADDR_BITS),
              "区域 %s 超出地址图的 %d 位范围" % (name, addrmap.MAP_ADDR_BITS))
        if kind == "dram":
            check(addrmap.is_dram(base),
                  "DRAM 区域 %s 应落在 CoralNPU 的 DDR 判定窗口内" % name)
        # 凡是要参与共享字节比对的区域都必须过得了 dram 过滤器，否则记录会被
        # 默默丢掉 —— 那是最难查的一类失败：trace 存在、就是少了几条。
        if kind in ("dram", "bar"):
            check(addrmap.is_traced(base) and addrmap.is_traced(base + size - 1),
                  "区域 %s 应整个落在 trace 窗口内" % name)

    check(addrmap.is_traced(addrmap.REGIONS["vortex_bar"][0]),
          "vortex_bar 必须过得了 dram 过滤器 —— 经 BAR 的访问是真实内存流量")
    check(not addrmap.is_dram(addrmap.REGIONS["vortex_bar"][0]),
          "vortex_bar 不在 CoralNPU 的 DDR 判定窗口内，is_dram 应为假")
    check(not addrmap.is_traced(addrmap.REGIONS["vortex_cp"][0]),
          "CP 寄存器不是内存流量，不该进 trace")

    shared_base = addrmap.REGIONS["shared_buffer"][0]
    check(not any(len(region[3]) >= 3 for region in addrmap.REGIONS.values()),
          "当前地址约束下不应声称存在三方直连共享区")
    check(addrmap.REGIONS["shared_buffer"][3] == ("host", "coralnpu"),
          "shared_buffer 应只属于 host↔CoralNPU 交接")
    check(not addrmap.may_access("vortex", shared_base),
          "Vortex 不应以同一物理地址访问 shared_buffer")
    check(addrmap.region_of(shared_base) == "shared_buffer", "region_of 应正确")
    check(addrmap.region_of(0xDEADBEEF) is None, "未映射地址应返回 None")
    check(not addrmap.may_access("coralnpu", addrmap.REGIONS["host_heap"][0]),
          "NPU 不应被允许访问 host 私有堆")
    check(addrmap.may_access("coralnpu", shared_base),
          "NPU 应被允许访问共享区")


def test_docs_are_current():
    """持久化文档不得链接缺失文件或复活已删除架构。"""
    markdown = []
    for directory, dirnames, filenames in os.walk(ROOT):
        dirnames[:] = [
            name for name in dirnames
            if name not in (".git", "build", "__pycache__")
        ]
        markdown.extend(
            os.path.join(directory, name)
            for name in filenames if name.endswith(".md")
        )

    broken = []
    obsolete = []
    forbidden = (
        "HetTraceProbe",
        "05-beginner-report.md",
        "architecture-overview.png",
        "current-architecture-dataflow.png",
        "heterogeneous-trace-beginner-guide.pdf",
    )
    link_pattern = re.compile(r"!?\[[^\]]*\]\(([^)]+)\)")
    for path in markdown:
        with open(path, encoding="utf-8") as handle:
            text = handle.read()
        for marker in forbidden:
            if marker in text:
                obsolete.append("%s:%s" % (os.path.relpath(path, ROOT), marker))
        for raw_target in link_pattern.findall(text):
            target = raw_target.strip().split()[0].strip("<>")
            if target.startswith(("http://", "https://", "mailto:", "#")):
                continue
            relative = target.split("#", 1)[0]
            resolved = os.path.join(os.path.dirname(path), relative)
            if relative and not os.path.exists(resolved):
                broken.append("%s:%s" % (os.path.relpath(path, ROOT), target))

    check(not broken, "Markdown 相对链接必须存在，失效: %r" % broken)
    check(not obsolete, "文档不得引用已删除内容: %r" % obsolete)
    with open(os.path.join(ROOT, "README.md"), encoding="utf-8") as handle:
        readme = handle.read()
    with open(os.path.join(ROOT, "docs", "USER_MANUAL.md"), encoding="utf-8") as handle:
        manual = handle.read()
    check("05-validation-report.md" in readme and "05-validation-report.md" in manual,
          "README 与项目手册都必须指向当前验证报告")


# ---------------------------------------------------------------------------
# 读取与归并
# ---------------------------------------------------------------------------

def test_flag_bits_match_cpp():
    """reader.py 的 FLAG_* 必须与 record.h 的 kFlag* 逐位一致。

    格式常量在两侧各写了一遍（Python 侧不 include C++ 头），而 flags 是唯一没有
    被 gen_addrmap.py 生成、也不会在 writer 交叉验证里被检查的一组常量 —— 它们
    只在"按 flag 分类记录"时才起作用，错一位的后果是分类静默错位，没有任何报错。
    所以在这里直接从头文件里把值抠出来比。
    """
    h = os.path.join(ROOT, "libhettrace", "include", "hettrace", "record.h")
    with open(h) as f:
        text = f.read()
    cpp = dict(re.findall(r"constexpr uint8_t kFlag(\w+)\s*=\s*1u\s*<<\s*(\d+)", text))
    check(len(cpp) >= 5, "record.h 里应能抠出 5 个以上 kFlag，实为 %r" % (cpp,))

    # kFlagBurstBeat -> FLAG_BURST_BEAT
    def to_py(name):
        return "FLAG_" + re.sub(r"(?<!^)(?=[A-Z])", "_", name).upper()

    for name, bit in cpp.items():
        py_name = to_py(name)
        py_val = getattr(reader, py_name, None)
        check(py_val is not None,
              "reader.py 缺少 %s（record.h 里有 kFlag%s）" % (py_name, name))
        if py_val is not None:
            check(py_val == 1 << int(bit),
                  "%s 应为 1<<%s，实为 %r" % (py_name, bit, py_val))

    # 反向：Python 侧不该有 C++ 侧没有的位，否则是删了 C++ 定义没同步
    for py_name in [k for k in dir(reader) if k.startswith("FLAG_")]:
        want = {to_py(n) for n in cpp}
        check(py_name in want,
              "reader.py 的 %s 在 record.h 里没有对应的 kFlag" % py_name)


def test_synth_roundtrip():
    d = tmpdir()
    try:
        synth.gen_cooperative(d, n_per_src=20)
        entries = discover(d)
        check(len(entries) == 3, "应发现 3 个源，实为 %d" % len(entries))
        names = [h.name for _p, h in entries]
        check(names == ["host", "vortex", "coralnpu"],
              "应按 src_id 排序，实为 %r" % names)

        for path, hdr in entries:
            recs = list(read_records(path))
            check(len(recs) > 0, "%s 应有记录" % hdr.name)
            seqs = [r.seq for r in recs]
            check(seqs == list(range(len(recs))), "%s 的 seq 应连续" % hdr.name)
            check(all(r.src_id == hdr.src_id for r in recs),
                  "%s 每条记录的 src_id 应与头部一致" % hdr.name)
            _sid, _lvl, _mhz, period = addrmap.SOURCES[hdr.name]
            check(hdr.clock_period_ticks == period,
                  "%s 的时钟周期应与 addrmap 一致" % hdr.name)
    finally:
        shutil.rmtree(d)


def test_synth_axi_full_roundtrip():
    """五通道合成流必须逐字段可读，并通过结构校验。"""
    d = tmpdir()
    try:
        synth.gen_axi_full(d, n=6)
        issues, summaries = validate.validate_dir(d)
        errors = [issue for issue in issues if issue.level == "ERROR"]
        check(not errors, "健康五通道 trace 不应有 ERROR，实际: %r" % errors)
        check(len(summaries) == 3, "五通道场景应覆盖三个源")

        expected_channels = {CHAN_AW, CHAN_W, CHAN_B, CHAN_AR, CHAN_R}
        for path, header in discover(d):
            records = list(read_records(path))
            check(header.version == FORMAT_VERSION,
                  "%s 应写 v%d header" % (header.name, FORMAT_VERSION))
            check(header.record_size == RECORD_SIZE,
                  "%s record_size 应为 %d" % (header.name, RECORD_SIZE))
            check(header.level == addrmap.LEVELS["interconnect"],
                  "%s 应标为 interconnect tap" % header.name)
            channels = {record.chan for record in records}
            if header.name == "host":
                check({CHAN_AW, CHAN_W, CHAN_B}.issubset(channels),
                      "host 写事务应含 AW/W/B")
            else:
                check({CHAN_AR, CHAN_R}.issubset(channels),
                      "%s 读事务应含 AR/R" % header.name)
            check(channels.issubset(expected_channels), "不应出现未知 AXI 通道")
            check(len(list(read_data_records(path))) < len(records),
                  "%s 五通道投影应滤掉控制记录" % header.name)
            check(all(record.flags & FLAG_SYNTH for record in records),
                  "%s 的统一 gem5 monitor AXI 字段应标 synth"
                  % header.name)

        vortex_path = os.path.join(d, "vortex.hettrace")
        vortex_records = list(read_records(vortex_path))
        ar = next(record for record in vortex_records if record.chan == CHAN_AR)
        r = next(
            record for record in vortex_records
            if record.chan == CHAN_R and record.txn == ar.txn
        )
        check(r.tick > ar.tick, "R 必须在 AR 之后，trace 才保留读延迟")
    finally:
        shutil.rmtree(d)


def test_merge_is_totally_ordered():
    d = tmpdir()
    try:
        synth.gen_cooperative(d, n_per_src=40)
        records, entries = merge.merge_dir(d)
        merged = list(records)
        check(len(merged) > 0, "归并结果不应为空")

        keys = [(r.tick, r.src_id, r.seq) for r in merged]
        check(keys == sorted(keys), "归并结果应按 (tick, src_id, seq) 全序")

        # 记录总数必须守恒
        total = 0
        for path, _h in entries:
            total += sum(1 for _ in read_records(path))
        check(len(merged) == total,
              "归并不应丢记录: 合并后 %d, 各源合计 %d" % (len(merged), total))

        # 必须真的交织，否则归并没意义
        srcs_seen = []
        for r in merged:
            if not srcs_seen or srcs_seen[-1] != r.src_id:
                srcs_seen.append(r.src_id)
        check(len(srcs_seen) > 3, "归并流应在多源之间反复交织，实际切换 %d 次"
              % len(srcs_seen))
    finally:
        shutil.rmtree(d)


def test_merge_text_output():
    d = tmpdir()
    try:
        synth.gen_cooperative(d, n_per_src=10)
        records, _ = merge.merge_dir(d)
        buf = io.StringIO()
        n = merge.write_text(records, buf)
        text = buf.getvalue()
        check(n > 0, "应写出记录")
        check("shared_buffer" in text, "输出应带 region 列")
        check("host" in text and "vortex" in text and "coralnpu" in text,
              "输出应含三个源名")
        data_lines = [l for l in text.splitlines() if not l.startswith("#")]
        check(len(data_lines) == n, "数据行数应与返回值一致")
    finally:
        shutil.rmtree(d)


def test_truncated_file_raises():
    d = tmpdir()
    try:
        synth.gen_cooperative(d, n_per_src=10)
        p = os.path.join(d, "host.hettrace")
        with open(p, "ab") as f:
            f.write(b"\x00" * 7)   # 非记录整数倍的残余
        try:
            list(read_records(p))
            check(False, "尾部残余应抛 TraceError")
        except TraceError as e:
            check("截断" in str(e), "错误信息应提示截断，实为 %s" % e)
    finally:
        shutil.rmtree(d)


def test_discover_rejects_bad_candidate_header():
    """名字已经是 *.hettrace 的坏文件不能被静默忽略。"""
    d = tmpdir()
    try:
        with open(os.path.join(d, "damaged.hettrace"), "wb") as handle:
            handle.write(b"not-a-header")
        try:
            discover(d)
            check(False, "discover 应拒绝损坏的 trace 候选文件")
        except TraceError as exc:
            check("damaged.hettrace" in str(exc),
                  "discover 错误应点名损坏文件: %s" % exc)

        issues, _summaries = validate.validate_dir(d)
        check(any(i.level == "ERROR" and "damaged.hettrace" in i.message
                  for i in issues),
              "validate 应把坏 header 报为 ERROR")
    finally:
        shutil.rmtree(d)


# ---------------------------------------------------------------------------
# validate 必须抓住每种失效形态
# ---------------------------------------------------------------------------

def _run_validate(d):
    issues, summaries = validate.validate_dir(d)
    errs = [i for i in issues if i.level == "ERROR"]
    warns = [i for i in issues if i.level == "WARN"]
    return issues, summaries, errs, warns


def test_validate_accepts_healthy():
    d = tmpdir()
    try:
        synth.gen_cooperative(d, n_per_src=50)
        issues, summaries, errs, _warns = _run_validate(d)
        check(not errs, "健康 trace 不应有 ERROR，实际: %r"
              % [(e.source, e.message) for e in errs])
        check(len(summaries) == 3, "应汇总 3 个源")
        infos = [i for i in issues if i.level == "INFO"]
        check(any("shared_buffer" in i.message for i in infos),
              "应报告 host↔NPU shared_buffer 交接")
        check(any("vortex_bar" in i.message for i in infos),
              "应报告 host↔Vortex BAR 交接")
        # 报告应能生成且不抛异常
        rep = validate.format_report(issues, summaries)
        check("结论" in rep, "报告应含结论行")
        check("通过" in rep or "限制" in rep, "健康 trace 结论应为通过或仅有限制")
    finally:
        shutil.rmtree(d)


def test_validate_accepts_bar_pair():
    """host + Vortex 经 BAR 交接、完全不碰 shared_buffer —— 必须放行。

    这条盯的是一个具体的回归：交接区按源的配对而不同，validate 不能要求
    三方在同一物理地址上共享，否则会把这种合法跑法判成"无信息量"。
    """
    d = tmpdir()
    try:
        synth.gen_bar_pair(d, n=40)
        issues, summaries, errs, _warns = _run_validate(d)
        check(not errs, "host+Vortex 经 BAR 交接不应有 ERROR，实际: %r"
              % [(e.source, e.message) for e in errs])
        check(len(summaries) == 2, "应汇总 2 个源")
        infos = [i for i in issues if i.level == "INFO"]
        check(any("vortex_bar" in i.message for i in infos),
              "应报告 vortex_bar 被两源共同访问")
        _sizes, pairwise, _lb = stats.footprint(d)
        check(pairwise.get(("host", "vortex"), 0) > 0,
              "两源经 BAR 应量出共享 cache line，实际 %r" % pairwise)
    finally:
        shutil.rmtree(d)


def test_validate_explicit_single_source_mode():
    d = tmpdir()
    try:
        synth.gen_cooperative(d, n_per_src=10)
        for name in ("vortex", "coralnpu"):
            os.remove(os.path.join(d, name + ".hettrace"))
            os.remove(os.path.join(d, name + ".hettrace.meta.json"))
        _issues, summaries = validate.validate_dir(
            d, require_heterogeneous=False
        )
        errors = [issue for issue in _issues if issue.level == "ERROR"]
        check(not errors, "显式单源模式不应报跨源 ERROR，实际: %r" % errors)
        check(len(summaries) == 1, "显式单源模式应保留一个汇总")
        infos = [issue.message for issue in _issues if issue.level == "INFO"]
        check(any("单源诊断模式" in message for message in infos),
              "报告应明确标注单源诊断模式")
    finally:
        shutil.rmtree(d)


def test_validate_catches_no_sharing():
    d = tmpdir()
    try:
        synth.gen_broken_no_sharing(d)
        _i, _s, errs, _w = _run_validate(d)
        check(any("交接" in e.message for e in errs),
              "应报出无跨源交接，实际: %r" % [e.message for e in errs])
    finally:
        shutil.rmtree(d)


def test_validate_catches_non_monotonic():
    d = tmpdir()
    try:
        synth.gen_broken_non_monotonic(d)
        _i, _s, errs, _w = _run_validate(d)
        check(any("tick 回退" in e.message for e in errs),
              "应报出 tick 回退，实际: %r" % [e.message for e in errs])
    finally:
        shutil.rmtree(d)


def test_validate_catches_serial():
    d = tmpdir()
    try:
        synth.gen_broken_serial(d)
        _i, _s, _errs, warns = _run_validate(d)
        check(any("不重叠" in w.message for w in warns),
              "应警告时间区间不重叠，实际: %r" % [w.message for w in warns])
    finally:
        shutil.rmtree(d)


def test_validate_catches_truncation():
    d = tmpdir()
    try:
        synth.gen_broken_truncated(d)
        _i, _s, errs, _w = _run_validate(d)
        check(any("截断" in e.message for e in errs),
              "应报出截断，实际: %r" % [e.message for e in errs])
    finally:
        shutil.rmtree(d)


def test_validate_catches_seq_gap():
    d = tmpdir()
    try:
        synth.gen_broken_seq_gap(d)
        _i, _s, errs, _w = _run_validate(d)
        check(any("seq" in e.message for e in errs),
              "应报出 seq 断裂，实际: %r" % [e.message for e in errs])
    finally:
        shutil.rmtree(d)


def test_validate_catches_axi_orphan():
    d = tmpdir()
    try:
        synth.gen_broken_axi_orphan(d)
        _issues, _summaries, errors, _warnings = _run_validate(d)
        check(any("数据拍找不到" in error.message for error in errors),
              "应报出没有对应 AR 的孤儿 R，实际: %r"
              % [error.message for error in errors])
    finally:
        shutil.rmtree(d)


def test_validate_catches_axi_beat_contract():
    """逐拍抓 ID/地址/大小/LAST/WSTRB 以及少拍、多拍。"""
    d = tmpdir()
    try:
        shared = addrmap.REGIONS["shared_buffer"][0]
        writer = synth.SynthWriter(
            d, "host", level="interconnect", axi_data_bytes=16,
            synth_flag=True,
        )

        # txn 0 声明 2 拍，但只给 1 拍就回 B；该拍还故意改坏
        # ID、地址、size、LAST 和 WSTRB。
        writer._push(1000, shared, 0, 32, 7, 0, 1, OP_WRITE, CHAN_AW,
                     1, 4, 0, 0)
        writer._push(1100, shared + 16, 1 << 63, 8, 7, 0, 2, OP_WRITE,
                     CHAN_W, 1, 4, 0, FLAG_LAST)
        writer._push(1200, shared, 0, 0, 7, 0, 1, OP_WRITE, CHAN_B,
                     1, 4, 0, FLAG_LAST)

        # txn 1 只声明 1 拍，却给两拍。
        writer._push(2000, shared + 64, 0, 16, 8, 1, 3, OP_WRITE,
                     CHAN_AW, 0, 4, 0, 0)
        writer._push(2100, shared + 64, 0xffff, 16, 8, 1, 3, OP_WRITE,
                     CHAN_W, 0, 4, 0, FLAG_LAST)
        writer._push(2200, shared + 80, 0xffff, 16, 8, 1, 3, OP_WRITE,
                     CHAN_W, 0, 4, 0, FLAG_LAST)
        writer._push(2300, shared + 64, 0, 0, 8, 1, 3, OP_WRITE,
                     CHAN_B, 0, 4, 0, FLAG_LAST)
        writer.close()

        issues, _summaries = validate.validate_dir(
            d, require_heterogeneous=False
        )
        text_ = "\n".join(i.message for i in issues if i.level == "ERROR")
        for category in (
            "field_mismatch", "beat_size", "beat_address", "beat_flags",
            "write_strobe", "missing_data", "extra_data",
        ):
            check(category in text_, "validator 应报出 %s: %s" % (category, text_))
    finally:
        shutil.rmtree(d)


def test_validate_catches_data_only_beat_contract():
    """没有 AW/AR 的设备投影也必须校验宽度、strobe 与拍组形状。"""
    d = tmpdir()
    try:
        shared = addrmap.REGIONS["shared_buffer"][0]
        writer = synth.SynthWriter(
            d, "host", level="post_llc", axi_data_bytes=16
        )

        # 非零 RSTRB。
        writer._push(1000, shared, 1, 16, 0, 0, 0, OP_READ,
                     CHAN_R, 0, 4, 0, FLAG_LAST)
        # 128B beat 放在 16B 总线上。
        writer._push(1100, shared + 0x100, 0xffff, 128, 0, 1, 0,
                     OP_WRITE, CHAN_W, 0, 7, 0, FLAG_LAST)
        # 单拍 R 缺 LAST。
        writer._push(1200, shared + 0x200, 0, 16, 0, 2, 0, OP_READ,
                     CHAN_R, 0, 4, 0, 0)
        # 两拍投影的第二拍地址与 BURST_BEAT 都故意错误。
        writer._push(1300, shared + 0x300, 0xffff, 16, 0, 3, 0,
                     OP_WRITE, CHAN_W, 1, 4, 0, 0)
        writer._push(1400, shared + 0x320, 0xffff, 16, 0, 3, 0,
                     OP_WRITE, CHAN_W, 1, 4, 0, FLAG_LAST)
        writer.close()

        issues, _summaries = validate.validate_dir(
            d, require_heterogeneous=False
        )
        text_ = "\n".join(
            issue.message for issue in issues if issue.level == "ERROR"
        )
        for category in (
            "read_strobe", "beat_width", "beat_flags", "beat_address"
        ):
            check(category in text_,
                  "设备级 validator 应报出 %s: %s" % (category, text_))
    finally:
        shutil.rmtree(d)


def test_validate_rejects_mixed_capture_levels():
    """逐源 level 都合法，也不能把 interconnect 与设备 tap 混为一批。"""
    d = tmpdir()
    try:
        bar = addrmap.REGIONS["vortex_bar"][0]
        host = synth.SynthWriter(d, "host", level="interconnect")
        vortex = synth.SynthWriter(d, "vortex", level="post_llc")
        for i in range(2):
            host.emit(1000 + i * 100, bar + i * 64, 64, OP_WRITE)
            vortex.emit(1050 + i * 100, bar + i * 64, 64, OP_READ)
        host.close()
        vortex.close()

        issues, _summaries = validate.validate_dir(d)
        check(any(
            issue.level == "ERROR" and "混用了不同观察层级" in issue.message
            for issue in issues
        ), "同一批跨源 trace 的 level 不一致必须报 ERROR")
    finally:
        shutil.rmtree(d)


def test_validate_catches_full_axi_header_and_control_edges():
    """覆盖 EXOKAY、非法 RESP/FIXED、unaligned 与地址位宽。"""
    d = tmpdir()
    try:
        shared = addrmap.REGIONS["shared_buffer"][0]
        writer = synth.SynthWriter(
            d, "host", level="interconnect", axi_data_bytes=16,
            synth_flag=True,
        )
        # EXOKAY 是成功响应，不应被计为 slave/decoder error。
        writer.emit_txn(
            1000, 1100, shared, 16, OP_READ, axi_id=1, resp=1
        )
        # 17 拍 FIXED 超过 AXI4 上限 16。
        writer._push(
            1200, shared + 0x100, 0, 17 * 16, 0, 10, 2,
            OP_READ, CHAN_AR, 16, 4, 0, 0, burst=0,
        )
        # 当前无 RSTRB 的可转换契约显式拒绝 unaligned 地址通道。
        writer._push(
            1300, shared + 0x203, 0, 8, 0, 11, 3,
            OP_READ, CHAN_AR, 1, 2, 0, 0,
        )
        # RESP=4 不在 AXI4 编码内，必须是协议 ERROR 而非普通 warning。
        writer._push(
            1400, shared + 0x300, 0, 4, 0, 12, 4,
            OP_READ, CHAN_R, 0, 2, 4, FLAG_LAST,
        )
        writer.close()

        issues, summaries = validate.validate_dir(
            d, require_heterogeneous=False
        )
        errors = "\n".join(
            issue.message for issue in issues if issue.level == "ERROR"
        )
        for category in ("bad_burst", "address_alignment", "bad_resp"):
            check(category in errors,
                  "完整 AXI 边界测试应报出 %s: %s" % (category, errors))
        check(summaries[0].resp_errors == 0,
              "EXOKAY 与非法编码都不能误计为 SLVERR/DECERR")
        check(not any(
            issue.level == "WARN" and "SLVERR/DECERR" in issue.message
            for issue in issues
        ), "EXOKAY 不应产生错误响应 warning")

        records = iter(list(read_records(writer.path))[3:4])
        try:
            convert.convert_memsim(records, io.StringIO(), 100)
            check(False, "unaligned AR 必须被 memsim converter 拒绝")
        except convert.ConvertError as exc:
            check("unaligned" in str(exc),
                  "unaligned 转换错误应解释 RSTRB 限制: %s" % exc)
    finally:
        shutil.rmtree(d)

    d = tmpdir()
    try:
        bar = addrmap.REGIONS["vortex_bar"][0]
        writer = synth.SynthWriter(
            d, "host", level="interconnect", axi_data_bytes=16,
            synth_flag=True,
        )
        writer._push(
            1000, bar, 0, 16, 0, 0, 0,
            OP_READ, CHAN_AR, 0, 4, 0, 0,
        )
        writer.close()
        rewrite_header(writer.path, axi_addr_bits=32)
        issues, _summaries = validate.validate_dir(
            d, require_heterogeneous=False
        )
        check(any(
            issue.level == "ERROR" and "address_span" in issue.message
            for issue in issues
        ), "只有地址通道时也必须按 axi_addr_bits 检查整笔范围")
    finally:
        shutil.rmtree(d)


def test_validate_catches_axi4_ordering_rules():
    """W 服从全局 AW 顺序，B/R 对同一 ID 不能重排。"""
    d = tmpdir()
    try:
        shared = addrmap.REGIONS["shared_buffer"][0]
        writer = synth.SynthWriter(
            d, "host", level="interconnect", axi_data_bytes=16,
            synth_flag=True,
        )

        # 两个写地址先后发出，却先发送第二笔 W；随后 B 也按同一 ID 逆序。
        for txn, address in ((0, shared), (1, shared + 16)):
            writer._push(
                1000 + txn * 10, address, 0, 16, 0, txn, 7,
                OP_WRITE, CHAN_AW, 0, 4, 0, 0,
            )
        for txn, address in ((1, shared + 16), (0, shared)):
            writer._push(
                1100 + (1 - txn) * 10, address, 0xffff, 16, 0, txn, 7,
                OP_WRITE, CHAN_W, 0, 4, 0, FLAG_LAST,
            )
        for txn, address in ((1, shared + 16), (0, shared)):
            writer._push(
                1200 + (1 - txn) * 10, address, 0, 0, 0, txn, 7,
                OP_WRITE, CHAN_B, 0, 4, 0, FLAG_LAST,
            )

        # 同一 ID 的两笔读先后发出，却让第二笔 R 先返回。
        for txn, address in ((2, shared + 32), (3, shared + 48)):
            writer._push(
                1300 + (txn - 2) * 10, address, 0, 16, 0, txn, 9,
                OP_READ, CHAN_AR, 0, 4, 0, 0,
            )
        for txn, address in ((3, shared + 48), (2, shared + 32)):
            writer._push(
                1400 + (3 - txn) * 10, address, 0, 16, 0, txn, 9,
                OP_READ, CHAN_R, 0, 4, 0, FLAG_LAST,
            )
        writer.close()

        issues, _summaries = validate.validate_dir(
            d, require_heterogeneous=False
        )
        errors = "\n".join(
            issue.message for issue in issues if issue.level == "ERROR"
        )
        check("write_data_order" in errors,
              "W 逆序必须报 write_data_order: %s" % errors)
        check("response_order" in errors,
              "同 ID B/R 逆序必须报 response_order: %s" % errors)
    finally:
        shutil.rmtree(d)


def test_validate_rejects_wrong_global_timebase_and_source_name():
    d = tmpdir()
    try:
        writer = synth.SynthWriter(d, "vortex")
        writer.emit(
            1000, addrmap.REGIONS["vortex_vram"][0], 64, OP_READ
        )
        writer.close()
        rewrite_header(
            writer.path,
            ticks_per_second=2 * addrmap.TICKS_PER_SECOND,
            name="not-vortex",
        )
        issues, _summaries = validate.validate_dir(
            d, require_heterogeneous=False
        )
        messages = "\n".join(
            issue.message for issue in issues if issue.level == "ERROR"
        )
        check("ticks_per_second" in messages,
              "错误全局 tick 基准必须报 ERROR: %s" % messages)
        check("规范名" in messages,
              "header name/src_id 不一致必须报 ERROR: %s" % messages)
    finally:
        shutil.rmtree(d)


def test_validate_explicit_fs_timebase():
    d = tmpdir()
    try:
        writer = synth.SynthWriter(d, "host")
        writer.emit(1000000, addrmap.REGIONS["shared_buffer"][0], 8, OP_READ)
        writer.emit(2000000, addrmap.REGIONS["shared_buffer"][0], 8, OP_READ)
        writer.close()
        rewrite_header(writer.path, ticks_per_second=10**15, clock_period_ticks=500000)
        meta_path = writer.path + ".meta.json"
        if os.path.exists(meta_path):
            with open(meta_path) as f:
                meta = json.load(f)
            meta.update(ticks_per_second=10**15, clock_period_ticks=500000)
            with open(meta_path, "w") as f:
                json.dump(meta, f)
        issues, summaries = validate.validate_dir(d, require_heterogeneous=False,
                                                 ticks_per_second=10**15)
        check(not any(i.level == "ERROR" for i in issues),
              "explicit fs timebase should validate: %r" % (issues,))
        check("0.001 us" in validate.format_report(issues, summaries),
              "1000000 fs must be reported as 0.001 us")
        check("0.001 us" in stats.format_report(d, window_ticks=1000000),
              "bandwidth window must use trace timebase")
        issues, _ = validate.validate_dir(d, require_heterogeneous=False)
        check(any(i.level == "ERROR" and "ticks_per_second" in i.message for i in issues),
              "default validator must still enforce the default timebase")
        rewrite_header(writer.path, clock_period_ticks=500)
        issues, _ = validate.validate_dir(d, require_heterogeneous=False,
                                         ticks_per_second=10**15)
        check(any(i.level == "ERROR" and "clock_period_ticks" in i.message for i in issues),
              "ps clock period in fs trace must be rejected")
    finally:
        shutil.rmtree(d)


def test_validate_rejects_duplicate_source_ids():
    d = tmpdir()
    try:
        synth.gen_cooperative(d, n_per_src=2)
        shutil.copyfile(
            os.path.join(d, "host.hettrace"),
            os.path.join(d, "host-copy.hettrace"),
        )
        issues, _summaries = validate.validate_dir(d)
        check(any(i.level == "ERROR" and "src_id=0" in i.message
                  and "重复" in i.message for i in issues),
              "重复 src_id 必须报 ERROR")
    finally:
        shutil.rmtree(d)


def test_validate_sees_filtered_unmapped_in_meta():
    """默认过滤不得把未映射地址伪装成普通 filtered。"""
    d = tmpdir()
    try:
        writer = synth.SynthWriter(d, "host")
        writer.emit(1000, addrmap.REGIONS["shared_buffer"][0], 8, OP_READ)
        writer.emit(1100, 0xDEADBEEF, 8, OP_READ)
        writer.close()
        issues, _summaries = validate.validate_dir(
            d, require_heterogeneous=False
        )
        check(any(i.level == "ERROR" and "meta 记录 1 次未映射" in i.message
                  for i in issues),
              "被 filter 拦下的未映射访问仍必须由 meta 报 ERROR")
    finally:
        shutil.rmtree(d)


def test_validate_catches_empty_dir():
    d = tmpdir()
    try:
        _i, _s, errs, _w = _run_validate(d)
        check(errs, "空目录应报 ERROR")
    finally:
        shutil.rmtree(d)


# ---------------------------------------------------------------------------
# 统计与转换
# ---------------------------------------------------------------------------

def test_stats_detects_sharing():
    d = tmpdir()
    try:
        synth.gen_cooperative(d, n_per_src=50)
        sizes, pairwise, lb = stats.footprint(d)
        check(len(sizes) == 3, "footprint 应覆盖 3 个源")
        check(lb == 64, "默认 line 应为 64 字节")
        # host 与 Vortex 经 BAR、host 与 NPU 经 shared_buffer 共享 line
        check(pairwise.get(("host", "vortex"), 0) > 0,
              "host 与 vortex 应有共享 line，实为 %r" % pairwise)
        check(pairwise.get(("coralnpu", "host"), 0) > 0
              or pairwise.get(("host", "coralnpu"), 0) > 0,
              "host 与 npu 应有共享 line，实为 %r" % pairwise)

        windows, srcs = stats.bandwidth_timeline(d, 100000)
        check(len(windows) > 0, "应产生带宽窗口")
        check(set(srcs) == {"host", "vortex", "coralnpu"}, "应覆盖三个源")
        overlap = sum(1 for w in windows
                      if sum(1 for n in srcs if w.per_src_count.get(n, 0) > 0) >= 2)
        check(overlap > 0, "协同负载应存在多源并发窗口")

        rep = stats.format_report(d, 100000)
        check("共享 line" in rep, "统计报告应含共享 line 段")
    finally:
        shutil.rmtree(d)


def test_stats_no_sharing_reports_zero():
    d = tmpdir()
    try:
        synth.gen_broken_no_sharing(d)
        _sizes, pairwise, _lb = stats.footprint(d)
        check(all(v == 0 for v in pairwise.values()),
              "各干各的负载不应有共享 line，实为 %r" % pairwise)
    finally:
        shutil.rmtree(d)


def test_convert_presets():
    d = tmpdir()
    try:
        synth.gen_cooperative(d, n_per_src=10)
        records, _ = merge.merge_dir(d)
        buf = io.StringIO()
        n = convert.convert(records, buf,
                            convert.PRESETS["readwrite"]["template"])
        lines = buf.getvalue().strip().splitlines()
        check(len(lines) == n, "行数应与返回值一致")
        check(all(l.split()[1] in ("R", "W") for l in lines),
              "readwrite 预设第二列应为 R/W")
        check(all(l.split()[0].startswith("0x") for l in lines),
              "readwrite 预设地址应为十六进制")

        # 按源过滤
        records, _ = merge.merge_dir(d)
        buf2 = io.StringIO()
        npu_id = addrmap.SOURCES["coralnpu"][0]
        n2 = convert.convert(records, buf2,
                             convert.PRESETS["timed"]["template"], {npu_id})
        check(0 < n2 < n, "按源过滤应减少行数: %d vs %d" % (n2, n))
        for l in buf2.getvalue().strip().splitlines():
            check(int(l.split()[1]) == npu_id, "过滤后只应剩 NPU 记录")
    finally:
        shutil.rmtree(d)

    d = tmpdir()
    try:
        synth.gen_axi_full(d, n=2)
        records, _ = merge.merge_dir(d)
        data_only = io.StringIO()
        n_data = convert.convert(
            records, data_only, convert.PRESETS["readwrite"]["template"]
        )
        records, _ = merge.merge_dir(d)
        all_channels = io.StringIO()
        n_all = convert.convert(
            records,
            all_channels,
            "{chan} {addr:#x}",
            include_control=True,
        )
        check(n_data == 2 * (4 + 4 + 1),
              "通用转换默认只应输出 W/R 数据拍，实为 %d" % n_data)
        check(n_all > n_data, "--include-control 应额外保留 AW/B/AR")
    finally:
        shutil.rmtree(d)


# ---------------------------------------------------------------------------
# 外部 mem_sim 投影
# ---------------------------------------------------------------------------
def test_convert_memsim_projection_and_mapping():
    """完整 AXI 从 AW/AR 发射时刻投影；设备 trace 则回退到 W/R。"""
    d = tmpdir()
    try:
        synth.gen_axi_full(d, n=4)
        records, _entries = merge.merge_dir(d)
        output = io.StringIO()
        mapping = io.StringIO()
        count = convert.convert_memsim(
            records, output, 100, map_fh=mapping
        )

        # 每轮 host 64B 写 4 拍、Vortex 64B 读 4 拍、NPU 16B 读 1 拍。
        check(count == 4 * (4 + 4 + 1),
              "AXI 地址通道展开数应为 36，实为 %d" % count)
        lines = output.getvalue().strip().splitlines()
        cycles = [int(line.split()[0]) for line in lines]
        check(cycles == sorted(cycles), "mem_sim inject_cycle 必须非递减")
        check(all(line.split()[1] in ("R", "W") for line in lines),
              "mem_sim 第二列必须为 R/W")

        rows = list(csv.DictReader(io.StringIO(mapping.getvalue())))
        check(len(rows) == count, "映射 sidecar 应与请求逐行对应")
        check([int(row["host_request_id"]) for row in rows]
              == list(range(count)), "host_request_id 必须零起始连续")
        check(
            all(row["projection"] in
                ("axi_write_data", "axi_read_address") for row in rows),
            "完整 trace 必须从 W/AR 投影",
        )
        check(any(row["projection"] == "axi_write_data" for row in rows),
              "写请求必须从 W 投影以保留 WSTRB")
        check(any(row["projection"] == "axi_read_address" for row in rows),
              "读请求必须从 AR 投影以保留发射 tick")

        # W 的 mapping 必须能逐拍回溯，同一 txn 不能全部写成 beat=0。
        host_write_rows = [
            row for row in rows
            if row["src_name"] == "host"
            and row["projection"] == "axi_write_data"
        ]
        first_write_txn = host_write_rows[0]["txn"]
        first_write_beats = [
            int(row["beat"]) for row in host_write_rows
            if row["txn"] == first_write_txn
        ]
        check(first_write_beats == [0, 1, 2, 3],
              "完整写事务的 mapping beat 应逐拍递增，实为 %r"
              % first_write_beats)

        first_vortex = next(row for row in rows if row["src_name"] == "vortex")
        check(int(first_vortex["tick"]) == 1100,
              "Vortex 读请求必须使用 AR tick，而不是较晚的 R tick")

        # data=/expect= 的十六进制字节数必须与 sidecar.size
        # 逐行一致，否则 mem_sim 会回退到 64B line_size。
        for line, row in zip(lines, rows):
            tokens = line.split()
            size = int(row["size"])
            payload_token = next(
                token for token in tokens
                if token.startswith("data=") or token.startswith("expect=")
            )
            check(len(payload_token.split("=", 1)[1]) == size * 2,
                  "mem_sim payload/expect 必须精确携带 %dB" % size)
            if tokens[1] == "W":
                mask_token = next(token for token in tokens
                                  if token.startswith("mask="))
                check(len(mask_token.split("=", 1)[1]) == size * 2,
                      "mem_sim 写 mask 必须与 payload 等长")

        # 设备 tap 只有 W/R；转换器必须仍然能直接使用，不能要求伪造 AW/AR。
        d2 = tmpdir()
        try:
            synth.gen_cooperative(d2, n_per_src=3)
            records2, _ = merge.merge_dir(d2)
            output2 = io.StringIO()
            mapping2 = io.StringIO()
            count2 = convert.convert_memsim(
                records2, output2, 100, map_fh=mapping2
            )
            check(count2 == 3 * (2 + 3 + 2),
                  "设备级 W/R 投影请求数应守恒，实为 %d" % count2)
            rows2 = list(csv.DictReader(io.StringIO(mapping2.getvalue())))
            check(all(row["projection"] == "data" for row in rows2),
                  "设备级 trace 应标明从 data channel 投影")
        finally:
            shutil.rmtree(d2)
    finally:
        shutil.rmtree(d)


def test_convert_memsim_preserves_partial_wstrb():
    d = tmpdir()
    try:
        shared = addrmap.REGIONS["shared_buffer"][0] + 8
        writer = synth.SynthWriter(
            d, "host", level="interconnect", axi_data_bytes=16,
            synth_flag=True,
        )
        writer._push(100, shared, 0, 4, 1, 0, 5, OP_WRITE, CHAN_AW,
                     0, 2, 0, 0)
        writer._push(110, shared, (1 << 8) | (1 << 10), 4, 1, 0, 5,
                     OP_WRITE, CHAN_W, 0, 2, 0, FLAG_LAST)
        writer._push(120, shared, 0, 0, 1, 0, 5, OP_WRITE, CHAN_B,
                     0, 2, 0, FLAG_LAST)
        writer.close()

        records, entries = merge.merge_dir(d)
        output = io.StringIO()
        mapping = io.StringIO()
        count = convert.convert_memsim(
            records, output, 10, map_fh=mapping,
            axi_data_bytes_by_src={entries[0][1].src_id: 16},
        )
        check(count == 1, "单拍部分写应投影为 1 条 mem_sim 请求")
        line = output.getvalue().strip()
        check("data=00000000" in line, "4B 写应用 4B dummy payload 携带大小")
        check("mask=ff00ff00" in line,
              "全局 WSTRB lane 8/10 应投影成相对 mask ff00ff00: %s" % line)
        row = next(csv.DictReader(io.StringIO(mapping.getvalue())))
        check(row["projection"] == "axi_write_data" and row["size"] == "4",
              "sidecar 应标明 W 投影与精确尺寸")
    finally:
        shutil.rmtree(d)


def test_cli_end_to_end():
    """跑真正的命令行，捕捉 import / 参数解析层面的问题。"""
    d = tmpdir()
    try:
        synth.gen_cooperative(d, n_per_src=30)
        env = dict(os.environ)
        env["PYTHONPATH"] = os.path.join(ROOT, "tools")

        for args, want_rc in (
            (["validate", d], 0),
            (["stats", d], 0),
            (["merge", d], 0),
            (["convert", d, "--preset", "readwrite"], 0),
            (["convert", "--list-presets"], 0),
            (["dump", os.path.join(d, "host.hettrace"), "-n", "5"], 0),
        ):
            r = subprocess.run(
                [sys.executable, "-m", "hettrace"] + args,
                capture_output=True, text=True, env=env, cwd=ROOT,
            )
            check(r.returncode == want_rc,
                  "hettrace %s 应返回 %d，实为 %d\nstderr:\n%s"
                  % (" ".join(args[:2]), want_rc, r.returncode, r.stderr))

        # mem_sim 是 convert 的一个预设；-o 时自动生成 request-id 映射。
        memsim_path = os.path.join(d, "mem_sim.trace")
        r = subprocess.run(
            [sys.executable, "-m", "hettrace", "convert", d,
             "--preset", "memsim", "--ticks-per-cycle", "100",
             "-o", memsim_path],
            capture_output=True, text=True, env=env, cwd=ROOT,
        )
        check(r.returncode == 0, "memsim convert CLI 应通过:\n%s" % r.stderr)
        check(os.path.getsize(memsim_path) > 100, "mem_sim trace 不应为空")
        map_path = memsim_path + ".map.csv"
        check(os.path.getsize(map_path) > 100, "request-id 映射不应为空")

        # 坏 trace 应让 validate 以非零退出，这样能直接用于 CI 门禁
        d2 = tmpdir()
        try:
            synth.gen_broken_no_sharing(d2)
            r = subprocess.run(
                [sys.executable, "-m", "hettrace", "validate", d2],
                capture_output=True, text=True, env=env, cwd=ROOT,
            )
            check(r.returncode != 0, "坏 trace 应让 validate 非零退出")
        finally:
            shutil.rmtree(d2)
    finally:
        shutil.rmtree(d)


def test_llm_memory_benchmark_generator():
    """LLM benchmark 必须产出三源、合法、可重复分析的标准 HETTrace。"""
    d = tmpdir()
    try:
        trace_dir = os.path.join(d, "traces")
        script = os.path.join(ROOT, "workloads", "llm_memory", "generate_trace.py")
        r = subprocess.run(
            [
                sys.executable,
                script,
                "--output", trace_dir,
                "--hidden-size", "32",
                "--layers", "2",
                "--context-tokens", "4",
                "--decode-tokens", "2",
                "--layer-gap", "1000",
            ],
            capture_output=True,
            text=True,
            cwd=ROOT,
            timeout=60,
        )
        check(r.returncode == 0, "LLM trace 生成器应通过:\n%s%s" % (r.stdout, r.stderr))
        if r.returncode != 0:
            return

        entries = discover(trace_dir)
        check([header.name for _path, header in entries]
              == ["host", "vortex", "coralnpu"],
              "LLM benchmark 应生成规范的三源 trace")
        issues, summaries = validate.validate_dir(trace_dir)
        errors = [issue for issue in issues if issue.level == "ERROR"]
        check(not errors, "LLM benchmark trace 应通过 validate，实际: %r" % errors)

        with open(os.path.join(trace_dir, "benchmark.json")) as handle:
            manifest = json.load(handle)
        total = sum(values["requests"]
                    for values in manifest["source_stats"].values())
        observed = sum(summary.count for summary in summaries)
        check(total == observed and total > 0,
              "manifest 请求数应与三份 trace 守恒: %d vs %d" % (total, observed))
        check(manifest["functional_model"] is False,
              "LLM benchmark 必须机器可读地声明不是功能模型")

        records, _entries = merge.merge_dir(trace_dir)
        memsim_trace = io.StringIO()
        count = convert.convert_memsim(records, memsim_trace, 100)
        check(count == total,
              "LLM trace 转为外部 mem_sim 请求后数量必须守恒")
    finally:
        shutil.rmtree(d)


def test_cli_rejects_bad_args():
    """坏参数与坏文件必须换来干净的错误信息，不是 traceback、更不是卡死。

    这四种情形都曾经真的出过：--line 0 除零、--window 0 静默死循环（窗口永远推
    不动，表现是卡住而不是报错）、--sources 打错名字抛 ValueError、截断的 trace
    让 TraceError 一路冒到栈顶。它们都是**用户输入**，工具崩在这上面等于把"你输
    错了"报成"工具坏了"，所以每一种都在这里钉住。

    每个子进程都带 timeout：死循环那一类的回归只有超时能抓到。
    """
    d = tmpdir()
    try:
        synth.gen_cooperative(d, n_per_src=20)
        env = dict(os.environ)
        env["PYTHONPATH"] = os.path.join(ROOT, "tools")

        def run(args):
            try:
                return subprocess.run(
                    [sys.executable, "-m", "hettrace"] + args,
                    capture_output=True, text=True, env=env, cwd=ROOT,
                    timeout=60,
                )
            except subprocess.TimeoutExpired:
                return None

        # 非法参数：argparse 层就该挡掉（rc=2）
        for args in (
            ["stats", d, "--window", "0"],
            ["stats", d, "--line", "0"],
            ["stats", d, "--window", "-1"],
            ["stats", d, "-n", "-1"],
            ["convert", d, "--preset", "memsim", "--ticks-per-cycle", "0"],
        ):
            r = run(args)
            check(r is not None,
                  "hettrace %s 卡死了（超时）" % " ".join(args[1:]))
            if r is None:
                continue
            check(r.returncode == 2,
                  "hettrace %s 应以 2 退出（argparse），实为 %d"
                  % (" ".join(args[1:]), r.returncode))
            check("Traceback" not in r.stderr,
                  "hettrace %s 不应打 traceback:\n%s"
                  % (" ".join(args[1:]), r.stderr))

        # 认不出的源名：给出可用源名，不是 ValueError
        r = run(["convert", d, "--sources", "nosuch"])
        check(r is not None and r.returncode == 1,
              "--sources nosuch 应以 1 退出，实为 %r"
              % (None if r is None else r.returncode))
        if r is not None:
            check("Traceback" not in r.stderr,
                  "--sources nosuch 不应打 traceback:\n%s" % r.stderr)
            check("coralnpu" in r.stderr,
                  "--sources 的错误信息里应列出可用源名:\n%s" % r.stderr)

        r = run(["convert", d, "--preset", "memsim"])
        check(r is not None and r.returncode == 1,
              "memsim 预设缺少时钟换算应以 1 退出")
        if r is not None:
            check("Traceback" not in r.stderr and "ticks-per-cycle" in r.stderr,
                  "缺少时钟换算应给干净错误:\n%s" % r.stderr)

        r = run(["convert", d, "--preset", "memsim",
                 "--ticks-per-cycle", "100", "--sources", "nosuch"])
        check(r is not None and r.returncode == 1,
              "memsim convert 未知源应以 1 退出")
        if r is not None:
            check("Traceback" not in r.stderr and "coralnpu" in r.stderr,
                  "memsim convert 未知源应列出可用名字:\n%s" % r.stderr)

        same = os.path.join(d, "same.out")
        r = run(["convert", d, "--preset", "memsim",
                 "--ticks-per-cycle", "100", "-o", same,
                 "--map-output", same])
        check(r is not None and r.returncode == 1,
              "trace/map 同路径应以 1 退出")
        if r is not None:
            check("同一个文件" in r.stderr and "Traceback" not in r.stderr,
                  "输出路径冲突应给干净错误:\n%s" % r.stderr)

        # 源名与数字 id 混用仍应正常工作
        r = run(["convert", d, "--sources", "vortex,2", "--preset", "timed"])
        check(r is not None and r.returncode == 0,
              "--sources vortex,2 应正常工作，实为 %r"
              % (None if r is None else r.returncode))
    finally:
        shutil.rmtree(d)

    # 截断的 trace：这是本工具**预期要检测**的失效形态，必须报得干净
    d = tmpdir()
    try:
        synth.gen_cooperative(d, n_per_src=20)
        p = os.path.join(d, "host.hettrace")
        with open(p, "r+b") as f:
            f.truncate(os.path.getsize(p) - 13)  # 半条记录
        env = dict(os.environ)
        env["PYTHONPATH"] = os.path.join(ROOT, "tools")
        for args in (["merge", d], ["stats", d], ["dump", p]):
            r = subprocess.run(
                [sys.executable, "-m", "hettrace"] + args,
                capture_output=True, text=True, env=env, cwd=ROOT, timeout=60,
            )
            check(r.returncode == 1,
                  "截断的 trace 应让 hettrace %s 以 1 退出，实为 %d"
                  % (args[0], r.returncode))
            check("Traceback" not in r.stderr,
                  "截断的 trace 不应打 traceback (%s):\n%s" % (args[0], r.stderr))
            check("hettrace:" in r.stderr,
                  "截断的 trace 应给出 hettrace: 前缀的信息 (%s):\n%s"
                  % (args[0], r.stderr))
    finally:
        shutil.rmtree(d)


def test_cpp_writer_interop():
    """交叉验证：C++ TraceWriter 写的文件，Python 侧必须能原样读出。

    这是整条链上最关键的一个测试 —— 两侧的格式定义各写一遍，任何字段偏移或
    字节序分歧都会在这里暴露，而不是等到分析真 trace 时出现无意义的地址。
    """
    src = os.path.join(ROOT, "libhettrace", "tests", "test_writer.cc")
    inc = os.path.join(ROOT, "libhettrace", "include")
    if not os.path.exists(src):
        check(False, "找不到 C++ 测试源")
        return

    d = tmpdir()
    try:
        exe = os.path.join(d, "cpp_writer_test")
        r = subprocess.run(
            ["g++", "-std=c++17", "-I", inc, src, "-o", exe],
            capture_output=True, text=True,
        )
        if r.returncode != 0:
            check(False, "C++ 测试编译失败:\n%s" % r.stderr)
            return

        outdir = os.path.join(d, "traces")
        os.makedirs(outdir)
        env = dict(os.environ)
        env["HETTRACE_DIR"] = outdir
        env["HETTRACE_FORMAT"] = "bin"
        r = subprocess.run([exe], capture_output=True, text=True, env=env)
        check(r.returncode == 0, "C++ 测试应通过:\n%s%s" % (r.stdout, r.stderr))

        # C++ 侧写的 vortex.hettrace：3 条记录，字段已知
        p = os.path.join(outdir, "vortex.hettrace")
        check(os.path.exists(p), "C++ 应产出 vortex.hettrace")
        if not os.path.exists(p):
            return

        hdr = read_header(p)
        bar = addrmap.REGIONS["vortex_bar"][0]
        vram = addrmap.REGIONS["vortex_vram"][0]
        check(hdr.name == "vortex", "Python 应读出源名 vortex，实为 %r" % hdr.name)
        check(hdr.src_id == addrmap.SOURCES["vortex"][0], "src_id 应一致")
        check(hdr.clock_period_ticks == addrmap.SOURCES["vortex"][3],
              "时钟周期应一致")
        check(hdr.filtered_dram, "默认应为 dram 过滤")

        recs = list(read_records(p))
        check(len(recs) == 3, "应读出 3 条，实为 %d" % len(recs))
        if len(recs) == 3:
            check(recs[0].tick == 1000 and recs[0].addr == bar
                  and recs[0].size == 64 and recs[0].op == OP_READ
                  and recs[0].ctx == 7 and recs[0].seq == 0,
                  "记录 0 应逐字段一致，实为 %r" % (recs[0],))
            check(recs[1].op == OP_WRITE and recs[1].addr == bar + 64,
                  "记录 1 应为写 BAR 下一行")
            check(recs[2].addr == vram, "记录 2 应为 VRAM 地址")

        # burst 展开的产物也要能读，且地址真的递增
        pb = os.path.join(outdir, "npu_burst.hettrace")
        if os.path.exists(pb):
            brecs = list(read_records(pb))
            check(len(brecs) == 4, "burst 应展开为 4 条")
            addrs = [r.addr for r in brecs]
            shared = addrmap.REGIONS["shared_buffer"][0]
            check(addrs == [shared + i * 16 for i in range(4)],
                  "burst 地址应按拍递增，实为 %r" % [hex(a) for a in addrs])
    finally:
        shutil.rmtree(d)


def main():
    tests = [v for k, v in sorted(globals().items()) if k.startswith("test_")]
    for t in tests:
        try:
            t()
        except Exception as e:  # noqa: BLE001
            global _failed
            _failed += 1
            sys.stderr.write("FAIL %s 抛出异常: %r\n" % (t.__name__, e))
    print("hettrace tools: %d 项检查, %d 项失败 (%d 个用例)"
          % (_checks, _failed, len(tests)))
    return 1 if _failed else 0


if __name__ == "__main__":
    sys.exit(main())
