#!/usr/bin/env python3
"""Auxiliary validation against DRAMsim3's older-HBM, IDD and thermal surfaces."""

from __future__ import annotations

import argparse
import configparser
import csv
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

from config_selection import DRAMSIM3_HBM2, explicit_selection_args
from result_io import run_simulator


ROOT = Path(__file__).resolve().parents[1]
PROJECT_CONFIG = DRAMSIM3_HBM2[0]
PROJECT_SELECTION = explicit_selection_args(DRAMSIM3_HBM2)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", type=Path, default=ROOT / "build/hbm_sim")
    parser.add_argument("--dramsim3-root", type=Path, required=True)
    parser.add_argument("--dramsim3-build", default="build-validation")
    parser.add_argument("--dramsim3-thermal-build", default="build-thermal-validation")
    # One 10k-cycle epoch is still in DRAMsim3's transient initialization; two
    # epochs are required before using its final steady-temperature output.
    parser.add_argument("--thermal-cycles", type=int, default=20000)
    parser.add_argument("--skip-thermal-runtime", action="store_true")
    parser.add_argument("--json-out", type=Path)
    return parser.parse_args()


def add(checks: list[dict], name: str, passed: bool, detail: str) -> None:
    checks.append({"name": name, "passed": bool(passed), "detail": detail})


def dramsim_env(root: Path, build_name: str) -> dict[str, str]:
    env = os.environ.copy()
    library = str(root / build_name)
    env["LD_LIBRARY_PATH"] = library + (
        os.pathsep + env["LD_LIBRARY_PATH"] if env.get("LD_LIBRARY_PATH") else "")
    return env


def read_ini(path: Path) -> configparser.ConfigParser:
    parser = configparser.ConfigParser(inline_comment_prefixes=(";", "#"))
    parser.optionxform = str
    parser.read(path)
    return parser


def ini_float(value: str) -> float:
    return float(value.split(";", 1)[0].split("#", 1)[0].strip())


