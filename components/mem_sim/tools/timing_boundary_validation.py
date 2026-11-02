#!/usr/bin/env python3
"""Audit active table constraints and stateful LPDDR6 REFdb S/L boundaries."""

from __future__ import annotations

import argparse
import csv
import json
import re
import subprocess
import sys
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]

CORE_PARAMETERS = {
    "HBM3": {
        "nRCDRD", "nRCDWR", "nRP", "nRAS", "nRC", "nRTP",
        "nCCDS", "nCCDL", "nRRDS", "nRRDL", "nFAW", "nRTW",
        "nRFC", "nRFCpb", "nCWL+nBL+nWR", "nCWL+nBL+nWTRS",
    },
    "HBM4": {
        "nRCDRD", "nRCDWR", "nRP", "nRAS", "nRC", "nRTP",
        "nCCDS", "nCCDL", "nRRDS", "nRRDL", "nFAW", "nRTW",
        "nRFC", "nRFCpb", "nRFMab", "nRFMpb", "nCWL+nBL+nWR",
        "nCWL+nBL+nWTRS",
    },
    "LPDDR5": {
        "nRCDRD", "nRCDWR", "nRP", "nRAS", "nRC", "nRTP", "nCCDS",
        "nCCDL", "nRRDS", "nRRDL", "nFAW", "nRFC", "nRFCpb",
        "nCL+nBL+2-nCWL", "nCWL+nBL+nWR", "nCWL+nBL+nWTRS",
    },
    "LPDDR6": {
        "nRCDRD", "nRCDWR", "nRP", "nRAS", "nRC", "nRTP", "nCCDS",
        "nCCDL", "nRRDS", "nRRDL", "nFAW", "nRFC", "nRFCpb",
        "nREFDB2ACT", "nREFDB2REFDBS", "nREFDB2REFDBL", "nCAS",
        "nWCKSYNC", "nWCKTRAIN", "nDVFS", "nCL+nBL+2-nCWL",
        "nCWL+nBL+nWR", "nCWL+nBL+nWTRS",
    },
}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--probe",
        type=Path,
        default=ROOT / "build" / "timing_boundary_tests",
    )
    parser.add_argument("--csv-out", type=Path)
    parser.add_argument("--json-out", type=Path)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    probe = args.probe.resolve()
    if not probe.is_file():
        raise SystemExit(f"timing boundary probe not found: {probe}")

    with tempfile.TemporaryDirectory(prefix="hbm_timing_boundaries_") as temp_name:
        generated_csv = Path(temp_name) / "timing_boundaries.csv"
        completed = subprocess.run(
            [str(probe), "--csv-out", str(generated_csv)],
            cwd=ROOT,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
        if completed.returncode != 0:
            print(completed.stdout, end="")
            print(completed.stderr, end="", file=sys.stderr)
            return completed.returncode
        with generated_csv.open(newline="", encoding="utf-8") as stream:
            rows = list(csv.DictReader(stream))

        checks: list[dict[str, object]] = []

        def add(name: str, passed: bool, detail: str) -> None:
            checks.append({"name": name, "passed": bool(passed), "detail": detail})

        add("nonempty_matrix", bool(rows), f"rows={len(rows)}")
        failed_rows = [row for row in rows if row["result"] != "PASS"]
        add("all_boundaries_pass", not failed_rows, f"failed={len(failed_rows)}")
        invalid_edges = [
            row for row in rows
            if row["t_minus_1_allowed"] != "false"
            or row["last_internal_tick_allowed"] != "false"
            or row["at_t_allowed"] != "true"
            or row["other_scope_allowed"] != "true"
        ]
        add("edge_and_scope_semantics", not invalid_edges, f"invalid={len(invalid_edges)}")

        coverage: dict[str, object] = {}
        for standard, expected in CORE_PARAMETERS.items():
            standard_rows = [row for row in rows if row["standard"] == standard]
            observed = {row["parameter"] for row in standard_rows}
            missing = sorted(expected - observed)
            add(
                f"{standard.lower()}_core_coverage",
                bool(standard_rows) and not missing,
                f"rows={len(standard_rows)} missing={missing}",
            )
            coverage[standard] = {
                "rows": len(standard_rows),
                "parameters": sorted(observed),
                "required_core_parameters": sorted(expected),
                "missing": missing,
            }

        match = re.search(r"zero-latency/non-applicable constraints skipped=(\d+)", completed.stdout)
        skipped = int(match.group(1)) if match else None
        add("probe_summary_parse", skipped is not None, f"skipped={skipped}")

        if args.csv_out:
            args.csv_out.parent.mkdir(parents=True, exist_ok=True)
            args.csv_out.write_bytes(generated_csv.read_bytes())

        report = {
            "schema_version": 1,
            "scope": "active_table_constraints_and_stateful_refdb_boundaries_four_standards",
            "probe": str(probe),
            "rows": len(rows),
            "zero_latency_or_non_applicable_skipped": skipped,
            "coverage": coverage,
            "checks": checks,
            "passed": all(bool(check["passed"]) for check in checks),
            "claim_boundary": (
                "Proves implementation boundary enforcement and scope isolation for active "
                "table-driven constraints and counter-dependent REFdb S/L intervals. "
                "A row marked research_default or with pending "
                "clause binding is not evidence of vendor calibration or full JEDEC certification."
            ),
        }
        if args.json_out:
            args.json_out.parent.mkdir(parents=True, exist_ok=True)
            args.json_out.write_text(
                json.dumps(report, indent=2, sort_keys=True) + "\n",
                encoding="utf-8",
            )

    for check in checks:
        print(f"{'PASS' if check['passed'] else 'FAIL':4} {check['name']}: {check['detail']}")
    passed_count = sum(bool(check["passed"]) for check in checks)
    print(f"\ntiming boundary validation: {passed_count}/{len(checks)} checks passed")
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    sys.exit(main())
