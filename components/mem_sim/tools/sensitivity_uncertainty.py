#!/usr/bin/env python3
"""One-at-a-time sensitivity and deterministic timing-uncertainty intervals."""

from __future__ import annotations

import argparse
import csv
import json
import math
import random
import subprocess
import sys
import tempfile
from pathlib import Path
from result_io import run_simulator

from config_selection import REFERENCE_PRESETS, selection_args


ROOT = Path(__file__).resolve().parents[1]
PARAMETERS = ("nCL", "nRCDRD", "nRP", "nCCDS", "nRRDS", "nFAW")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", type=Path, default=ROOT / "build/hbm_sim")
    parser.add_argument("--standards", default="hbm3,hbm4,lpddr5,lpddr6")
    parser.add_argument("--requests", type=int, default=1000)
    parser.add_argument("--samples", type=int, default=24)
    parser.add_argument("--fraction", type=float, default=0.10)
    parser.add_argument("--seed", type=int, default=20260816)
    parser.add_argument("--csv-out", type=Path)
    parser.add_argument("--json-out", type=Path)
    return parser.parse_args()


def timing_table(binary: Path, standard: str, temp: Path) -> dict[str, int]:
    output = temp / (standard + "_timing.csv")
    completed = subprocess.run(
        [str(binary), *selection_args(standard), "--requests", "0",
         "--dump-timing-table", str(output)], cwd=ROOT, text=True,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)
    if completed.returncode:
        raise RuntimeError(completed.stdout + completed.stderr)
    with output.open(newline="", encoding="utf-8") as stream:
        return {row["name"]: int(row["value_nck"]) for row in csv.DictReader(stream)}


def derived_config(overrides: dict[str, int], path: Path,
                   nominal: dict[str, int]) -> dict[str, int]:
    # 只生成小型 override，不复制权威 master，避免临时实验形成第三份参数源。
    overrides = dict(overrides)
    # Reference presets explicitly pin nRC. Preserve their extra row-cycle margin
    # when varying nRP/nRAS, rather than keeping an impossible inherited nRC.
    if "nRP" in overrides or "nRAS" in overrides:
        margin = nominal["nRC"] - nominal["nRAS"] - nominal["nRP"]
        if margin < 0:
            raise ValueError("nominal nRC must be >= nRAS + nRP")
        overrides["nRC"] = (overrides.get("nRAS", nominal["nRAS"]) +
                            overrides.get("nRP", nominal["nRP"]) + margin)
    lines = ["[override]", "timing_override_source = research_default"]
    lines += [f"{key} = {value}" for key, value in overrides.items()]
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")
    return overrides


def simulate(binary: Path, standard: str, requests: int, seed: int,
             overlay: Path | None = None,
             extra: list[str] | None = None) -> dict[str, float]:
    command = [str(binary), *selection_args(standard)]
    if overlay is not None:
        command += ["--config", str(overlay)]
    command += ["--requests", str(requests),
               "--pattern", "random", "--read-ratio", "100", "--inject-interval", "2",
               "--seed", str(seed), "--max-cycles", "100000000"]
    command += extra or []
    stats, _ = run_simulator(command, cwd=ROOT)
    if stats.get("hit_cycle_limit", "true").lower() != "false":
        raise RuntimeError("sensitivity run hit cycle limit")
    result = {
        "latency_ticks": float(stats["avg_read_latency"]),
        "throughput_GBps": float(stats["achieved_bw_GBps"]),
    }
    # Timing-only reference cases disable these models. Their old diagnostic
    # zeroes were not measurements and must not appear as sensitivity results.
    if stats["power_model_enabled"] == "true":
        result["energy_pJ"] = float(stats["power_energy_pJ"])
    if stats["thermal_model_enabled"] == "true":
        result["peak_temperature_C"] = float(stats["thermal_peak_temp_C"])
    return result


def quantile(values: list[float], probability: float) -> float:
    ordered = sorted(values)
    position = (len(ordered) - 1) * probability
    lower = math.floor(position)
    upper = math.ceil(position)
    if lower == upper:
        return ordered[lower]
    weight = position - lower
    return ordered[lower] * (1.0 - weight) + ordered[upper] * weight


