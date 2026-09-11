#!/usr/bin/env python3
"""Validate hbm_sim's project-native analytical, storage, DFI, and provenance rules."""

from __future__ import annotations

import argparse
import csv
import json
import math
import subprocess
import sys
import tempfile
from dataclasses import dataclass, asdict
from pathlib import Path
from result_io import parse_stats, run_simulator


ROOT = Path(__file__).resolve().parents[1]


@dataclass
class Check:
    name: str
    passed: bool
    detail: str


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", type=Path, default=ROOT / "build/hbm_sim")
    parser.add_argument(
        "--config",
        type=Path,
        default=ROOT / "configs/validation/hbm4.cfg",
    )
    parser.add_argument("--standard", default="hbm4")
    parser.add_argument("--preset", default="validation_native_1ch")
    parser.add_argument(
        "--project-config",
        type=Path,
        default=ROOT / "configs/hbm.cfg",
    )
    parser.add_argument(
        "--manifest",
        type=Path,
        default=ROOT / "configs/validation/vendor_parameters.csv",
    )
    parser.add_argument(
        "--identity-manifest",
        type=Path,
        default=ROOT / "configs/validation/project_identity.csv",
    )
    parser.add_argument("--json-out", type=Path)
    parser.add_argument("--bandwidth-min-pct", type=float)
    parser.add_argument("--bandwidth-baseline-pct", type=float, default=64.5)
    parser.add_argument("--bandwidth-regression-margin-pct", type=float, default=4.5)
    return parser.parse_args()



def run(binary: Path, config_args: list[str], extra: list[str]) -> tuple[dict[str, str], str]:
    return run_simulator([str(binary), *config_args, *extra], cwd=ROOT, diagnostic=True)


def run_cli(binary: Path, extra: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [str(binary), *extra, "--stats-view", "diagnostic"],
        cwd=ROOT,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )


def read_csv(path: Path) -> list[dict[str, str]]:
    with path.open(newline="", encoding="utf-8") as stream:
        return list(csv.DictReader(stream))


def add(checks: list[Check], name: str, condition: bool, detail: str) -> None:
    checks.append(Check(name=name, passed=bool(condition), detail=detail))


def close(actual: float, expected: float, rel_tol: float = 1e-9) -> bool:
    return math.isclose(actual, expected, rel_tol=rel_tol, abs_tol=1e-9)


def write_trace(path: Path, text: str) -> None:
    path.write_text(text, encoding="ascii")


def timing_values(rows: list[dict[str, str]]) -> dict[str, int]:
    return {row["name"]: int(row["value_nck"]) for row in rows}


def validate_manifest(path: Path, checks: list[Check]) -> None:
    rows = read_csv(path)
    required = {
        "area",
        "parameters",
        "current_source",
        "validation_status",
        "vendor_data_needed",
        "claim_boundary",
    }
    actual = set(rows[0]) if rows else set()
    complete = len(rows) >= 10 and required.issubset(actual)
    complete = complete and all(all(row.get(key, "").strip() for key in required) for row in rows)
    add(checks, "vendor_manifest_complete", complete, f"rows={len(rows)}")
    vendor_rows = sum(row.get("vendor_data_needed", "").lower() == "yes" for row in rows)
    add(
        checks,
        "vendor_manifest_marks_unknowns",
        vendor_rows >= 7,
        f"vendor_required_areas={vendor_rows}",
    )


def validate_project_identity(path: Path, checks: list[Check]) -> None:
    rows = read_csv(path)
    required = {
        "capability",
        "project_owner",
        "project_semantics",
        "validation",
        "reference_relation",
        "must_remain_project_specific",
    }
    actual = set(rows[0]) if rows else set()
    complete = len(rows) >= 8 and required.issubset(actual)
    complete = complete and all(all(row.get(key, "").strip() for key in required) for row in rows)
    add(checks, "project_identity_manifest_complete", complete, f"rows={len(rows)}")
    project_specific = sum(
        row.get("project_owner") == "hbm_sim"
        and row.get("must_remain_project_specific", "").lower() == "yes"
        for row in rows
    )
    add(
        checks,
        "project_specific_capabilities_explicit",
        project_specific == len(rows) and project_specific >= 8,
        f"project_specific_capabilities={project_specific}",
    )
    passed_names = {check.name for check in checks if check.passed}
    missing_evidence: dict[str, list[str]] = {}
    for row in rows:
        evidence = [
            item.strip()
            for item in row.get("validation", "").split(";")
            if item.strip()
        ]
        missing = [item for item in evidence if item not in passed_names]
        if not evidence or missing:
            missing_evidence[row.get("capability", "<unknown>")] = missing or [
                "<empty>"
            ]
    add(
        checks,
        "project_identity_evidence_bound",
        not missing_evidence,
        f"missing_evidence={missing_evidence}",
    )


