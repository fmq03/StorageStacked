"""hettrace 命令行入口。

    python3 -m hettrace validate  <trace_dir>
    python3 -m hettrace merge     <trace_dir> [-o out.txt]
    python3 -m hettrace stats     <trace_dir> [--window TICKS] [--line BYTES]
    python3 -m hettrace dump      <trace_file> [-n N]
    python3 -m hettrace convert   <trace_dir> --preset memsim \
        --ticks-per-cycle N [-o out.trace]
"""

from __future__ import annotations

import argparse
import os
import sys

from . import addrmap, convert as convert_mod, merge, stats, validate
from .reader import (
    CHAN_NAMES,
    FLAG_DMA,
    FLAG_INSTR,
    FLAG_PREFETCH,
    OP_WRITE,
    TraceError,
    read_header,
    read_meta,
    read_records,
)


def _positive_int(s):
    """argparse 类型：必须为正。

    0 不是"没有限制"而是死循环 / 除零：--window 0 会让 bandwidth_timeline 的
    窗口永远推不动，--line 0 会在 footprint 里除零。这两个都出现过，所以在
    参数层就挡掉，而不是等到跑起来。
    """
    v = int(s)
    if v <= 0:
        raise argparse.ArgumentTypeError("必须是正整数，收到 %r" % s)
    return v


def _nonneg_int(s):
    v = int(s)
    if v < 0:
        raise argparse.ArgumentTypeError("不能为负，收到 %r" % s)
    return v


def _parse_sources(raw):
    if not raw:
        return None
    source_ids = set()
    for tok in raw.split(","):
        tok = tok.strip()
        if tok in addrmap.SOURCES:
            source_ids.add(addrmap.SOURCES[tok][0])
            continue
        try:
            source_ids.add(int(tok, 0))
        except ValueError:
            raise ValueError(
                "--sources 里的 %r 既不是源名也不是数字 id。可用源名: %s"
                % (tok, ", ".join(sorted(addrmap.SOURCES)))
            )
    return source_ids


def cmd_validate(args):
    issues, summaries = validate.validate_dir(
        args.trace_dir, require_heterogeneous=not args.allow_single_source,
        ticks_per_second=args.ticks_per_second,
    )
    print(validate.format_report(issues, summaries))
    return 1 if any(i.level == "ERROR" for i in issues) else 0


def cmd_merge(args):
    records, entries = merge.merge_dir(args.trace_dir)
    if not entries:
        sys.stderr.write("%s 下没有 trace 文件\n" % args.trace_dir)
        return 1
    sys.stderr.write(
        "归并 %d 个源: %s\n"
        % (len(entries), ", ".join(h.name for _p, h in entries))
    )
    if args.output:
        with open(args.output, "w") as fh:
            n = merge.write_text(records, fh)
        sys.stderr.write("写出 %d 条到 %s\n" % (n, args.output))
    else:
        n = merge.write_text(records, sys.stdout)
        sys.stderr.write("写出 %d 条\n" % n)
    return 0


def cmd_stats(args):
    print(stats.format_report(args.trace_dir, args.window, args.line))
    return 0


def cmd_dump(args):
    try:
        hdr = read_header(args.trace_file)
    except (TraceError, OSError) as e:
        sys.stderr.write("%s\n" % e)
        return 1
    print("# 文件: %s" % args.trace_file)
    print(
        "# src_id=%d name=%s level=%s clock_period_ticks=%d filter=%s"
        % (
            hdr.src_id,
            hdr.name,
            addrmap.LEVEL_NAMES.get(hdr.level, hdr.level),
            hdr.clock_period_ticks,
            "dram" if hdr.filtered_dram else "all",
        )
    )
    meta = read_meta(args.trace_file)
    if meta:
        print(
            "# meta: emitted=%s filtered=%s unmapped=%s non_monotonic=%s"
            % (
                meta.get("emitted"),
                meta.get("filtered"),
                meta.get("unmapped"),
                meta.get("non_monotonic"),
            )
        )
    print(
        "# %-14s %-4s %-3s %-12s %6s %5s %7s %6s %6s %-6s %s"
        % (
            "tick", "chan", "op", "addr", "size", "id", "txn", "ctx",
            "seq", "flags", "region",
        )
    )
    n = 0
    for r in read_records(args.trace_file):
        if args.count and n >= args.count:
            print("# ... (--count %d 截断)" % args.count)
            break
        print(
            "  %-14d %-4s %-3s 0x%-10x %6d %5d %7d %6d %6d 0x%-4x %s"
            % (
                r.tick,
                CHAN_NAMES.get(r.chan, "??"),
                "W" if r.op == OP_WRITE else "R",
                r.addr,
                r.size,
                r.axi_id,
                r.txn,
                r.ctx,
                r.seq,
                r.flags,
                addrmap.region_of(r.addr) or "-",
            )
        )
        n += 1
    return 0


