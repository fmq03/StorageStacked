#!/usr/bin/env python3
"""Validate the synthetic LLM zero-surrogate mapping/HostResponse pair."""

from __future__ import annotations

import argparse
import csv
import json
import math
import statistics
import sys
from collections import Counter, defaultdict
from pathlib import Path


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("manifest", type=Path, help="benchmark.json")
    parser.add_argument("mapping", type=Path, help="convert --preset memsim map CSV")
    parser.add_argument("responses", type=Path, help="hbm_sim HostResponse CSV")
    parser.add_argument("--allow-uninitialized", action="store_true",
                        help="仅用于无初始内存镜像的真实 trace；记录并容忍 uninitialized_data")
    return parser.parse_args()


def load_json(path: Path) -> dict:
    with path.open(encoding="utf-8") as handle:
        return json.load(handle)


def load_csv(path: Path, required: set[str]) -> list[dict[str, str]]:
    with path.open(newline="", encoding="utf-8") as handle:
        reader = csv.DictReader(handle)
        missing = required - set(reader.fieldnames or ())
        if missing:
            raise ValueError(
                "%s 缺少列: %s" % (path, ", ".join(sorted(missing)))
            )
        return list(reader)


def index_unique(
    rows: list[dict[str, str]], path: Path
) -> dict[int, dict[str, str]]:
    indexed: dict[int, dict[str, str]] = {}
    for line, row in enumerate(rows, 2):
        try:
            request_id = int(row["host_request_id"])
        except ValueError as error:
            raise ValueError(
                "%s:%d host_request_id 不是整数" % (path, line)
            ) from error
        if request_id in indexed:
            raise ValueError("%s: 重复 host_request_id=%d" % (path, request_id))
        indexed[request_id] = row
    return indexed


def percentile(values: list[int], fraction: float) -> int:
    ordered = sorted(values)
    if not ordered:
        return 0
    rank = max(0, math.ceil(fraction * len(ordered)) - 1)
    return ordered[rank]


def latency_row(name: str, values: list[int]) -> str:
    return (
        "| %s | %d | %d | %.1f | %d | %d | %d |"
        % (
            name,
            len(values),
            min(values),
            statistics.fmean(values),
            percentile(values, 0.50),
            percentile(values, 0.95),
            max(values),
        )
    )


