#!/usr/bin/env python3
"""Validate/replay an existing HETTrace directory and audit every response.

Uses only the standard library. Outputs and the resolved model configuration
are retained in a new directory; the input trace is never modified.
"""

import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
from hettrace import addrmap, reader  # noqa: E402


def run(command, output, env):
    with output.open("w") as log:
        subprocess.run(command, stdout=log, stderr=subprocess.STDOUT,
                       check=True, env=env)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trace_dir", type=Path)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--config", type=Path)
    parser.add_argument("--standard", default="hbm4")
    parser.add_argument("--ticks-per-cycle", type=int, default=1000)
    parser.add_argument("--max-cycles", type=int, default=100000000)
    parser.add_argument("--scheduler", choices=("fcfs", "frfcfs"))
    parser.add_argument("--row-policy", choices=("open_page", "closed_page", "closed_cap"))
    parser.add_argument("--allow-uninitialized", action="store_true",
                        help="真实 trace 未包含初始内存镜像时，显式容忍并报告 uninitialized_data")
    args = parser.parse_args()
    if args.ticks_per_cycle <= 0 or args.max_cycles <= 0:
        parser.error("周期参数必须为正数")
    memsim = Path(os.environ.get("MEMSIM_HOME", ROOT.parent / "mem_sim")).resolve()
    build = Path(os.environ.get("MEMSIM_BUILD", memsim / "build"))
    binary = Path(os.environ.get("MEMSIM_BIN", build / "hbm_sim")).resolve()
    config = (args.config or memsim / "configs/hbm.cfg").resolve()
    trace_dir, out = args.trace_dir.resolve(), args.out.resolve()
    if not config.is_file() or not binary.is_file():
        parser.error("未找到 mem_sim config/binary；先 source scripts/native_env.sh 并构建")
    traces = sorted(trace_dir.glob("*.hettrace"))
    if not traces:
        parser.error("输入目录中没有 *.hettrace")
    if out.exists():
        parser.error("输出目录已存在，请使用新的 --out")
    out.mkdir(parents=True)
    env = dict(os.environ)
    env["PYTHONPATH"] = str(ROOT / "tools")
    cli = [sys.executable, "-m", "hettrace"]
    run(cli + ["validate", str(trace_dir)], out / "validate.txt", env)
    run(cli + ["stats", str(trace_dir)], out / "stats.txt", env)

    source_stats = {}
    for trace in traces:
        for record in reader.read_records(str(trace)):
            if record.chan not in (reader.CHAN_W, reader.CHAN_R):
                continue
            name = addrmap.SRC_NAME_BY_ID[record.src_id]
            values = source_stats.setdefault(name, {"requests": 0, "bytes": 0})
            values["requests"] += 1
            values["bytes"] += record.size
    if not source_stats:
        raise ValueError("没有可投影的数据拍")
    manifest = {
        "benchmark": "existing HETTrace replay",
        "warning": "固定请求流的 open-loop 存储实验，不代表应用性能。",
        "projection_payload_semantics": "zero surrogate; no original WDATA/RDATA",
        "source_stats": source_stats,
    }
    (out / "trace_manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    request_trace = out / "mem_sim.trace"
    mapping = out / "mem_sim.map.csv"
    responses = out / "hbm_sim.responses.csv"
    run(cli + ["convert", str(trace_dir), "--preset", "memsim",
               "--ticks-per-cycle", str(args.ticks_per_cycle),
               "--map-output", str(mapping), "-o", str(request_trace)],
        out / "convert.txt", env)
    # Serialize resolved values, including inherited configuration and CLI overrides.
    command = [str(binary), "--config", str(config), "--standard", args.standard,
               "--trace", str(request_trace), "--requests", "0",
               "--max-cycles", str(args.max_cycles), "--progress-interval", "0",
               "--stats-view", "summary", "--response-delivery-mode", "host",
               "--response-trace", str(responses),
               "--dump-resolved-config", str(out / "resolved.cfg")]
    if args.scheduler:
        command += ["--scheduler", args.scheduler]
    if args.row_policy:
        command += ["--row-policy", args.row_policy]
    # Keep the original file too; its path is recorded for auditing includes.
    shutil.copyfile(config, out / "input.cfg")
    metadata = {
        "trace_files": [str(p) for p in traces],
        "config": str(config),
        "binary": str(binary),
        "ticks_per_cycle": args.ticks_per_cycle, "standard": args.standard,
        "max_cycles": args.max_cycles, "command": command,
        "allow_uninitialized": args.allow_uninitialized,
        "upstream_lock": json.loads((ROOT / "upstream.lock.json").read_text()),
    }
    (out / "run.json").write_text(json.dumps(metadata, indent=2) + "\n")
    run(command, out / "hbm_sim.txt", env)
    audit = [sys.executable, str(ROOT / "workloads/llm_memory/compare_results.py"),
             str(out / "trace_manifest.json"), str(mapping), str(responses)]
    if args.allow_uninitialized:
        audit += ["--allow-uninitialized"]
    run(audit, out / "summary.md", env)
    print((out / "summary.md").read_text())
    mode = "请求/时序审计，显式允许未初始化" if args.allow_uninitialized else "请求/时序与零替身状态审计"
    print("PASS (%s): replay outputs %s" % (mode, out))
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print("FAIL: %s；检查 --out 目录中对应阶段日志。" % error, file=sys.stderr)
        sys.exit(1)