def cmd_convert(args):
    if args.list_presets:
        print(convert_mod.list_presets())
        return 0

    template = args.template
    selected_preset = None
    if template is None:
        if args.preset not in convert_mod.PRESETS:
            sys.stderr.write(
                "未知预设 %r。可用: %s\n"
                % (args.preset, ", ".join(convert_mod.PRESETS))
            )
            return 1
        selected_preset = args.preset
        template = convert_mod.PRESETS[args.preset]["template"]

    try:
        srcs = _parse_sources(args.sources)
    except ValueError as exc:
        sys.stderr.write("%s\n" % exc)
        return 1

    records, entries = merge.merge_dir(args.trace_dir)
    if not entries:
        sys.stderr.write("%s 下没有 trace 文件\n" % args.trace_dir)
        return 1

    excluded_flags = 0
    if args.exclude_instr:
        excluded_flags |= FLAG_INSTR
    if args.exclude_prefetch:
        excluded_flags |= FLAG_PREFETCH
    if args.exclude_dma:
        excluded_flags |= FLAG_DMA

    is_memsim = selected_preset == "memsim"
    if is_memsim and args.ticks_per_cycle is None:
        sys.stderr.write(
            "hettrace convert: memsim 预设需要 --ticks-per-cycle，"
            "用它把 HETTrace tick 显式换算成控制器 cycle\n"
        )
        return 1

    map_path = args.map_output
    if is_memsim and args.output and not args.no_map and not map_path:
        map_path = args.output + ".map.csv"
    if map_path and args.output:
        if os.path.abspath(map_path) == os.path.abspath(args.output):
            sys.stderr.write("hettrace convert: trace 与映射 sidecar 不能是同一个文件\n")
            return 1

    out_fh = sys.stdout
    map_fh = None
    try:
        if args.output:
            out_fh = open(args.output, "w")
        if map_path:
            map_fh = open(map_path, "w", newline="")
        if is_memsim:
            bus_widths = {}
            for _path, header in entries:
                if header.src_id in bus_widths:
                    raise convert_mod.ConvertError(
                        "src_id=%d 在多个 trace 文件中重复，"
                        "无法确定 mem_sim 投影宽度" % header.src_id
                    )
                bus_widths[header.src_id] = header.axi_data_bytes
            n = convert_mod.convert_memsim(
                records,
                out_fh,
                args.ticks_per_cycle,
                srcs,
                map_fh=map_fh,
                excluded_flags=excluded_flags,
                allow_unmapped=args.allow_unmapped,
                axi_data_bytes_by_src=bus_widths,
            )
        else:
            n = convert_mod.convert(
                records,
                out_fh,
                template,
                srcs,
                include_control=args.include_control,
                ticks_per_cycle=args.ticks_per_cycle or 1,
                excluded_flags=excluded_flags,
            )
    finally:
        if map_fh is not None:
            map_fh.close()
        if args.output and out_fh is not sys.stdout:
            out_fh.close()

    destination = "到 %s" % args.output if args.output else ""
    sys.stderr.write("写出 %d 条%s\n" % (n, destination))
    if map_path:
        sys.stderr.write("写出请求映射到 %s\n" % map_path)
    return 0