def main() -> int:
    args = parse_args()
    try:
        manifest = load_json(args.manifest)
        mapping_rows = load_csv(
            args.mapping,
            {
                "host_request_id",
                "trace_line",
                "cycle",
                "src_name",
                "chan",
                "addr",
                "size",
                "projection",
            },
        )
        response_rows = load_csv(
            args.responses,
            {
                "host_request_id",
                "type",
                "system_address",
                "arrival_cycle",
                "completion_cycle",
                "latency_cycles",
                "status",
            },
        )
        mapping = index_unique(mapping_rows, args.mapping)
        responses = index_unique(response_rows, args.responses)
    except (OSError, ValueError, json.JSONDecodeError) as error:
        print("错误: %s" % error, file=sys.stderr)
        return 2

    expected_ids = set(range(len(mapping)))
    if set(mapping) != expected_ids:
        missing = sorted(expected_ids - set(mapping))[:8]
        print(
            "错误: mapping 的 host_request_id 必须从 0 连续编号; 缺失 %s"
            % missing,
            file=sys.stderr,
        )
        return 1
    if set(responses) != set(mapping):
        missing = sorted(set(mapping) - set(responses))[:8]
        extra = sorted(set(responses) - set(mapping))[:8]
        print(
            "错误: HostResponse 与 mapping 不守恒; missing=%s extra=%s"
            % (missing, extra),
            file=sys.stderr,
        )
        return 1

    failures: list[str] = []
    by_source: dict[str, list[int]] = defaultdict(list)
    by_source_bytes: dict[str, int] = defaultdict(int)
    by_type: dict[str, list[int]] = defaultdict(list)
    injection_delays: list[int] = []
    for request_id in sorted(mapping):
        mapped = mapping[request_id]
        response = responses[request_id]
        try:
            mapped_cycle = int(mapped["cycle"])
            arrival = int(response["arrival_cycle"])
            completion = int(response["completion_cycle"])
            latency = int(response["latency_cycles"])
            mapped_addr = int(mapped["addr"], 0)
            response_addr = int(response["system_address"], 0)
            mapped_size = int(mapped["size"])
        except ValueError:
            failures.append(
                "id=%d 含非整数 cycle/address/size/latency" % request_id
            )
            continue
        if mapped_size <= 0:
            failures.append("id=%d mapping size=%d 非正" % (request_id, mapped_size))

        expected_type = "Write" if mapped["chan"] in {"AW", "W"} else "Read"
        allowed_statuses = {"ok", "uninitialized_data"} if args.allow_uninitialized else {"ok"}
        if response["status"] not in allowed_statuses:
            failures.append("id=%d status=%s" % (request_id, response["status"]))
        if arrival < mapped_cycle:
            failures.append(
                "id=%d arrival=%d 早于 mapping cycle=%d"
                % (request_id, arrival, mapped_cycle)
            )
        if completion - arrival != latency:
            failures.append("id=%d latency 字段与完成周期不一致" % request_id)
        if completion < arrival or latency < 0:
            failures.append("id=%d completion/latency 违反因果顺序" % request_id)
        if mapped_addr != response_addr:
            failures.append("id=%d 地址不一致" % request_id)
        if response["type"] != expected_type:
            failures.append(
                "id=%d 类型=%s, 预期=%s"
                % (request_id, response["type"], expected_type)
            )
        by_source[mapped["src_name"]].append(latency)
        by_source_bytes[mapped["src_name"]] += mapped_size
        by_type[response["type"]].append(latency)
        injection_delays.append(arrival - mapped_cycle)

    source_stats = manifest.get("source_stats", {})
    manifest_total = sum(
        int(values.get("requests", 0)) for values in source_stats.values()
    )
    if manifest_total and manifest_total != len(mapping):
        failures.append(
            "benchmark manifest requests=%d, mem_sim mapping=%d"
            % (manifest_total, len(mapping))
        )
    for source, values in source_stats.items():
        expected = int(values.get("requests", 0))
        actual = len(by_source.get(source, ()))
        if expected != actual:
            failures.append(
                "%s manifest requests=%d, mapping=%d"
                % (source, expected, actual)
            )
        expected_bytes = int(values.get("bytes", 0))
        actual_bytes = by_source_bytes.get(source, 0)
        if expected_bytes != actual_bytes:
            failures.append(
                "%s manifest bytes=%d, mapping=%d"
                % (source, expected_bytes, actual_bytes)
            )

    if failures:
        print("错误: mem_sim 结果交叉校验失败:", file=sys.stderr)
        for failure in failures[:20]:
            print("  - %s" % failure, file=sys.stderr)
        if len(failures) > 20:
            print("  - 另有 %d 项" % (len(failures) - 20), file=sys.stderr)
        return 1

    all_latencies = [
        int(responses[request_id]["latency_cycles"])
        for request_id in sorted(responses)
    ]
    print("# 外部 mem_sim/HBM 时序摘要")
    print()
    print(
        "`%s`：%s"
        % (manifest.get("benchmark", "unknown"), manifest.get("warning", ""))
    )
    print()
    print("请求守恒：manifest / mapping / HostResponse = **%d / %d / %d**。" % (
        manifest_total or len(mapping), len(mapping), len(responses)
    ))
    manifest_bytes = sum(
        int(values.get("bytes", 0)) for values in source_stats.values()
    )
    print(
        "字节守恒：manifest / mapping = **%d / %d B**。"
        % (manifest_bytes, sum(by_source_bytes.values()))
    )
    print(
        "\n载荷口径：`%s`；status/data mismatch 只校验零值替身，"
        "不校验原始 AXI 数据。"
        % manifest.get("projection_payload_semantics", "unspecified")
    )
    statuses = Counter(row["status"] for row in response_rows)
    print("状态计数：%s。" % ", ".join("%s=%d" % item for item in sorted(statuses.items())))
    if args.allow_uninitialized:
        print("本次显式允许 uninitialized_data：只通过请求/时序审计，"
              "不代表离线初始数据已完整提供；其他错误状态仍会失败。")
    print()
    print("| 流量 | 请求数 | min | mean | p50 | p95 | max |")
    print("|---|---:|---:|---:|---:|---:|---:|")
    print(latency_row("全部", all_latencies))
    for source in sorted(by_source):
        print(latency_row(source, by_source[source]))
    for request_type in ("Read", "Write"):
        if request_type in by_type:
            print(latency_row(request_type, by_type[request_type]))
    print()
    print(
        "hbm_sim 前端按序提交等待：mean %.1f cycle，max %d cycle。"
        % (statistics.fmean(injection_delays), max(injection_delays))
    )
    print(
        "从请求计划周期到完成的平均延迟：%.1f cycle（提交等待 + response latency）。"
        % statistics.fmean(
            int(responses[rid]["completion_cycle"]) - int(mapping[rid]["cycle"])
            for rid in sorted(mapping)
        )
    )
    if all("forwarded" in row for row in response_rows):
        forwarded = [row for row in response_rows if row["forwarded"].lower() == "true"]
        print("写缓冲转发响应：%d / %d；低延迟不能直接解释为 DRAM 阵列访问更快。"
              % (len(forwarded), len(response_rows)))
    print()
    print(
        "延迟单位是 hbm_sim cycle。该结果是固定到达流的 open-loop 存储时序："
        "HETTrace 不含 WDATA，hbm_sim completion 也不会反向推迟 XPU 后继请求，"
        "因此不能据此声称功能数据正确、IPC、tokens/s 或闭环应用加速比。"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
