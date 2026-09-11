#!/usr/bin/env python3
"""Run auditable, workload-directed HBM/LPDDR architecture sweeps."""

from __future__ import annotations

import argparse
import csv
import html
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
from result_io import number, read_result


@dataclass(frozen=True)
class Case:
    group: str
    name: str
    overrides: dict[str, str | int]


@dataclass(frozen=True)
class Check:
    status: str
    name: str
    detail: str


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=ROOT / "build-clang-debug/hbm_sim")
    parser.add_argument("--standards", default="hbm4,lpddr6",
                        help="comma-separated subset of hbm3,hbm4,lpddr5,lpddr6")
    parser.add_argument("--requests", type=int, default=512,
                        help="requests generated for every directed workload")
    parser.add_argument("--inject-interval", type=int, default=2,
                        help="arrival spacing used by the refresh workload")
    parser.add_argument("--case-timeout", type=float, default=120.0,
                        help="wall-clock timeout in seconds for one simulator case")
    parser.add_argument("--out", type=Path, default=ROOT / "outputs/experiments/architecture_sweep")
    return parser.parse_args()


def family(standard: str) -> str:
    if standard in {"hbm3", "hbm4"}:
        return "hbm"
    if standard in {"lpddr5", "lpddr6"}:
        return "lpddr"
    raise SystemExit(f"unsupported standard: {standard}")


def transaction_bytes(standard: str) -> int:
    # 两个权威 master 当前对四个标准都使用 32 B DRAM transaction；
    # frontend 的 64 B host line 会再拆成两个 transaction。这里必须使用
    # transaction 粒度编码 column/bank/row，不能误用 host line_size。
    values = {"hbm3": 32, "hbm4": 32, "lpddr5": 32, "lpddr6": 32}
    try:
        return values[standard]
    except KeyError as error:
        raise SystemExit(f"unsupported standard: {standard}") from error


def cases() -> list[Case]:
    # 每组只改变目标维度，其他组织参数和工作负载由下方函数固定。
    return [
        Case("bank", "bg2_bank4", {"bank_groups": 2, "banks_per_group": 4}),
        Case("bank", "bg4_bank4", {"bank_groups": 4, "banks_per_group": 4}),
        Case("bank", "bg4_bank8", {"bank_groups": 4, "banks_per_group": 8}),
        Case("geometry", "row32768_col512", {"rows": 32768, "columns": 512}),
        Case("geometry", "row65536_col1024", {"rows": 65536, "columns": 1024}),
        Case("geometry", "row131072_col2048", {"rows": 131072, "columns": 2048}),
        Case("refresh", "per_bank", {"refresh_policy": "per_bank"}),
        Case("refresh", "all_bank", {"refresh_policy": "all_bank"}),
    ]


def as_float(stats: dict[str, str], key: str) -> float:
    return number(stats, key)


def common_overrides(standard: str, case: Case) -> dict[str, str | int]:
    # 只保留一个 controller，避免默认多 channel 组织掩盖目标维度，也缩短实验时间。
    values: dict[str, str | int] = {
        "model_name": f"architecture_sweep_{case.name}",
        "stack_count": 1,
        "single_controller": "true",
        "channels": 1,
        "pseudo_channels": 1,
        "sids": 1,
        "ranks": 1,
        "mem_phy_mode": "direct",
        "address_mapping": "default",
        "supports_refresh": "true" if case.group == "refresh" else "false",
        "max_cycles": 10_000_000,
    }
    if family(standard) == "hbm":
        values["hbm_full_32_channel_stack"] = "false"
    elif standard == "lpddr6" and case.group != "refresh":
        # LPDDR6 baseline 支持 REFdb，并要求可成对的偶数 BG。bank/geometry
        # 实验已经关闭 refresh；若 geometry 缩到单 BG，还必须同步关闭这个
        # 不会执行的 REFdb 能力，否则会得到“组织与已声明能力不兼容”的正确
        # 配置错误，而不是目标几何实验。
        values["lpddr_dual_bank_refresh"] = "false"
    if case.group == "bank":
        values.update({"rows": 4096, "columns": 64})
    elif case.group == "geometry":
        values.update({"bank_groups": 1, "banks_per_group": 1})
    else:
        # 相同刷新间隔用于隔离 scope；这是研究型压力参数，并非 baseline JEDEC 值。
        values.update({
            "bank_groups": 2,
            "banks_per_group": 4,
            "rows": 4096,
            "columns": 64,
            "nRFC": 16,
            "nRFCpb": 16,
            "nREFI": 128,
            "nREFIpb": 128,
            "nREFDB2ACT": 16,
            "nREFDB2REFDBS": 16,
            "nREFDB2REFDBL": 16,
            "timing_override_source": "research",
        })
    values.update(case.overrides)
    return values