def main() -> int:
    args = parse_args()
    binary = args.binary.resolve()
    dramsim_root = args.dramsim3_root.resolve()
    dramsim_binary = dramsim_root / args.dramsim3_build / "dramsim3main"
    thermal_binary = dramsim_root / args.dramsim3_thermal_build / "dramsim3main"
    hbm2_ini = dramsim_root / "configs/HBM2_4Gb_x128.ini"
    hbm_thermal_ini = dramsim_root / "configs/HBM_4Gb_x128.ini"
    for required in (binary, dramsim_binary, hbm2_ini, PROJECT_CONFIG):
        if not required.is_file():
            raise SystemExit(f"required file not found: {required}")

    ini = read_ini(hbm2_ini)
    timing = ini["timing"]
    power = ini["power"]
    structure = ini["dram_structure"]
    system = ini["system"]
    tck = float(timing["tCK"])
    tras, trp = float(timing["tRAS"]), float(timing["tRP"])
    trc = tras + trp
    burst_cycles = float(structure["BL"]) / 2.0
    devices = float(system["bus_width"]) / float(structure["device_width"])
    vdd = float(power["VDD"])
    expected = {
        "act_energy_pJ": vdd * (float(power["IDD0"]) * trc
                         - (float(power["IDD3N"]) * tras
                            + float(power["IDD2N"]) * trp)) * tck * devices,
        "read_energy_pJ": vdd * (float(power["IDD4R"])
                          - float(power["IDD3N"])) * burst_cycles * tck * devices,
        "write_energy_pJ": vdd * (float(power["IDD4W"])
                           - float(power["IDD3N"])) * burst_cycles * tck * devices,
        "refresh_energy_pJ": vdd * (float(power["IDD5AB"])
                             - float(power["IDD3N"])) * float(timing["tRFC"]) * tck * devices,
    }

    checks: list[dict] = []
    with tempfile.TemporaryDirectory(prefix="hbm_dramsim3_aux_") as temp_name:
        temp = Path(temp_name)
        dram_trace = temp / "single_read.trace"
        dram_trace.write_text("0x0 READ 0\n", encoding="ascii")
        dram_out = temp / "dramsim_command"
        dram_out.mkdir()
        completed = subprocess.run(
            [str(dramsim_binary), str(hbm2_ini), "-c", "200", "-t", str(dram_trace),
             "-o", str(dram_out)], cwd=dramsim_root,
            env=dramsim_env(dramsim_root, args.dramsim3_build), text=True,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)
        if completed.returncode:
            raise RuntimeError(completed.stdout + completed.stderr)
        external_stats = json.loads((dram_out / "dramsim3.json").read_text())["0"]
        external_commands = []
        for line in (dram_out / "dramsim3ch_0cmd.trace").read_text().splitlines():
            fields = line.split()
            if len(fields) >= 2:
                external_commands.append({"cycle": int(fields[0]), "command": fields[1]})
        external_core = [event for event in external_commands
                         if event["command"] in {"activate", "read", "write", "precharge"}]

        project_trace = temp / "project.trace"
        project_trace.write_text("0 R 0x0\n", encoding="ascii")
        project_csv = temp / "project_commands.csv"
        project_stats, _ = run_simulator(
            [str(binary), *PROJECT_SELECTION, "--trace", str(project_trace),
             "--cmd-trace", str(project_csv), "--validate-cmd-trace"], cwd=ROOT, diagnostic=True)
        # DRAMsim3 normalizes HBM input columns by *2, then divides by BL
        # for transaction addressing (configuration.cc InitDRAMParams /
        # SetAddressMapping). Do not equate the raw INI count with our slots.
        transaction_bytes = int(system["bus_width"]) // 8 * int(structure["BL"])
        transaction_columns = int(structure["columns"]) * 2 // int(structure["BL"])
        row_bytes = transaction_columns * transaction_bytes
        capacity_bytes = (row_bytes * int(structure["rows"]) *
                          int(structure["bankgroups"]) * int(structure["banks_per_group"]))
        add(checks, "older_hbm_transaction_geometry",
            int(project_stats["columns"]) == transaction_columns and
            int(project_stats["dram_transaction_bytes"]) == transaction_bytes and
            int(project_stats["capacity_per_instance_bytes"]) == capacity_bytes and
            int(project_stats["stack_height"]) == int(structure["num_dies"]),
            f"reference slots={transaction_columns}, transaction={transaction_bytes}B, "
            f"row={row_bytes}B, channel capacity={capacity_bytes}B, dies={structure['num_dies']}")
        with project_csv.open(newline="", encoding="utf-8") as stream:
            project_commands = [{"cycle": int(row["cycle"]), "command": row["command"]}
                                for row in csv.DictReader(stream)]

        add(checks, "older_hbm_common_command_sequence",
            [event["command"] for event in project_commands] == ["ACT", "RD"] and
            [event["command"] for event in external_core] == ["activate", "read"],
            f"hbm_sim={project_commands} dramsim3={external_core}")
        project_gap = project_commands[1]["cycle"] - project_commands[0]["cycle"]
        external_gap = external_core[1]["cycle"] - external_core[0]["cycle"]
        add(checks, "older_hbm_tRCDRD_gap", project_gap == external_gap == 14,
            f"hbm_sim={project_gap} dramsim3={external_gap} cycles")
        add(checks, "older_hbm_read_latency",
            abs(float(project_stats["avg_read_latency"])
                - float(external_stats["average_read_latency"])) <= 1.0,
            f"hbm_sim={project_stats['avg_read_latency']} "
            f"dramsim3={external_stats['average_read_latency']}")

        observed = {
            "act_energy_pJ": float(project_stats["power_act_energy_pJ"]),
            "read_energy_pJ": float(project_stats["power_read_energy_pJ"]),
        }
        for metric in ("act_energy_pJ", "read_energy_pJ"):
            external_metric = "act_energy" if metric.startswith("act") else "read_energy"
            add(checks, "idd_formula_" + metric,
                abs(observed[metric] - expected[metric]) < 1e-9 and
                abs(float(external_stats[external_metric]) - expected[metric]) < 1e-9,
                f"formula={expected[metric]} hbm_sim={observed[metric]} "
                f"dramsim3={external_stats[external_metric]}")

        dram_write_trace = temp / "single_write.trace"
        # DRAMsim3 acknowledges isolated writes in its write buffer; exceed its
        # high watermark so physical WRITE commands are observable.
        dram_write_trace.write_text(
            "".join(f"0x{index * 64:x} WRITE {index}\n" for index in range(40)) +
            "0x10000 READ 50\n", encoding="ascii")
        dram_write_out = temp / "dramsim_write"
        dram_write_out.mkdir()
        dram_write_run = subprocess.run(
            [str(dramsim_binary), str(hbm2_ini), "-c", "2000", "-t",
             str(dram_write_trace), "-o", str(dram_write_out)], cwd=dramsim_root,
            env=dramsim_env(dramsim_root, args.dramsim3_build), text=True,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)
        if dram_write_run.returncode:
            raise RuntimeError(dram_write_run.stdout + dram_write_run.stderr)
        dram_write_stats = json.loads((dram_write_out / "dramsim3.json").read_text())["0"]
        project_write_trace = temp / "project_write.trace"
        project_write_trace.write_text("0 W 0x0\n", encoding="ascii")
        project_write_stats, _ = run_simulator(
            [str(binary), *PROJECT_SELECTION, "--trace",
             str(project_write_trace)], cwd=ROOT, diagnostic=True)
        observed["write_energy_pJ"] = float(project_write_stats["power_write_energy_pJ"])
        write_count = int(dram_write_stats["num_write_cmds"])
        external_write_per_command = (
            float(dram_write_stats["write_energy"]) / write_count if write_count else 0.0)
        add(checks, "idd_formula_write_energy_pJ",
            write_count > 0 and
            abs(observed["write_energy_pJ"] - expected["write_energy_pJ"]) < 1e-9 and
            abs(external_write_per_command - expected["write_energy_pJ"]) < 1e-9,
            f"formula={expected['write_energy_pJ']} hbm_sim={observed['write_energy_pJ']} "
            f"dramsim3_per_write={external_write_per_command} count={write_count}")

        # DRAMsim3 schedules an all-bank refresh at tREFI. Compare its per-command
        # energy with one explicit hbm_sim REFab maintenance command.
        dram_refresh_out = temp / "dramsim_refresh"
        dram_refresh_out.mkdir()
        dram_refresh_run = subprocess.run(
            [str(dramsim_binary), str(hbm2_ini), "-c", "4000", "-t", str(dram_trace),
             "-o", str(dram_refresh_out)], cwd=dramsim_root,
            env=dramsim_env(dramsim_root, args.dramsim3_build), text=True,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)
        if dram_refresh_run.returncode:
            raise RuntimeError(dram_refresh_run.stdout + dram_refresh_run.stderr)
        dram_refresh_stats = json.loads((dram_refresh_out / "dramsim3.json").read_text())["0"]
        project_refresh_trace = temp / "project_refresh.trace"
        project_refresh_trace.write_text("0 M REFab 0x0\n", encoding="ascii")
        project_refresh_stats, _ = run_simulator(
            [str(binary), *PROJECT_SELECTION, "--trace",
             str(project_refresh_trace)], cwd=ROOT, diagnostic=True)
        observed["refresh_energy_pJ"] = float(
            project_refresh_stats["power_refresh_energy_pJ"])
        refresh_count = int(dram_refresh_stats["num_ref_cmds"])
        external_refresh_per_command = (
            float(dram_refresh_stats["ref_energy"]) / refresh_count if refresh_count else 0.0)
        add(checks, "idd_formula_refresh_energy_pJ",
            refresh_count > 0 and
            abs(observed["refresh_energy_pJ"] - expected["refresh_energy_pJ"]) < 1e-9 and
            abs(external_refresh_per_command - expected["refresh_energy_pJ"]) < 1e-9,
            f"formula={expected['refresh_energy_pJ']} hbm_sim="
            f"{observed['refresh_energy_pJ']} dramsim3_per_ref="
            f"{external_refresh_per_command} count={refresh_count}")

        thermal_result = {"runtime_executed": False}
        if not args.skip_thermal_runtime:
            if not thermal_binary.is_file():
                raise SystemExit(f"thermal build not found: {thermal_binary}")
            thermal_out = temp / "dramsim_thermal"
            thermal_out.mkdir()
            thermal_run = subprocess.run(
                [str(thermal_binary), str(hbm_thermal_ini), "-c", str(args.thermal_cycles),
                 "-s", "random", "-o", str(thermal_out)], cwd=dramsim_root,
                env=dramsim_env(dramsim_root, args.dramsim3_thermal_build), text=True,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)
            if thermal_run.returncode:
                raise RuntimeError(thermal_run.stdout + thermal_run.stderr)
            with (thermal_out / "dramsim3final_temp.csv").open(
                    newline="", encoding="utf-8") as stream:
                external_peak = max(float(row["temperature"]) for row in csv.DictReader(stream))
            project_thermal_stats, _ = run_simulator(
                [str(binary), *PROJECT_SELECTION, "--requests", "2000",
                 "--pattern", "random", "--read-ratio", "70", "--inject-interval", "1",
                 "--seed", "20260816", "--max-cycles", "100000000"], cwd=ROOT, diagnostic=True)
            project_peak = float(project_thermal_stats["thermal_peak_temp_C"])
            thermal_ini = read_ini(hbm_thermal_ini)
            ambient = ini_float(thermal_ini["thermal"]["amb_temp"])
            add(checks, "thermal_runtime_positive_heating",
                external_peak > ambient and project_peak > ambient,
                f"ambient={ambient} hbm_sim_peak={project_peak} "
                f"dramsim3_peak={external_peak}")
            add(checks, "thermal_shared_ambient_and_geometry",
                ambient == 40.0 and
                ini_float(thermal_ini["thermal"]["chip_dim_x"]) == 0.008 and
                ini_float(thermal_ini["thermal"]["chip_dim_y"]) == 0.008,
                "both runs use ambient=40C and 8mm x 8mm HBM footprint")
            thermal_result = {
                "runtime_executed": True, "cycles": args.thermal_cycles,
                "dramsim3_peak_temperature_C": external_peak,
                "hbm_sim_peak_temperature_C": project_peak,
                "comparison_kind": "qualitative_shared-input_trend_not_absolute_calibration",
            }

    commit = subprocess.run(["git", "rev-parse", "HEAD"], cwd=dramsim_root,
                            text=True, stdout=subprocess.PIPE, check=True).stdout.strip()
    report = {
        "schema_version": 1,
        "scope": "dramsim3_older_hbm_idd_thermal_auxiliary_validation",
        "external_engine_executed": True,
        "external_engine_binary": str(dramsim_binary),
        "external_thermal_binary": None if args.skip_thermal_runtime else str(thermal_binary),
        "dramsim3_root": str(dramsim_root), "dramsim3_commit": commit,
        "reference_config": str(hbm2_ini), "project_config": str(PROJECT_CONFIG),
        "project_preset": DRAMSIM3_HBM2[2],
        "formula_expected_pJ": expected, "project_observed_pJ": observed,
        "thermal": thermal_result, "checks": checks,
        "passed": all(check["passed"] for check in checks),
        "claim_boundary": (
            "DRAMsim3 HBM2 is used only for shared legacy ACT/RD timing and IDD formulas. "
            "Thermal comparison checks shared-input qualitative heating because hbm_sim's "
            "behavioral sparse thermal-coupling model is not DRAMsim3's SuperLU floorplan solver. This does not "
            "validate HBM3/HBM4/LPDDR5/LPDDR6 device power or temperature accuracy."),
    }
    if args.json_out:
        args.json_out.parent.mkdir(parents=True, exist_ok=True)
        args.json_out.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n",
                                 encoding="utf-8")
    for check in checks:
        print(f"{'PASS' if check['passed'] else 'FAIL':4} {check['name']}: {check['detail']}")
    failed = sum(not check["passed"] for check in checks)
    print(f"\nDRAMsim3 auxiliary validation: {len(checks)-failed}/{len(checks)} checks passed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