def build_parser():
    p = argparse.ArgumentParser(
        prog="hettrace", description="异构访存 trace 工具"
    )
    sub = p.add_subparsers(dest="cmd")

    v = sub.add_parser("validate", help="校验 trace 是否可用于分析")
    v.add_argument("trace_dir")
    v.add_argument("--ticks-per-second", type=_positive_int,
                   default=addrmap.TICKS_PER_SECOND,
                   help="Expected global tick frequency (default: address map)")
    v.add_argument(
        "--allow-single-source", action="store_true",
        help="独立设备 bring-up：仍做文件内全部检查，但不要求跨源交接",
    )
    v.set_defaults(func=cmd_validate)

    m = sub.add_parser("merge", help="按全局 tick 归并多源 trace")
    m.add_argument("trace_dir")
    m.add_argument("-o", "--output")
    m.set_defaults(func=cmd_merge)

    s = sub.add_parser("stats", help="带宽 / footprint / 共享度统计")
    s.add_argument("trace_dir")
    s.add_argument(
        "--window", type=_positive_int, default=1000000,
        help="带宽窗口 tick 数，默认 1e6 (=1us)")
    s.add_argument("--line", type=_positive_int, default=64,
                   help="cache line 字节数")
    s.set_defaults(func=cmd_stats)

    d = sub.add_parser("dump", help="人读单个 trace 文件")
    d.add_argument("trace_file")
    d.add_argument("-n", "--count", type=_nonneg_int, default=50,
                   help="最多打印多少条，0 表示全部")
    d.set_defaults(func=cmd_dump)

    c = sub.add_parser("convert", help="转换为下游 DRAM 模拟器格式")
    c.add_argument("trace_dir", nargs="?")
    c.add_argument("--preset", default="readwrite")
    c.add_argument("--template", help="自定义格式串，覆盖 --preset")
    c.add_argument("--sources", help="只保留这些源，逗号分隔（名字或 id）")
    c.add_argument(
        "--ticks-per-cycle", type=_positive_int,
        help="HETTrace tick 到目标 cycle 的整数除数；memsim 预设必须显式给出",
    )
    c.add_argument(
        "--include-control", action="store_true",
        help="自定义/通用预设也输出 AW/B/AR；默认只输出 W/R 数据通道",
    )
    c.add_argument("--exclude-instr", action="store_true")
    c.add_argument("--exclude-prefetch", action="store_true")
    c.add_argument("--exclude-dma", action="store_true")
    c.add_argument(
        "--allow-unmapped", action="store_true",
        help="memsim 投影时允许 addrmap 未声明的地址（默认拒绝）",
    )
    c.add_argument(
        "--map-output",
        help="memsim host_request_id 到 AXI 记录的 CSV；默认 OUTPUT.map.csv",
    )
    c.add_argument(
        "--no-map", action="store_true",
        help="指定 -o 时不自动生成 memsim 映射 sidecar",
    )
    c.add_argument("-o", "--output")
    c.add_argument("--list-presets", action="store_true")
    c.set_defaults(func=cmd_convert)

    return p


def main(argv=None):
    p = build_parser()
    args = p.parse_args(argv)
    if not getattr(args, "func", None):
        p.print_help()
        return 1
    if args.cmd == "convert" and args.list_presets:
        return cmd_convert(args)
    if args.cmd == "convert" and not args.trace_dir:
        p.error("convert 需要 trace_dir（或用 --list-presets）")

    try:
        return args.func(args)
    except TraceError as e:
        # 截断的 trace 是 read_records 在流中途才发现的（尾部残余字节），所以
        # 这个异常可以从 merge / stats / convert / dump 的任何一处冒出来。它是
        # 本工具**预期要检测**的失效形态，用 traceback 报出来只会让人以为是工具
        # 自己崩了 —— 而且 merge/convert 此时已经写出了一部分内容。
        sys.stderr.write("hettrace: %s\n" % e)
        sys.stderr.write(
            "         先跑 hettrace validate 看是哪个源出的问题。"
            "merge / convert 此前写出的内容是残缺的，不要使用。\n"
        )
        return 1
    except convert_mod.ConvertError as e:
        sys.stderr.write("hettrace convert: %s\n" % e)
        return 1
    except BrokenPipeError:
        # `hettrace merge dir | head` 的正常收场。必须排在 OSError 之前 ——
        # BrokenPipeError 是它的子类。按 CPython 官方建议把 stdout 重定向到
        # devnull，否则解释器退出时刷缓冲还会再抛一次，屏幕上留下
        # "Exception ignored in: <_io.TextIOWrapper name='<stdout>'>"。
        devnull = os.open(os.devnull, os.O_WRONLY)
        os.dup2(devnull, sys.stdout.fileno())
        return 1
    except OSError as e:
        sys.stderr.write("hettrace: %s\n" % e)
        return 1
    except KeyboardInterrupt:
        return 130


if __name__ == "__main__":
    sys.exit(main())