def make_sensitivity_config(output: Path, ncl: int) -> None:
    text = (
        "[override]\n# Generated by tools/model_validation.py for sensitivity validation.\n"
        "timing_override_source = research_default\n"
        f"nCL = {ncl}\n"
    )
    output.write_text(text, encoding="utf-8")


def main() -> int:
    args = parse_args()
    binary = args.binary.resolve()
    config = args.config.resolve()
    project_config = args.project_config.resolve()
    manifest = args.manifest.resolve()
    identity_manifest = args.identity_manifest.resolve()
    if not binary.is_file():
        raise SystemExit(f"hbm_sim binary not found: {binary}")
    validation_selection = ["--config", str(config), "--standard", args.standard,
                            "--preset", args.preset]
    project_selection = ["--config", str(project_config), "--standard", "hbm4"]

    checks: list[Check] = []
    metrics: dict[str, object] = {}
    with tempfile.TemporaryDirectory(prefix="hbm_sim_validation_") as temp_name:
        temp = Path(temp_name)
        timing_csv = temp / "timing.csv"
        base_stats, _ = run(
            binary,
            validation_selection,
            ["--requests", "0", "--dump-timing-table", str(timing_csv)],
        )
        timing_rows = read_csv(timing_csv)
        timings = timing_values(timing_rows)

        add(
            checks,
            "project_validation_config_identity",
            base_stats["timing_profile"]
            == "hbm4_project_native_8000_32gb_8hi"
            and base_stats["mode_profile"] == "project_native_validation"
            and int(base_stats["channels"]) == 1,
            f"timing_profile={base_stats['timing_profile']} "
            f"mode_profile={base_stats['mode_profile']} "
            f"channels={base_stats['channels']}",
        )

        data_rate = float(base_stats["data_rate_mbps"])
        bus_bits = float(base_stats["data_bus_bits"])
        expected_peak = data_rate * bus_bits / 8000.0
        actual_peak = float(base_stats["peak_bandwidth_GBps"])
        add(
            checks,
            "peak_bandwidth_formula",
            close(actual_peak, expected_peak),
            f"actual={actual_peak:.6f} expected={expected_peak:.6f} GB/s",
        )
        expected_rate = data_rate / 1000.0
        actual_rate = float(base_stats["if_xfer_rate_Gbps"])
        add(
            checks,
            "interface_rate_formula",
            close(actual_rate, expected_rate),
            f"actual={actual_rate:.6f} expected={expected_rate:.6f} Gbps",
        )

        allowed_sources = {"jedec", "vendor", "derived", "research_default"}
        bad_sources = sorted(
            {
                row["source"]
                for row in timing_rows
                if row["source"].lower() not in allowed_sources
            }
        )
        add(
            checks,
            "timing_source_audit",
            not bad_sources and len(timing_rows) >= 20,
            f"entries={len(timing_rows)} bad_sources={bad_sources}",
        )
        research_entries = sum(
            row["source"].lower() == "research_default" for row in timing_rows
        )
        add(
            checks,
            "research_defaults_are_visible",
            research_entries > 0,
            f"research_default_entries={research_entries}",
        )

        one_read = temp / "one_read.trace"
        command_csv = temp / "one_read_commands.csv"
        dfi_csv = temp / "one_read_dfi.csv"
        # A 32B explicit expectation keeps this timing probe to one physical
        # HBM transaction. The separate C++ test covers 64B host-line splitting.
        write_trace(one_read, f"0 R 0x0 expect={'00' * 32}\n")
        read_stats, _ = run(
            binary,
            validation_selection,
            [
                "--trace",
                str(one_read),
                "--cmd-trace",
                str(command_csv),
                "--dfi-trace",
                str(dfi_csv),
                "--validate-cmd-trace",
                "--validate-dfi-trace",
            ],
        )
        commands = read_csv(command_csv)
        act = next(row for row in commands if row["command"] == "ACT")
        rd = next(row for row in commands if row["command"] == "RD")
        tick_multiplier = int(base_stats["tick_multiplier"])
        act_rd_gap = int(rd["cycle"]) - int(act["cycle"])
        expected_gap = timings["nRCDRD"] * tick_multiplier
        add(
            checks,
            "act_to_read_timing",
            act_rd_gap == expected_gap,
            f"actual={act_rd_gap} expected={expected_gap} ticks",
        )

        expected_completion = int(rd["cycle"]) + (
            timings["nCL"] + timings["nBL"]
        ) * tick_multiplier
        actual_latency = float(read_stats["avg_read_latency"])
        add(
            checks,
            "closed_row_read_completion",
            close(actual_latency, float(expected_completion)),
            f"actual={actual_latency:.2f} expected={expected_completion} ticks",
        )
        dfi_rows = read_csv(dfi_csv)
        first_read_beat = next(row for row in dfi_rows if row["kind"] == "READ_DATA")
        expected_dfi_cycle = int(rd["cycle"]) + int(
            base_stats["dfi_read_latency_nck"]
        ) * tick_multiplier
        add(
            checks,
            "dfi_read_latency",
            int(first_read_beat["cycle"]) == expected_dfi_cycle,
            f"actual={first_read_beat['cycle']} expected={expected_dfi_cycle}",
        )
        add(
            checks,
            "online_validators",
            read_stats["cmd_validation"] == "pass"
            and read_stats["dfi_validation"] == "pass",
            "command=pass dfi=pass",
        )

        payload_trace = temp / "payload.trace"
        payload_dfi = temp / "payload_dfi_signal.csv"
        payload = "00112233445566778899aabbccddeeff" * 2
        write_trace(
            payload_trace,
            f"0 W 0x1000 data={payload}\n"
            f"400 R 0x1000 expect={payload}\n",
        )
        payload_stats, _ = run(
            binary,
            validation_selection,
            [
                "--trace",
                str(payload_trace),
                "--dfi-signal-trace",
                str(payload_dfi),
                "--validate-dfi-trace",
            ],
        )
        payload_rows = read_csv(payload_dfi)
        write_data = "".join(
            row["dfi_wrdata"]
            for row in sorted(
                (row for row in payload_rows if row["kind"] == "WRITE_DATA"),
                key=lambda row: (int(row["cycle"]), int(row["phase"])),
            )
        )
        read_data = "".join(
            row["dfi_rddata"]
            for row in sorted(
                (row for row in payload_rows if row["kind"] == "READ_DATA"),
                key=lambda row: (int(row["cycle"]), int(row["phase"])),
            )
        )
        add(
            checks,
            "dfi_real_payload_roundtrip",
            write_data == payload
            and read_data == payload
            and int(payload_stats["data_mismatches"]) == 0,
            f"write_bytes={len(write_data) // 2} read_bytes={len(read_data) // 2}",
        )
        add(
            checks,
            "project_storage_payload_semantics",
            int(payload_stats["data_write_commits"]) == 1
            and int(payload_stats["data_checked_reads"]) == 1
            and int(payload_stats["data_mismatches"]) == 0
            and int(payload_stats["storage_lines_allocated"]) >= 1
            and payload_stats["memory_backend"] == "sparse",
            "commit=1 checked_read=1 mismatches=0 backend=sparse",
        )
        masks_ok = all(
            row["dfi_wrdata_mask"] == "0" * len(row["dfi_wrdata"])
            for row in payload_rows
            if row["kind"] == "WRITE_DATA"
        )
        sources = {row["payload_source"] for row in payload_rows}
        add(
            checks,
            "dfi_mask_and_source",
            masks_ok and {"request_payload", "memory_image"}.issubset(sources),
            f"sources={sorted(sources)} masks_ok={masks_ok}",
        )
        add(
            checks,
            "dfi_expected_payload_guard",
            int(payload_stats["dfi_validation_expected_checks"]) > 0,
            "expected read payload was checked independently",
        )
        add(
            checks,
            "physical_stack_coordinates",
            int(payload_stats["storage_layers_touched"]) >= 1
            and int(payload_stats["storage_banks_touched"]) >= 1
            and int(payload_stats["storage_subarrays_touched"]) >= 1
            and int(payload_stats["storage_cells_touched"]) >= 1,
            "layer/bank/subarray/cell coordinates observed",
        )

        power_stats, _ = run(
            binary,
            validation_selection,
            [
                "--trace",
                str(payload_trace),
                "--power-model",
                "true",
                "--thermal-model",
                "true",
            ],
        )
        add(
            checks,
            "power_thermal_event_path",
            int(power_stats["power_events"]) > 0
            and int(power_stats["thermal_updates"]) > 0
            and float(power_stats["power_energy_pJ"]) > 0.0,
            f"power_events={power_stats['power_events']} "
            f"thermal_updates={power_stats['thermal_updates']}",
        )

        ordering_image = temp / "ordering_init.txt"
        ordering_trace = temp / "ordering.trace"
        ordering_commands = temp / "ordering_commands.csv"
        write_trace(ordering_image, "0x1000 data=00112233\n")
        write_trace(
            ordering_trace,
            "0 R 0x1000 expect=00112233\n"
            "1 W 0x1000 data=aabbccdd\n",
        )
        ordering_stats, _ = run(
            binary,
            validation_selection,
            [
                "--memory-image",
                str(ordering_image),
                "--trace",
                str(ordering_trace),
                "--cmd-trace",
                str(ordering_commands),
                "--validate-dfi-trace",
            ],
        )
        ordering_rows = read_csv(ordering_commands)
        ordering_rd = next(
            row for row in ordering_rows if row["command"] in {"RD", "RDA"}
        )
        ordering_wr = next(
            row for row in ordering_rows if row["command"] in {"WR", "WRA"}
        )
        add(
            checks,
            "same_address_ordering",
            int(ordering_stats["data_mismatches"]) == 0
            and int(ordering_rd["cycle"]) < int(ordering_wr["cycle"])
            and int(ordering_stats["dfi_validation_expected_checks"]) > 0,
            f"rd_cycle={ordering_rd['cycle']} wr_cycle={ordering_wr['cycle']} "
            f"mismatches={ordering_stats['data_mismatches']}",
        )

        wrong_trace = temp / "wrong_expected.trace"
        write_trace(wrong_trace, "0 R 0x1000 expect=ffffffff\n")
        wrong_run = run_cli(
            binary,
            [
                "--config",
                str(config), "--standard", args.standard, "--preset", args.preset,
                "--memory-image",
                str(ordering_image),
                "--trace",
                str(wrong_trace),
            ],
        )
        wrong_stats = parse_stats(wrong_run.stdout)
        add(
            checks,
            "data_mismatch_exit_code",
            wrong_run.returncode == 3
            and int(wrong_stats.get("data_mismatches", "0")) == 1,
            f"returncode={wrong_run.returncode} "
            f"mismatches={wrong_stats.get('data_mismatches', 'missing')}",
        )

        backend_write = temp / "backend_write.trace"
        backend_read = temp / "backend_read.trace"
        write_trace(backend_write, f"0 W 0x2000 data={payload}\n")
        write_trace(backend_read, f"0 R 0x2000 expect={payload}\n")
        backend_ok = True
        backend_details = []
        for backend_kind in ("mmap_sparse", "chunk_file"):
            backend_file = temp / f"{backend_kind}.bin"
            backend_args = [
                "--memory-backend",
                backend_kind,
                "--memory-capacity-bytes",
                str(1024 * 1024),
                "--memory-data-file",
                str(backend_file),
            ]
            write_stats, _ = run(
                binary,
                validation_selection,
                ["--trace", str(backend_write), *backend_args],
            )
            read_stats, _ = run(
                binary,
                validation_selection,
                ["--trace", str(backend_read), *backend_args],
            )
            current_ok = (
                int(write_stats["unique_written_lines"]) == 1
                and int(read_stats["data_checked_reads"]) == 1
                and int(read_stats["data_mismatches"]) == 0
            )
            backend_ok = backend_ok and current_ok
            backend_details.append(f"{backend_kind}={current_ok}")
        add(
            checks,
            "file_backed_backend_persistence",
            backend_ok,
            " ".join(backend_details),
        )

        supported_standards = {}
        for standard in ("hbm4", "hbm3", "lpddr6", "lpddr5"):
            completed = run_cli(
                binary, ["--standard", standard, "--requests", "0"]
            )
            stats = parse_stats(completed.stdout)
            supported_standards[standard] = (
                completed.returncode == 0 and bool(stats.get("standard"))
            )
        add(
            checks,
            "supported_standard_scope",
            all(supported_standards.values()),
            f"standards={supported_standards}",
        )

        burst_trace = temp / "burst.trace"
        write_trace(
            burst_trace,
            "BW 0x4000 len=64 pattern=ff\n"
            "BR 0x4000 len=64 check=last_write\n",
        )
        burst_stats, _ = run(
            binary,
            validation_selection,
            ["--trace", str(burst_trace), "--requests", "0"],
        )
        add(
            checks,
            "streaming_burst_frontend",
            int(burst_stats["burst_trace_lines"]) == 2
            and int(burst_stats["burst_split_requests"]) == 4
            and int(burst_stats["data_mismatches"]) == 0,
            f"lines={burst_stats['burst_trace_lines']} "
            f"split_requests={burst_stats['burst_split_requests']}",
        )

        load_stats, _ = run(
            binary,
            validation_selection,
            [
                "--pattern",
                "stream",
                "--requests",
                "4096",
                "--read-ratio",
                "100",
            ],
        )
        utilization = float(load_stats["bandwidth_util_pct"])
        achieved = float(load_stats["achieved_bw_GBps"])
        bandwidth_min_pct = (
            args.bandwidth_min_pct
            if args.bandwidth_min_pct is not None
            else args.bandwidth_baseline_pct
            - args.bandwidth_regression_margin_pct
        )
        add(
            checks,
            "sustained_bandwidth_threshold",
            utilization >= bandwidth_min_pct
            and achieved <= actual_peak * 1.001
            and load_stats["hit_cycle_limit"] == "false",
            f"utilization={utilization:.2f}% threshold={bandwidth_min_pct:.2f}% "
            f"baseline={args.bandwidth_baseline_pct:.2f}% "
            f"margin={args.bandwidth_regression_margin_pct:.2f}pp",
        )

        low_config = temp / "low_ncl.cfg"
        high_config = temp / "high_ncl.cfg"
        make_sensitivity_config(low_config, timings["nCL"] - 4)
        make_sensitivity_config(high_config, timings["nCL"] + 4)
        low_stats, _ = run(
            binary, [*validation_selection, "--config", str(low_config)],
            ["--trace", str(one_read)])
        high_stats, _ = run(
            binary, [*validation_selection, "--config", str(high_config)],
            ["--trace", str(one_read)])
        low_latency = float(low_stats["avg_read_latency"])
        high_latency = float(high_stats["avg_read_latency"])
        expected_delta = 8 * tick_multiplier
        add(
            checks,
            "ncl_sensitivity",
            close(high_latency - low_latency, float(expected_delta)),
            f"low={low_latency:.2f} high={high_latency:.2f} "
            f"expected_delta={expected_delta}",
        )

        full_stack_stats, _ = run(binary, project_selection, ["--requests", "0"])
        full_stack_peak = float(full_stack_stats["peak_bandwidth_GBps"])
        add(
            checks,
            "project_full_stack_scope",
            full_stack_stats["standard"] == "HBM4"
            and full_stack_stats["hbm_full_32ch_stack"] == "true"
            and int(full_stack_stats["channels"]) == 32
            and int(full_stack_stats["data_bus_bits"]) == 2048
            and close(full_stack_peak, 2048.0),
            f"channels={full_stack_stats['channels']} "
            f"bus_bits={full_stack_stats['data_bus_bits']} "
            f"peak={full_stack_peak:.1f} GB/s",
        )

        validate_manifest(manifest, checks)
        validate_project_identity(identity_manifest, checks)
        metrics.update(
            {
                "peak_bandwidth_GBps": actual_peak,
                "sustained_bandwidth_GBps": achieved,
                "bandwidth_util_pct": utilization,
                "act_to_rd_ticks": act_rd_gap,
                "closed_row_read_ticks": actual_latency,
                "timing_entries": len(timing_rows),
                "research_default_entries": research_entries,
                "project_full_stack_peak_bandwidth_GBps": full_stack_peak,
            }
        )

    for check in checks:
        status = "PASS" if check.passed else "FAIL"
        print(f"{status:4} {check.name}: {check.detail}")

    report = {
        "schema_version": 1,
        "scope": "hbm_sim_project_native_analytical_storage_dfi_provenance",
        "model_authority": "hbm_sim_project_requirements_and_declared_standard_profile",
        "external_reference_required": False,
        "device_claim": "research_baseline_not_vendor_calibrated",
        "config": str(config),
        "checks": [asdict(check) for check in checks],
        "metrics": metrics,
        "passed": all(check.passed for check in checks),
    }
    if args.json_out:
        args.json_out.parent.mkdir(parents=True, exist_ok=True)
        args.json_out.write_text(
            json.dumps(report, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )

    failed = [check for check in checks if not check.passed]
    print(f"\nmodel validation: {len(checks) - len(failed)}/{len(checks)} passed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
