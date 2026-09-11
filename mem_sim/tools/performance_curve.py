#!/usr/bin/env python3
"""Generate reproducible injection-rate/latency/throughput curves."""

from __future__ import annotations

import argparse
import csv
import json
import sys
from pathlib import Path
from result_io import run_simulator

from config_selection import REFERENCE_PRESETS, selection_args


ROOT = Path(__file__).resolve().parents[1]


def comma_ints(value: str) -> list[int]:
    return [int(item.strip()) for item in value.split(",") if item.strip()]


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", type=Path, default=ROOT / "build/hbm_sim")
    parser.add_argument("--standards", default="hbm3,hbm4,lpddr5,lpddr6")
    parser.add_argument("--intervals", default="64,32,16,8,4,2,1")
    parser.add_argument("--read-ratios", default="100,50,0")
    parser.add_argument("--requests", type=int, default=2000)
    parser.add_argument("--pattern", choices=("random", "stream"), default="random")
    parser.add_argument("--seed", type=int, default=20260816)
    parser.add_argument("--csv-out", type=Path)
    parser.add_argument("--json-out", type=Path)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    standards = [item.strip().lower() for item in args.standards.split(",") if item.strip()]
    intervals = comma_ints(args.intervals)
    read_ratios = comma_ints(args.read_ratios)
    if not args.binary.is_file():
        raise SystemExit(f"binary not found: {args.binary}")
    if not standards or len(standards) != len(set(standards)):
        raise SystemExit("standards must contain a non-empty, unique list")
    if any(item not in REFERENCE_PRESETS for item in standards):
        raise SystemExit(f"unsupported standards: {standards}")
    if args.requests < 1:
        raise SystemExit("requests must be positive")
    if not intervals or any(item < 1 for item in intervals):
        raise SystemExit("intervals must be positive controller ticks")
    if not read_ratios or any(item < 0 or item > 100 for item in read_ratios):
        raise SystemExit("read ratios must be in [0, 100]")

    rows = []
    for standard in standards:
        for read_ratio in read_ratios:
            for interval in sorted(set(intervals), reverse=True):
                command = [
                    str(args.binary.resolve()), *selection_args(standard),
                    "--requests", str(args.requests), "--pattern", args.pattern,
                    "--read-ratio", str(read_ratio), "--inject-interval", str(interval),
                    "--seed", str(args.seed), "--max-cycles", "100000000",
                ]
                stats, _ = run_simulator(command, cwd=ROOT)
                if stats.get("hit_cycle_limit", "true").lower() != "false":
                    raise RuntimeError(f"curve run hit cycle limit: {standard}/R{read_ratio}/i{interval}")
                completed_count = int(stats["completed_reads"]) + int(stats["completed_writes"])
                if completed_count != args.requests:
                    raise RuntimeError(
                        f"incomplete curve run: {standard}/R{read_ratio}/i{interval} "
                        f"completed={completed_count} expected={args.requests}")
                rows.append({
                    "standard": standard.upper(), "pattern": args.pattern,
                    "read_ratio_pct": read_ratio, "inject_interval_ticks": interval,
                    "offered_requests_per_tick": 1.0 / interval,
                    "requests": args.requests, "system_cycles": int(stats["system_cycles"]),
                    "completed_reads": int(stats["completed_reads"]),
                    "completed_writes": int(stats["completed_writes"]),
                    "avg_read_latency_ticks": float(stats["avg_read_latency"]),
                    "achieved_bw_GBps": float(stats["achieved_bw_GBps"]),
                    "peak_bandwidth_GBps": float(stats["peak_bandwidth_GBps"]),
                    "bandwidth_util_pct": float(stats["bandwidth_util_pct"]),
                    "row_hit_pct": float(stats["row_hit_pct"]),
                })

    checks = []
    for standard in standards:
        for read_ratio in read_ratios:
            curve = [row for row in rows if row["standard"] == standard.upper()
                     and row["read_ratio_pct"] == read_ratio]
            curve.sort(key=lambda row: row["offered_requests_per_tick"])
            low, high = curve[0], curve[-1]
            prefix = f"{standard}_r{read_ratio}"
            checks.append({
                "name": prefix + "_throughput_load_response",
                "passed": high["achieved_bw_GBps"] >= low["achieved_bw_GBps"],
                "detail": f"low={low['achieved_bw_GBps']} high={high['achieved_bw_GBps']} GB/s",
            })
            checks.append({
                "name": prefix + "_physical_peak_bound",
                "passed": all(row["achieved_bw_GBps"] <= row["peak_bandwidth_GBps"] * 1.001
                              for row in curve),
                "detail": f"max_util={max(row['bandwidth_util_pct'] for row in curve):.2f}%",
            })
            if read_ratio > 0:
                checks.append({
                    "name": prefix + "_queueing_latency_response",
                    "passed": high["avg_read_latency_ticks"] >= low["avg_read_latency_ticks"],
                    "detail": (f"low={low['avg_read_latency_ticks']} "
                               f"high={high['avg_read_latency_ticks']} ticks"),
                })

    report = {
        "schema_version": 2,
        "scope": "synthetic_injection_latency_throughput_curve",
        "workload": {"pattern": args.pattern, "requests": args.requests,
                     "seed": args.seed, "intervals": sorted(set(intervals), reverse=True),
                     "read_ratios": read_ratios},
        "rows": rows, "checks": checks,
        "passed": all(check["passed"] for check in checks),
        "claim_boundary": (
            "These deterministic synthetic curves validate response shape and internal peak "
            "bounds; they are not vendor or hardware performance calibration."),
    }
    if args.csv_out:
        args.csv_out.parent.mkdir(parents=True, exist_ok=True)
        with args.csv_out.open("w", newline="", encoding="utf-8") as stream:
            writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
            writer.writeheader()
            writer.writerows(rows)
    if args.json_out:
        args.json_out.parent.mkdir(parents=True, exist_ok=True)
        args.json_out.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n",
                                 encoding="utf-8")
    for check in checks:
        print(f"{'PASS' if check['passed'] else 'FAIL':4} {check['name']}: {check['detail']}")
    failed = sum(not check["passed"] for check in checks)
    print(f"\nperformance curves: {len(checks)-failed}/{len(checks)} checks passed; rows={len(rows)}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