def overlay_text(standard: str, case: Case) -> str:
    lines = ["[override]"]
    lines.extend(f"{key} = {value}" for key, value in common_overrides(standard, case).items())
    return "\n".join(lines) + "\n"


def default_address(*, row: int, column: int, bank_group: int, bank: int,
                    columns: int, bank_groups: int, banks_per_group: int,
                    tx_bytes: int) -> int:
    # AddressMapper::Default 的低位顺序是 column, bank, BG, PC, SID, rank,
    # channel, row。本实验把 PC/SID/rank/channel 固定为 1，故可化简如下。
    line = column + columns * (bank + banks_per_group * (bank_group + bank_groups * row))
    return line * tx_bytes


def write_trace(path: Path, standard: str, case: Case, requests: int,
                inject_interval: int) -> None:
    tx_bytes = transaction_bytes(standard)
    config = common_overrides(standard, case)
    columns = int(config["columns"])
    bank_groups = int(config["bank_groups"])
    banks_per_group = int(config["banks_per_group"])
    lines: list[str] = []
    for index in range(requests):
        if case.group == "bank":
            # 同时到达并轮转全部 bank；同一 bank 的每轮访问一个从未使用过的
            # row，避免 row wrap 产生的 FR-FCFS 命中把“可并行 bank 数”与
            # “调度器重排行命中”混在一起。column 固定为 0；若 host line 会拆成
            # 两个 transaction，第二个 transaction 仍只是同一 row 的相邻列。
            flat_bank = index % (bank_groups * banks_per_group)
            bank_group = flat_bank // banks_per_group
            bank = flat_bank % banks_per_group
            row = index // (bank_groups * banks_per_group)
            column = 0
            arrival = 0
        elif case.group == "geometry":
            # 在至少 4096 transaction 的固定 footprint 上等距取样。即使 smoke
            # 只有几十个请求，512/1024/2048-column case 也会跨不同数量的 row。
            footprint_lines = max(4096, requests)
            logical_line = index * footprint_lines // requests
            row = (logical_line // columns) % int(config["rows"])
            column = logical_line % columns
            bank_group = 0
            bank = 0
            arrival = 0
        else:
            # 持续负载让自动 REFpb/REFdb 与 REFab 都能介入调度。
            flat_bank = index % (bank_groups * banks_per_group)
            bank_group = flat_bank // banks_per_group
            bank = flat_bank % banks_per_group
            row = (index // (bank_groups * banks_per_group)) % 8
            column = index % columns
            arrival = index * inject_interval
        address = default_address(
            row=row, column=column, bank_group=bank_group, bank=bank,
            columns=columns, bank_groups=bank_groups,
            banks_per_group=banks_per_group, tx_bytes=tx_bytes,
        )
        lines.append(f"{arrival} R 0x{address:x}")
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def run_case(args: argparse.Namespace, standard: str, case: Case,
             ordinal: int, total: int) -> dict[str, object]:
    case_dir = args.out / standard / f"{case.group}_{case.name}"
    case_dir.mkdir(parents=True, exist_ok=True)
    trace_path = case_dir / "workload.trace"
    write_trace(trace_path, standard, case, args.requests, args.inject_interval)
    print(f"[{ordinal}/{total}] {standard.upper()} {case.group}/{case.name}", flush=True)
    started = time.monotonic()
    with tempfile.NamedTemporaryFile("w", suffix=".cfg", encoding="utf-8") as overlay:
        overlay.write(overlay_text(standard, case))
        overlay.flush()
        command = [
            str(args.binary), "--config", str(ROOT / f"configs/{family(standard)}.cfg"),
            "--standard", standard, "--config", overlay.name,
            "--trace", str(trace_path), "--requests", "0",
            "--inject-interval", str(args.inject_interval),
            "--dump-resolved-config", str(case_dir / "resolved.cfg"),
            "--stats-view", "diagnostic", "--stats-json", str(case_dir / "result.json"),
        ]
        try:
            completed = subprocess.run(
                command, cwd=ROOT, check=True, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                timeout=args.case_timeout,
            )
        except subprocess.TimeoutExpired as error:
            output = error.stdout or ""
            if isinstance(output, bytes):
                output = output.decode(errors="replace")
            (case_dir / "stats.txt").write_text(output, encoding="utf-8")
            raise SystemExit(
                f"case timed out after {args.case_timeout:g}s: {standard} {case.group}/{case.name}; "
                f"partial output: {case_dir / 'stats.txt'}"
            ) from error
        except subprocess.CalledProcessError as error:
            (case_dir / "stats.txt").write_text(error.stdout or "", encoding="utf-8")
            raise SystemExit(
                f"case failed: {standard} {case.group}/{case.name}; "
                f"output: {case_dir / 'stats.txt'}"
            ) from error
    elapsed = time.monotonic() - started
    (case_dir / "stats.txt").write_text(completed.stdout, encoding="utf-8")
    stats = read_result(case_dir / "result.json", require_completed=True)
    hits = as_float(stats, "row_hits")
    misses = as_float(stats, "row_misses")
    conflicts = as_float(stats, "row_conflicts")
    decisions = hits + misses + conflicts
    values = common_overrides(standard, case)
    return {
        "standard": standard.upper(), "group": case.group, "case": case.name,
        "workload": "bank_parallel" if case.group == "bank" else
                    "single_bank_footprint" if case.group == "geometry" else "refresh_pressure",
        "overrides": ";".join(f"{k}={v}" for k, v in case.overrides.items()),
        "total_banks": int(values["bank_groups"]) * int(values["banks_per_group"]),
        "columns": int(values["columns"]),
        "avg_read_latency_ticks": as_float(stats, "avg_read_latency"),
        "achieved_bw_GBps": as_float(stats, "achieved_bw_GBps"),
        "row_hits": int(hits), "row_misses": int(misses), "row_conflicts": int(conflicts),
        "row_hit_rate_pct": 100.0 * hits / decisions if decisions else 0.0,
        "row_conflict_rate_pct": 100.0 * conflicts / decisions if decisions else 0.0,
        "refresh_pb_batches": int(as_float(stats, "refresh_pb_batches")),
        "refresh_ab_batches": int(as_float(stats, "refresh_ab_batches")),
        "data_mismatches": int(as_float(stats, "data_mismatches")),
        "hit_cycle_limit": stats["hit_cycle_limit"].lower(),
        "cycles": int(as_float(stats, "cycles")),
        "wall_seconds": round(elapsed, 3),
    }


def evaluate(rows: list[dict[str, object]]) -> list[Check]:
    checks: list[Check] = []
    for row in rows:
        label = f"{row['standard']} {row['group']}/{row['case']}"
        valid = (row["cycles"] > 0 and row["hit_cycle_limit"] == "false" and
                 row["data_mismatches"] == 0)
        checks.append(Check("PASS" if valid else "FAIL", f"completion: {label}",
                            f"cycles={row['cycles']}, cycle_limit={row['hit_cycle_limit']}, "
                            f"mismatches={row['data_mismatches']}"))

    for standard in sorted({str(row["standard"]) for row in rows}):
        subset = [row for row in rows if row["standard"] == standard]
        banks = sorted((row for row in subset if row["group"] == "bank"),
                       key=lambda row: int(row["total_banks"]))
        # workload 对同一 bank 永不复用 row，因此端点比较反映可用 bank
        # 并行度，而不是 FR-FCFS 通过 row wrap 人为制造的命中收益。中间组织
        # 可能已触及 command/data bus 瓶颈，不能错误要求每个相邻点严格单调；
        # 门禁只要求最大 bank 组织不劣于最小组织（容许 1% 输出抖动）。
        bank_ok = (float(banks[-1]["achieved_bw_GBps"]) + 1e-9 >=
                   0.99 * float(banks[0]["achieved_bw_GBps"]))
        bank_points = "; ".join(
            f"{row['total_banks']} banks={row['achieved_bw_GBps']:.3f} GBps"
            for row in banks
        )
        checks.append(Check("PASS" if bank_ok else "FAIL", f"bank scaling: {standard}",
                            bank_points))

        geometry = sorted((row for row in subset if row["group"] == "geometry"),
                          key=lambda row: int(row["columns"]))
        geometry_ok = (float(geometry[-1]["row_hit_rate_pct"]) + 1e-9 >=
                       float(geometry[0]["row_hit_rate_pct"]) and
                       float(geometry[-1]["row_conflict_rate_pct"]) <=
                       float(geometry[0]["row_conflict_rate_pct"]) + 1e-9)
        checks.append(Check("PASS" if geometry_ok else "FAIL", f"geometry trend: {standard}",
                            f"hit {geometry[0]['row_hit_rate_pct']:.2f}% -> "
                            f"{geometry[-1]['row_hit_rate_pct']:.2f}%; conflict "
                            f"{geometry[0]['row_conflict_rate_pct']:.2f}% -> "
                            f"{geometry[-1]['row_conflict_rate_pct']:.2f}%"))

        by_case = {str(row["case"]): row for row in subset if row["group"] == "refresh"}
        per_bank = by_case["per_bank"]
        all_bank = by_case["all_bank"]
        refresh_ok = (int(per_bank["refresh_pb_batches"]) > 0 and
                      int(per_bank["refresh_ab_batches"]) == 0 and
                      int(all_bank["refresh_ab_batches"]) > 0 and
                      int(all_bank["refresh_pb_batches"]) == 0)
        checks.append(Check("PASS" if refresh_ok else "FAIL", f"refresh scope: {standard}",
                            f"per-bank(pb={per_bank['refresh_pb_batches']},ab={per_bank['refresh_ab_batches']}); "
                            f"all-bank(pb={all_bank['refresh_pb_batches']},ab={all_bank['refresh_ab_batches']})"))
    return checks


def write_csv(path: Path, rows: list[dict[str, object]]) -> None:
    with path.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)


def write_checks(path: Path, checks: list[Check]) -> None:
    with path.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.writer(stream)
        writer.writerow(["status", "check", "detail"])
        writer.writerows((check.status, check.name, check.detail) for check in checks)


def write_summary(path: Path, rows: list[dict[str, object]], checks: list[Check],
                  args: argparse.Namespace) -> None:
    lines = [
        "# Architecture sweep result", "",
        f"每个定向 workload 生成 {args.requests} 个请求；refresh workload 的 inject_interval={args.inject_interval}。",
        "bank 使用同时到达的跨-bank trace；geometry 使用固定 footprint 的单-bank顺序 trace；",
        "refresh 使用相同研究型间隔下的持续流量。workload.trace 与 resolved.cfg 是审计依据。", "",
        "## Automated checks", "", "| status | check | detail |", "|---|---|---|",
    ]
    lines.extend(f"| {c.status} | {c.name} | {c.detail} |" for c in checks)
    lines.extend([
        "", "## Trend interpretation", "",
        "以下解释只比较同一标准、同一组内的定向 workload。bank 和 geometry 具有由 trace "
        "构造保证的预期方向；refresh 的性能排序还取决于实际维护批次数和阻塞范围，因此只对 "
        "REFpb/REFab scope 做硬性 PASS/FAIL，不预设 per-bank 必然快于 all-bank。", "",
    ])
    for standard in sorted({str(row["standard"]) for row in rows}):
        subset = [row for row in rows if row["standard"] == standard]
        banks = sorted((row for row in subset if row["group"] == "bank"),
                       key=lambda row: int(row["total_banks"]))
        geometry = sorted((row for row in subset if row["group"] == "geometry"),
                          key=lambda row: int(row["columns"]))
        refresh = {str(row["case"]): row for row in subset if row["group"] == "refresh"}
        per_bank = refresh["per_bank"]
        all_bank = refresh["all_bank"]
        lines.extend([
            f"### {standard}", "",
            f"- Bank：{banks[0]['total_banks']}→{banks[-1]['total_banks']} banks 时，带宽 "
            f"{banks[0]['achieved_bw_GBps']:.3f}→{banks[-1]['achieved_bw_GBps']:.3f} GB/s。"
            "自动门禁只比较最大/最小组织；中间点可能因命令/数据总线饱和、映射和调度产生"
            "小幅非单调，必须结合全部三点解释。",
            f"- Geometry：columns {geometry[0]['columns']}→{geometry[-1]['columns']} 时，行命中率 "
            f"{geometry[0]['row_hit_rate_pct']:.2f}%→{geometry[-1]['row_hit_rate_pct']:.2f}%，"
            f"冲突率 {geometry[0]['row_conflict_rate_pct']:.2f}%→"
            f"{geometry[-1]['row_conflict_rate_pct']:.2f}%。固定 footprint 在更宽行中产生更强局部性，"
            "方向与地址几何一致。",
            f"- Refresh：per-bank 触发 {per_bank['refresh_pb_batches']} 个 PB batch，"
            f"延迟/带宽为 {per_bank['avg_read_latency_ticks']:.2f} tick / "
            f"{per_bank['achieved_bw_GBps']:.3f} GB/s；all-bank 触发 "
            f"{all_bank['refresh_ab_batches']} 个 AB batch，延迟/带宽为 "
            f"{all_bank['avg_read_latency_ticks']:.2f} tick / "
            f"{all_bank['achieved_bw_GBps']:.3f} GB/s。该差异是本次维护次数、scope 与调度共同结果，"
            "不能外推为所有负载或器件的固定优劣。", "",
        ])
    lines.extend([
        "", "## Measurements", "",
        "| standard | group | case | workload | overrides | latency/tick | BW/GBps | hit/% | conflict/% | REFpb | REFab | wall/s |",
        "|---|---|---|---|---|---:|---:|---:|---:|---:|---:|---:|",
    ])
    for row in rows:
        lines.append(
            f"| {row['standard']} | {row['group']} | {row['case']} | {row['workload']} | "
            f"`{row['overrides']}` | {row['avg_read_latency_ticks']:.2f} | "
            f"{row['achieved_bw_GBps']:.3f} | {row['row_hit_rate_pct']:.2f} | "
            f"{row['row_conflict_rate_pct']:.2f} | {row['refresh_pb_batches']} | "
            f"{row['refresh_ab_batches']} | {row['wall_seconds']:.3f} |"
        )
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def write_html(path: Path, rows: list[dict[str, object]], checks: list[Check]) -> None:
    serialized_checks = "\n".join(
        f"<tr><td class='{c.status.lower()}'>{c.status}</td><td>{html.escape(c.name)}</td>"
        f"<td>{html.escape(c.detail)}</td></tr>" for c in checks
    )
    serialized_rows = "\n".join(
        f"<tr><td>{html.escape(str(r['standard']))}</td><td>{html.escape(str(r['group']))}</td>"
        f"<td>{html.escape(str(r['case']))}</td><td>{r['avg_read_latency_ticks']:.2f}</td>"
        f"<td>{r['achieved_bw_GBps']:.3f}</td><td>{r['row_hit_rate_pct']:.2f}</td>"
        f"<td>{r['row_conflict_rate_pct']:.2f}</td></tr>" for r in rows
    )
    document = f"""<!doctype html><meta charset=\"utf-8\"><title>Architecture sweep</title>
<style>body{{font:14px system-ui;background:#0b1020;color:#e6edf7;padding:28px}}table{{border-collapse:collapse;width:100%;margin-bottom:28px}}th,td{{border:1px solid #334155;padding:8px;text-align:right}}th:first-child,td:first-child,th:nth-child(2),td:nth-child(2),th:nth-child(3),td:nth-child(3){{text-align:left}}tr:hover{{background:#17213b}}.pass{{color:#4ade80}}.fail{{color:#f87171}}</style>
<h1>HBM/LPDDR architecture sweep</h1><p>同组内比较趋势；结果是模型行为证据，不是器件标定值。</p>
<p>刷新策略只硬性检查 PB/AB scope；其延迟和带宽排序还受维护批次数与 workload 影响，
不预设 per-bank 必然优于 all-bank。详细逐标准解释见同目录 summary.md。</p>
<h2>Automated checks</h2><table><thead><tr><th>status</th><th>check</th><th>detail</th></tr></thead><tbody>{serialized_checks}</tbody></table>
<h2>Measurements</h2><table><thead><tr><th>standard</th><th>group</th><th>case</th><th>latency/tick</th><th>BW/GBps</th><th>row hit/%</th><th>conflict/%</th></tr></thead><tbody>{serialized_rows}</tbody></table>"""
    path.write_text(document, encoding="utf-8")


def main() -> int:
    args = parse_args()
    if not args.binary.is_file():
        raise SystemExit(f"binary not found: {args.binary}")
    if args.requests < 1 or args.inject_interval < 1 or args.case_timeout <= 0:
        raise SystemExit("--requests, --inject-interval and --case-timeout must be positive")
    standards = [item.strip().lower() for item in args.standards.split(",") if item.strip()]
    if not standards or len(standards) != len(set(standards)):
        raise SystemExit("--standards must contain a non-empty, unique list")
    for standard in standards:
        family(standard)
    args.out.mkdir(parents=True, exist_ok=True)
    all_cases = cases()
    total = len(standards) * len(all_cases)
    rows: list[dict[str, object]] = []
    ordinal = 0
    for standard in standards:
        for case in all_cases:
            ordinal += 1
            rows.append(run_case(args, standard, case, ordinal, total))
    checks = evaluate(rows)
    write_csv(args.out / "results.csv", rows)
    write_checks(args.out / "checks.csv", checks)
    write_summary(args.out / "summary.md", rows, checks, args)
    write_html(args.out / "trends.html", rows, checks)
    failures = [check for check in checks if check.status == "FAIL"]
    if failures:
        print(f"architecture sweep failed {len(failures)} check(s): {args.out / 'checks.csv'}")
        return 1
    print(f"architecture sweep complete: {args.out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