def main() -> int:
    args = parse_args()
    binary = args.binary.resolve()
    standards = [item.strip().lower() for item in args.standards.split(",") if item.strip()]
    if not binary.is_file():
        raise SystemExit(f"binary not found: {binary}")
    if not standards or len(standards) != len(set(standards)):
        raise SystemExit("standards must contain a non-empty, unique list")
    if any(item not in REFERENCE_PRESETS for item in standards):
        raise SystemExit(f"unsupported standards: {standards}")
    if args.requests < 1:
        raise SystemExit("requests must be positive")
    if not 0.0 < args.fraction < 1.0 or args.samples < 3:
        raise SystemExit("fraction must be in (0,1) and samples must be >= 3")

    sensitivity, uncertainty, checks = [], [], []
    rng = random.Random(args.seed)
    with tempfile.TemporaryDirectory(prefix="hbm_sensitivity_") as temp_name:
        temp = Path(temp_name)
        for standard in standards:
            nominal = timing_table(binary, standard, temp)
            base_stats = simulate(binary, standard, args.requests, args.seed)
            for parameter in PARAMETERS:
                center = nominal[parameter]
                values = {
                    "low": max(1, round(center * (1.0 - args.fraction))),
                    "nominal": center,
                    "high": max(1, round(center * (1.0 + args.fraction))),
                }
                case_results = {}
                for level, value in values.items():
                    config = temp / f"{standard}_{parameter}_{level}.cfg"
                    effective = derived_config({parameter: value}, config, nominal)
                    case_results[level] = simulate(
                        binary, standard, args.requests, args.seed, overlay=config)
                    sensitivity.append({
                        "standard": standard.upper(), "parameter": parameter,
                        "level": level, "value_nck": value,
                        "effective_nRC_nck": effective.get("nRC", nominal["nRC"]),
                        **case_results[level],
                    })
                low, high = case_results["low"], case_results["high"]
                sensitivity[-2]["latency_ticks"] = base_stats["latency_ticks"]
                sensitivity[-2]["throughput_GBps"] = base_stats["throughput_GBps"]
                checks.append({
                    "name": f"{standard}_{parameter}_finite_response",
                    "passed": all(math.isfinite(result[metric]) and result[metric] >= 0
                                  for result in case_results.values()
                                  for metric in ("latency_ticks", "throughput_GBps")),
                    "detail": (f"latency_low/nom/high={low['latency_ticks']}/"
                               f"{base_stats['latency_ticks']}/{high['latency_ticks']}"),
                })
                if parameter == "nCL":
                    checks.append({
                        "name": f"{standard}_nCL_latency_monotonic",
                        "passed": low["latency_ticks"] <= base_stats["latency_ticks"]
                                  <= high["latency_ticks"],
                        "detail": (f"low={low['latency_ticks']} nominal="
                                   f"{base_stats['latency_ticks']} high={high['latency_ticks']}"),
                    })

            samples = []
            for sample_index in range(args.samples):
                overrides = {}
                for parameter in PARAMETERS:
                    factor = 1.0 + rng.uniform(-args.fraction, args.fraction)
                    overrides[parameter] = max(1, round(nominal[parameter] * factor))
                config = temp / f"{standard}_uncertainty_{sample_index}.cfg"
                derived_config(overrides, config, nominal)
                samples.append(simulate(
                    binary, standard, args.requests, args.seed, overlay=config))
            entry = {"standard": standard.upper(), "samples": args.samples,
                     "fraction": args.fraction}
            for metric in ("latency_ticks", "throughput_GBps"):
                values = [sample[metric] for sample in samples]
                entry[f"{metric}_p05"] = quantile(values, 0.05)
                entry[f"{metric}_p50"] = quantile(values, 0.50)
                entry[f"{metric}_p95"] = quantile(values, 0.95)
                entry[f"{metric}_min"] = min(values)
                entry[f"{metric}_max"] = max(values)
            uncertainty.append(entry)
            checks.append({
                "name": f"{standard}_uncertainty_interval_ordered",
                "passed": (entry["latency_ticks_p05"] <= entry["latency_ticks_p50"]
                           <= entry["latency_ticks_p95"] and
                           entry["throughput_GBps_p05"] <= entry["throughput_GBps_p50"]
                           <= entry["throughput_GBps_p95"]),
                "detail": (f"latency_p05/p50/p95={entry['latency_ticks_p05']:.3f}/"
                           f"{entry['latency_ticks_p50']:.3f}/"
                           f"{entry['latency_ticks_p95']:.3f}"),
            })

        # Formula-level power/thermal knobs are checked separately from timing uncertainty.
        power_results = {}
        for label, scale in (("low", 0.9), ("nominal", 1.0), ("high", 1.1)):
            power_results[label] = simulate(
                binary, "hbm4", args.requests, args.seed,
                extra=["--floorplan", "true", "--power-model", "true",
                       "--thermal-model", "true", "--power-scale", str(scale)])
        checks.append({
            "name": "power_scale_energy_monotonic",
            "passed": (power_results["low"]["energy_pJ"] <
                       power_results["nominal"]["energy_pJ"] <
                       power_results["high"]["energy_pJ"]),
            "detail": str({key: value["energy_pJ"] for key, value in power_results.items()}),
        })
        checks.append({
            "name": "power_scale_temperature_monotonic",
            "passed": (power_results["low"]["peak_temperature_C"] <=
                       power_results["nominal"]["peak_temperature_C"] <=
                       power_results["high"]["peak_temperature_C"]),
            "detail": str({key: value["peak_temperature_C"]
                           for key, value in power_results.items()}),
        })

    report = {
        "schema_version": 2,
        "scope": "oat_timing_sensitivity_and_bounded_input_uncertainty",
        "parameters": list(PARAMETERS), "fraction": args.fraction,
        "row_cycle_policy": "nRC = nRAS + nRP + nominal row-cycle margin; nRC is not sampled independently",
        "requests": args.requests, "seed": args.seed,
        "sensitivity": sensitivity, "uncertainty_intervals": uncertainty,
        "power_thermal_sensitivity": power_results, "checks": checks,
        "passed": all(check["passed"] for check in checks),
        "claim_boundary": (
            "The p05-p95 ranges propagate an explicit independent uniform ±fraction input "
            "assumption. They quantify model-input uncertainty, not silicon population or "
            "measurement confidence intervals."),
    }
    if args.csv_out:
        args.csv_out.parent.mkdir(parents=True, exist_ok=True)
        with args.csv_out.open("w", newline="", encoding="utf-8") as stream:
            writer = csv.DictWriter(stream, fieldnames=list(sensitivity[0]))
            writer.writeheader()
            writer.writerows(sensitivity)
    if args.json_out:
        args.json_out.parent.mkdir(parents=True, exist_ok=True)
        args.json_out.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n",
                                 encoding="utf-8")
    for check in checks:
        print(f"{'PASS' if check['passed'] else 'FAIL':4} {check['name']}: {check['detail']}")
    failed = sum(not check["passed"] for check in checks)
    print(f"\nsensitivity/uncertainty: {len(checks)-failed}/{len(checks)} checks passed; "
          f"oat_rows={len(sensitivity)}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
