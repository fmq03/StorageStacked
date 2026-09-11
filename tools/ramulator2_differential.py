#!/usr/bin/env python3
"""Validate the shared HBM3/HBM4/LPDDR5/LPDDR6 surface against Ramulator2.1."""

from __future__ import annotations

import argparse
import csv
import json
import os
import subprocess
import sys
import tempfile
from dataclasses import asdict, dataclass
from pathlib import Path

from config_selection import REFERENCE_PRESETS, selection_args
from result_io import count, number, run_simulator


ROOT = Path(__file__).resolve().parents[1]

STANDARD_CONFIG = {
    "hbm3": {
        "config": REFERENCE_PRESETS["hbm3"][0],
        "preset": REFERENCE_PRESETS["hbm3"][1],
        "ramulator_class": "HBM3", "org_preset": "HBM3_16Gb_8hi",
        "timing_preset": "HBM3_6400Mbps", "controller": "hbm34",
        "cycle_tolerance": 2, "latency_tolerance": 2.0,
        "dimensions": {"channels": 1, "ranks": 1, "sids": 2,
                       "pseudo_channels": 2, "bank_groups": 4,
                       "banks": 4, "rows": 16384, "columns": 32, "transaction_bytes": 32},
        "timing_map": {
            "nBL": "nBL", "nCL": "nCL", "nCWL": "nCWL",
            "nRCDRD": "nRCDRD", "nRCDWR": "nRCDWR", "nRP": "nRP",
            "nRAS": "nRAS", "nRC": "nRC", "nRTP": "nRTP", "nWR": "nWR",
            "nCCDS": "nCCDS", "nCCDL": "nCCDL", "nCCDR": "nCCDR",
            "nRRDS": "nRRDS", "nRRDL": "nRRDL", "nFAW": "nFAW",
            "nRTW": "nRTW", "nWTRS": "nWTRS", "nWTRL": "nWTRL",
            "nRFC": "nRFC", "nRFCpb": "nRFCpb", "nRREFD": "nRREFD",
            "nREFI": "nREFI", "nREFIpb": "nREFIpb",
        },
    },
    "hbm4": {
        "config": REFERENCE_PRESETS["hbm4"][0],
        "preset": REFERENCE_PRESETS["hbm4"][1],
        "ramulator_class": "HBM4", "org_preset": "HBM4_32Gb_8Hi",
        "timing_preset": "HBM4_8000Mbps", "controller": "hbm34",
        "cycle_tolerance": 2, "latency_tolerance": 2.0,
        "dimensions": {"channels": 1, "ranks": 1, "sids": 2,
                       "pseudo_channels": 2, "bank_groups": 2,
                       "banks": 8, "rows": 16384, "columns": 32, "transaction_bytes": 32},
        "timing_map": {
            "nBL": "nBL", "nCL": "nCL", "nCWL": "nCWL",
            "nRCDRD": "nRCDRD", "nRCDWR": "nRCDWR", "nRP": "nRP",
            "nRAS": "nRAS", "nRC": "nRC", "nRTP": "nRTP", "nWR": "nWR",
            "nCCDS": "nCCDS", "nCCDL": "nCCDL", "nCCDR": "nCCDR",
            "nRRDS": "nRRDS", "nRRDL": "nRRDL", "nFAW": "nFAW",
            "nRTW": "nRTW", "nWTRS": "nWTRS", "nWTRL": "nWTRL",
            "nRFC": "nRFC", "nRFCpb": "nRFCpb", "nRREFD": "nRREFD",
            "nREFI": "nREFI", "nREFIpb": "nREFIpb",
            "nRFMab": "nRFMab", "nRFMpb": "nRFMpb",
        },
    },
    "lpddr5": {
        "config": REFERENCE_PRESETS["lpddr5"][0],
        "preset": REFERENCE_PRESETS["lpddr5"][1],
        "ramulator_class": "LPDDR5", "org_preset": "LPDDR5_16Gb_x16",
        "timing_preset": "LPDDR5_6400", "controller": "lpddr5",
        "cycle_tolerance": 8, "latency_tolerance": 2.0,
        "dimensions": {"channels": 1, "ranks": 1, "sids": 1,
                       "pseudo_channels": 1, "bank_groups": 4,
                       "banks": 4, "rows": 65536, "columns": 64, "transaction_bytes": 32},
        "timing_map": {
            "nBL": "nBL_min", "nCL": "nCL", "nCWL": "nCWL",
            "nRCDRD": "nRCD", "nRCDWR": "nRCD", "nRP": "nRP",
            "nRPab": "nRPab", "nRAS": "nRAS", "nRC": "nRC",
            "nRTP": "nRTP", "nWR": "nWR", "nCCDS": "nCCDS",
            "nCCDL": "nCCDL", "nRRDS": "nRRDS", "nRRDL": "nRRDL",
            "nFAW": "nFAW", "nWTRS": "nWTRS", "nWTRL": "nWTRL",
            "nRFC": "nRFC", "nRFCpb": "nRFCpb", "nREFI": "nREFI",
            "nREFIpb": "nREFIpb", "nAADMax": "nAAD", "nCAS": "nCAS",
            "nWCKPST": "nWCKPST", "nCS": "nCS", "nPPD": "nPPD",
        },
    },
    "lpddr6": {
        "config": REFERENCE_PRESETS["lpddr6"][0],
        "preset": REFERENCE_PRESETS["lpddr6"][1],
        "ramulator_class": "LPDDR6", "org_preset": "LPDDR6_16Gb_x12",
        "timing_preset": "LPDDR6_10667_BL24", "controller": "lpddr6",
        "cycle_tolerance": 8, "latency_tolerance": 2.0,
        "dimensions": {"channels": 1, "ranks": 1, "sids": 1,
                       "pseudo_channels": 1, "bank_groups": 4,
                       "banks": 4, "rows": 65536, "columns": 64, "transaction_bytes": 32},
        "timing_map": {
            "nBL": "nBL_min", "nCL": "nRL", "nCWL": "nWL",
            "nRCDRD": "nRCDr", "nRCDWR": "nRCDw", "nRP": "nRP",
            "nRPab": "nRPab", "nRAS": "nRAS", "nRC": "nRC",
            "nRTP": "nRTP", "nCCDS": "nCCDS", "nCCDL": "nCCDL",
            "nRRDS": "nRRD", "nRRDL": "nRRD", "nFAW": "nFAW",
            "nWTRS": "nWTRS", "nWTRL": "nWTRL", "nRFC": "nRFC",
            "nREFI": "nREFI", "nAADMax": "nAAD", "nCAS": "nCAS",
            "nWCKPST": "nWCKPST", "nCS": "nCS", "nPPD": "nPPD",
        },
    },
}


def req(kind: str, *, row: int = 0, column: int = 0, bank_group: int = 0,
        bank: int = 0, pseudo_channel: int = 0, sid: int = 0) -> dict[str, object]:
    return {"kind": "request", "type": kind, "row": row, "column": column,
            "bank_group": bank_group, "bank": bank,
            "pseudo_channel": pseudo_channel, "sid": sid}


def maintenance(command: str, *, bank_group: int = 0, bank: int = 0) -> dict[str, object]:
    return {"kind": "maintenance", "command": command, "row": 0, "column": 0,
            "bank_group": bank_group, "bank": bank, "pseudo_channel": 0, "sid": 0}


BASE_SCENARIOS = {
    "closed_read": {"events": [req("Read")]},
    "closed_write": {"events": [req("Write")]},
    "row_hit": {"events": [req("Read", column=0), req("Read", column=1)]},
    "row_conflict": {"events": [req("Read", row=0), req("Read", row=1)]},
    # Pre-open the participating rows before measuring column-command constraints.
    # This isolates nCCD/turnaround behavior from controller-specific ACT1/ACT2
    # interleaving decisions.
    "same_bg_read_read": {
        "preopen": [req("Read", bank=0), req("Read", bank=1)],
        "events": [req("Read", bank=0, column=1), req("Read", bank=1, column=1)]},
    "different_bg_read_read": {
        "preopen": [req("Read", bank_group=0), req("Read", bank_group=1)],
        "events": [req("Read", bank_group=0, column=1),
                   req("Read", bank_group=1, column=1)]},
    "read_to_write": {
        "preopen": [req("Read", bank_group=0), req("Read", bank_group=1)],
        "events": [req("Read", bank_group=0, column=1),
                   {**req("Write", bank_group=1, column=1), "delay_before": 1}]},
    "write_to_read": {
        "preopen": [req("Read", bank_group=0), req("Read", bank_group=1)],
        "events": [req("Write", bank_group=0, column=1),
                   {**req("Read", bank_group=1, column=1), "delay_before": 8}]},
    "four_activate": {
        "events": [req("Read", bank_group=i % 2, bank=i // 2) for i in range(4)]},
    "five_activate_tFAW": {
        "events": [req("Read", bank_group=i % 2, bank=i // 2) for i in range(5)]},
    "auto_precharge_read": {"row_policy": "closed", "events": [req("Read")]},
}

MAINTENANCE_SCENARIOS = {
    "hbm3": {
        "refresh_per_bank": {"events": [maintenance("REFpb")]},
        "refresh_all_bank": {"events": [maintenance("REFab")]},
        "rfm_per_bank": {"events": [maintenance("RFMpb")]},
        "rfm_all_bank": {"events": [maintenance("RFMab")]},
    },
    "hbm4": {
        "refresh_per_bank": {"events": [maintenance("REFpb")]},
        "refresh_all_bank": {"events": [maintenance("REFab")]},
        "rfm_per_bank": {"events": [maintenance("RFMpb")]},
        "rfm_all_bank": {"events": [maintenance("RFMab")]},
    },
    "lpddr5": {
        "refresh_per_bank": {"events": [maintenance("REFpb")]},
        "refresh_all_bank": {"events": [maintenance("REFab")]},
    },
    "lpddr6": {"refresh_all_bank": {"events": [maintenance("REFab")]}}
}


RAMULATOR_HELPER = r"""
import json
import sys
import ramulator
import tests.controller_scheduling.harness as cs

payload = json.load(sys.stdin)
result = {"standards": {}}

def make_dut(meta, row_policy_name):
    dram_cls = getattr(ramulator.dram, meta["ramulator_class"])
    dram_kwargs = {"org_preset": meta["org_preset"], "timing_preset": meta["timing_preset"]}
    if meta["controller"].startswith("lpddr"):
        dram_kwargs["rank"] = 1
    dram = dram_cls(**dram_kwargs)
    row_policy = ramulator.row_policy.ClosedCAP(cap=1) if row_policy_name == "closed" else ramulator.row_policy.Open()
    if meta["controller"] == "hbm34":
        return cs.ControllerUnderTest.make_hbm34(dram, row_policy=row_policy)
    controller_cls = getattr(ramulator.controller, "LPDDR5" if meta["controller"] == "lpddr5" else "LPDDR6")
    kwargs = {
        "dram": dram,
        "scheduler": ramulator.scheduler.FRFCFS(),
        "refresh_manager": ramulator.refresh_manager.NoRefresh(),
        "row_policy": row_policy,
        "addr_mapper": ramulator.addr_mapper.PassThroughAddrMapper(),
    }
    if meta["controller"] == "lpddr6":
        kwargs["wck_sync_mode"] = "always_on"
    return cs.ControllerUnderTest(controller_cls(**kwargs))

for standard, entry in payload["standards"].items():
    standard_result = {"scenarios": {}}
    for name, scenario in entry["scenarios"].items():
        dut = make_dut(entry["meta"], scenario.get("row_policy", "open"))
        def send(event):
            levels = {
                "Channel": 0, "PseudoChannel": event.get("pseudo_channel", 0),
                "Sid": event.get("sid", 0), "Rank": 0,
                "BankGroup": event.get("bank_group", 0), "Bank": event.get("bank", 0),
                "Row": event.get("row", 0), "Column": event.get("column", 0),
            }
            address = dut.addr_vec(**{key: value for key, value in levels.items() if key in dut.level_names})
            if event["kind"] == "request":
                dut.send_request(event["type"], address)
            else:
                dut.priority_send(event["command"], address)
        for event in scenario.get("preopen", []):
            send(event)
        if scenario.get("preopen"):
            dut.run_until_idle(max_ticks=200000)
        history_start = len(dut.history)
        for event in scenario["events"]:
            for _ in range(event.get("delay_before", 0)):
                dut.tick()
            send(event)
        dut.run_until_idle(max_ticks=200000)
        history = dut.history[history_start:]
        standard_result["scenarios"][name] = {
            "events": [{"cycle": item.clk, "command": item.command,
                        "decoded": dict(zip(dut.level_names, item.addr_vec))}
                       for item in history],
            "stats": dut.stats(),
        }
        standard_result["timings"] = dut.timings
        standard_result["tick_multiplier"] = dut.tick_multiplier
        standard_result["level_names"] = dut.level_names
        org, _ = dut.dram.resolve()
        cls = type(dut.dram)
        # Match AddrMapperBase::init: Column excludes prefetch address bits.
        # LPDDR6 has BL24 but an address-prefetch factor of 16 and 32B payload.
        prefetch = cls.internal_prefetch_size
        assert org["column"] % prefetch == 0
        tx_bytes = cls.data_payload_bytes or prefetch * org["channel_width"] // 8
        geometry = {
            "channels": 1, "ranks": org.get("rank", 1), "sids": org.get("sid", 1),
            "pseudo_channels": org.get("pseudochannel", 1),
            "bank_groups": org["bankgroup"], "banks": org["bank"],
            "rows": org["row"], "columns": org["column"] // prefetch,
            "transaction_bytes": tx_bytes,
        }
        standard_result["geometry"] = geometry
        standard_result["column_conversion"] = {
            "reference_columns": org["column"], "address_prefetch_factor": prefetch,
            "transaction_columns": geometry["columns"],
            "reference_mapper": "PassThroughAddrMapper (coordinate scenarios); normalized geometry follows AddrMapperBase",
        }
    result["standards"][standard] = standard_result
json.dump(result, sys.stdout)
"""


@dataclass
class Check:
    name: str
    passed: bool
    detail: str


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", type=Path, default=ROOT / "build/hbm_sim")
    parser.add_argument("--ramulator-root", type=Path, required=True)
    parser.add_argument("--standards", default="hbm3,hbm4,lpddr5,lpddr6")
    parser.add_argument("--config", type=Path, help="single-standard compatibility override")
    parser.add_argument("--identity-manifest", type=Path,
                        default=ROOT / "configs/validation/project_identity.csv")
    parser.add_argument("--cycle-tolerance", type=int)
    parser.add_argument("--json-out", type=Path)
    return parser.parse_args()


def selected_standards(value: str) -> list[str]:
    standards = list(STANDARD_CONFIG) if value.strip().lower() == "all" else [
        item.strip().lower() for item in value.split(",") if item.strip()]
    unknown = [item for item in standards if item not in STANDARD_CONFIG]
    if unknown:
        raise SystemExit(f"unsupported standards: {unknown}")
    if not standards:
        raise SystemExit("no standards selected")
    return standards


def scenarios_for(standard: str) -> dict[str, dict[str, object]]:
    scenarios = dict(BASE_SCENARIOS)
    if standard.startswith("hbm"):
        scenarios["parallel_pseudo_channel"] = {
            "events": [req("Read", pseudo_channel=0), req("Read", pseudo_channel=1)]}
        scenarios["parallel_sid"] = {
            "events": [req("Read", sid=0), req("Read", sid=1)]}
    scenarios.update(MAINTENANCE_SCENARIOS[standard])
    d = STANDARD_CONFIG[standard]["dimensions"]
    scenarios.update({
        "address_row_end": {"events": [req("Read", column=d["columns"] - 1)]},
        "address_next_row": {"events": [req("Read", row=1)]},
        "address_last_bank": {"events": [req("Read", bank_group=d["bank_groups"] - 1,
                                             bank=d["banks"] - 1)]},
        "address_last_transaction": {"events": [req("Read", row=d["rows"] - 1,
            column=d["columns"] - 1, bank_group=d["bank_groups"] - 1, bank=d["banks"] - 1,
            sid=d["sids"] - 1, pseudo_channel=d["pseudo_channels"] - 1)]},
    })
    return scenarios


def run_ramulator(root: Path, standards: list[str]) -> dict:
    payload = {"standards": {}}
    for standard in standards:
        meta = STANDARD_CONFIG[standard]
        payload["standards"][standard] = {
            "meta": {key: meta[key] for key in
                     ("ramulator_class", "org_preset", "timing_preset", "controller")},
            "scenarios": scenarios_for(standard),
        }
    env = os.environ.copy()
    python_paths = [str(root / "python"), str(root / ".python-deps")]
    if env.get("PYTHONPATH"):
        python_paths.append(env["PYTHONPATH"])
    env["PYTHONPATH"] = os.pathsep.join(python_paths)
    library_paths = [str(root)]
    if env.get("LD_LIBRARY_PATH"):
        library_paths.append(env["LD_LIBRARY_PATH"])
    env["LD_LIBRARY_PATH"] = os.pathsep.join(library_paths)
    completed = subprocess.run(
        [sys.executable, "-c", RAMULATOR_HELPER], cwd=root, env=env,
        input=json.dumps(payload), text=True, stdout=subprocess.PIPE,
        stderr=subprocess.PIPE, check=False)
    if completed.returncode != 0:
        raise RuntimeError(f"Ramulator2.1 harness failed:\n{completed.stderr}")
    return json.loads(completed.stdout)


def ramulator_identity(root: Path) -> dict[str, object]:
    """Capture the external reference revision without requiring a Git checkout."""
    def git(*arguments: str) -> str | None:
        completed = subprocess.run(
            ["git", "-C", str(root), *arguments], text=True,
            stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, check=False)
        return completed.stdout.strip() if completed.returncode == 0 else None

    commit = git("rev-parse", "HEAD")
    describe = git("describe", "--always", "--dirty", "--tags")
    status = git("status", "--porcelain")
    return {
        "declared_version": "Ramulator 2.1",
        "git_commit": commit,
        "git_describe": describe,
        "git_dirty": bool(status) if status is not None else None,
        "python_version": sys.version.split()[0],
        "root": str(root),
    }


def address_for(event: dict[str, object], dimensions: dict[str, int]) -> int:
    value = int(event.get("row", 0))
    for field, count in (
        ("channel", dimensions["channels"]), ("rank", dimensions["ranks"]),
        ("sid", dimensions["sids"]), ("pseudo_channel", dimensions["pseudo_channels"]),
        ("bank_group", dimensions["bank_groups"]), ("bank", dimensions["banks"]),
        ("column", dimensions["columns"]),
    ):
        value = value * count + int(event.get(field, 0))
    return value * dimensions["transaction_bytes"]



def normalize_command(standard: str, command: str) -> str:
    normalized = {"CASRD": "CAS_RD", "CASWR": "CAS_WR"}.get(command, command)
    if standard == "lpddr6":
        normalized = {"RD": "RD_S", "WR": "WR_S",
                      "RDA": "RDA_S", "WRA": "WRA_S"}.get(normalized, normalized)
    return normalized


def run_project_scenario(binary: Path, config_args: list[str], standard: str, name: str,
                         scenario: dict[str, object], dimensions: dict[str, int],
                         temp: Path) -> tuple[list[dict[str, object]], dict[str, str]]:
    trace = temp / f"{standard}_{name}.trace"
    command_csv = temp / f"{standard}_{name}.commands.csv"
    lines = []
    measure_cycle = 10000 if scenario.get("preopen") else 0
    for event in scenario.get("preopen", []):
        address = address_for(event, dimensions)
        token = "R" if event["type"] == "Read" else "W"
        lines.append(f"0 {token} 0x{address:x}")
    event_cycle = measure_cycle
    for event in scenario["events"]:
        event_cycle += int(event.get("delay_before", 0))
        address = address_for(event, dimensions)
        if event["kind"] == "request":
            token = "R" if event["type"] == "Read" else "W"
            lines.append(f"{event_cycle} {token} 0x{address:x}")
        else:
            lines.append(f"{event_cycle} M {event['command']} 0x{address:x}")
    trace.write_text("\n".join(lines) + "\n", encoding="ascii")
    command = [str(binary), *config_args, "--trace", str(trace),
               "--cmd-trace", str(command_csv), "--validate-cmd-trace"]
    if scenario.get("row_policy") == "closed":
        command += ["--row-policy", "closed_page"]
    stats, _ = run_simulator(command, cwd=ROOT, diagnostic=True)
    with command_csv.open(newline="", encoding="utf-8") as stream:
        rows = list(csv.DictReader(stream))
    rows = [row for row in rows if int(row["cycle"]) >= measure_cycle]
    events = [{
        "cycle": int(row["cycle"]) - measure_cycle,
        "command": normalize_command(standard, row["command"]),
        "decoded": {"Channel": int(row["channel"]),
                    "PseudoChannel": int(row["pseudo_channel"]),
                    "Sid": int(row["sid"]), "Rank": int(row["rank"]),
                    "BankGroup": int(row["bank_group"]), "Bank": int(row["bank"]),
                    "Row": int(row["row"]), "Column": int(row["column"])},
    } for row in rows]
    return events, stats


def canonical_events(standard: str, scenario_name: str,
                     events: list[dict[str, object]]) -> list[dict[str, object]]:
    """Normalize equivalent policy encodings without hiding timing information."""
    canonical = [dict(event, decoded=dict(event["decoded"])) for event in events]
    if scenario_name == "auto_precharge_read":
        collapsed = []
        index = 0
        while index < len(canonical):
            event = canonical[index]
            read_name = "RD_S" if standard == "lpddr6" else "RD"
            auto_name = "RDA_S" if standard == "lpddr6" else "RDA"
            same_bank = index + 1 < len(canonical) and all(
                canonical[index + 1]["decoded"].get(level) == event["decoded"].get(level)
                for level in ("Channel", "Rank", "Sid", "PseudoChannel",
                              "BankGroup", "Bank"))
            if (event["command"] == read_name and index + 1 < len(canonical)
                    and canonical[index + 1]["command"] == "PREpb" and same_bank):
                event["command"] = auto_name
                collapsed.append(event)
                index += 2
                continue
            collapsed.append(event)
            index += 1
        canonical = collapsed
    if scenario_name in {"four_activate", "five_activate_tFAW"}:
        # Controllers may interleave split ACT stages differently. Compare each
        # addressed bank's protocol sequence and absolute issue cycle instead.
        level_order = ("Channel", "Rank", "Sid", "PseudoChannel",
                       "BankGroup", "Bank", "Row", "Column")
        canonical.sort(key=lambda event: (
            tuple(event["decoded"].get(level, 0) for level in level_order),
            event["cycle"]))
    return canonical


def read_project_timings(binary: Path, config_args: list[str], standard: str,
                         temp: Path) -> tuple[dict[str, int], int, dict]:
    output = temp / (standard + "_timing.csv")
    stats, _ = run_simulator(
        [str(binary), *config_args, "--requests", "0",
         "--dump-timing-table", str(output)], cwd=ROOT, diagnostic=True)
    multiplier = count(stats, "tick_multiplier")
    if multiplier == 0:
        raise ValueError("tick_multiplier must be positive")
    with output.open(newline="", encoding="utf-8") as stream:
        rows = list(csv.DictReader(stream))
    return {row["name"]: int(row["value_nck"]) * multiplier for row in rows}, multiplier, stats


def add(checks: list[Check], name: str, passed: bool, detail: str) -> None:
    checks.append(Check(name, bool(passed), detail))


def main() -> int:
    args = parse_args()
    binary = args.binary.resolve()
    root = args.ramulator_root.resolve()
    standards = selected_standards(args.standards)
    if args.config and len(standards) != 1:
        raise SystemExit("--config requires exactly one selected standard")
    if not binary.is_file():
        raise SystemExit(f"hbm_sim binary not found: {binary}")
    if not (root / "libramulator.so").is_file():
        raise SystemExit(f"Ramulator2.1 build not found: {root}")
    if not args.identity_manifest.is_file():
        raise SystemExit(f"project identity manifest not found: {args.identity_manifest}")
    with args.identity_manifest.open(newline="", encoding="utf-8") as stream:
        capabilities = list(csv.DictReader(stream))

    reference_identity = ramulator_identity(root)
    ramulator = run_ramulator(root, standards)
    checks: list[Check] = []
    standard_results = {}
    with tempfile.TemporaryDirectory(prefix="hbm_ramulator_diff_") as temp_name:
        temp = Path(temp_name)
        for standard in standards:
            meta = STANDARD_CONFIG[standard]
            config = (args.config or meta["config"]).resolve()
            config_args = (["--config", str(config)] if args.config
                           else selection_args(standard, str(meta["preset"])))
            project_timings, multiplier, geometry_stats = read_project_timings(
                binary, config_args, standard, temp)
            ram = ramulator["standards"][standard]
            dims = ram["geometry"]
            add(checks, f"{standard}_normalized_reference_geometry",
                dims == meta["dimensions"], f"reference={dims} fixture={meta['dimensions']}")
            capacity = 1
            for value in dims.values():
                capacity *= value
            observed_geometry = {key: count(geometry_stats, "banks_per_group" if key == "banks"
                else "dram_transaction_bytes" if key == "transaction_bytes" else key)
                for key in dims}
            add(checks, f"{standard}_executed_geometry", observed_geometry == dims,
                f"executed={observed_geometry} reference={dims}")
            add(checks, f"{standard}_capacity_bytes",
                count(geometry_stats, "capacity_per_instance_bytes") == capacity,
                f"executed={count(geometry_stats, 'capacity_per_instance_bytes')} reference={capacity}")
            timing_diffs = {}
            for project_name, ram_name in meta["timing_map"].items():
                project_value = project_timings.get(project_name)
                ram_value = ram["timings"].get(ram_name)
                if project_value != ram_value:
                    timing_diffs[project_name] = {
                        "ramulator_name": ram_name, "hbm_sim_ticks": project_value,
                        "ramulator_ticks": ram_value}
            add(checks, f"{standard}_resolved_timing_table", not timing_diffs,
                f"compared={len(meta['timing_map'])} differences={timing_diffs}")

            results = {}
            for name, scenario in scenarios_for(standard).items():
                project_events, project_stats = run_project_scenario(
                    binary, config_args, standard, name, scenario, meta["dimensions"], temp)
                ram_result = ram["scenarios"][name]
                ram_events = ram_result["events"]
                raw_project_events = project_events
                raw_ram_events = ram_events
                if scenario.get("preopen"):
                    if project_events:
                        origin = project_events[0]["cycle"]
                        project_events = [dict(event, cycle=event["cycle"] - origin)
                                          for event in project_events]
                    if ram_events:
                        origin = ram_events[0]["cycle"]
                        ram_events = [dict(event, cycle=event["cycle"] - origin)
                                      for event in ram_events]
                project_events = canonical_events(standard, name, project_events)
                ram_events = canonical_events(standard, name, ram_events)
                project_commands = [event["command"] for event in project_events]
                ram_commands = [event["command"] for event in ram_events]
                sequence_ok = project_commands == ram_commands
                add(checks, f"{standard}_{name}_commands", sequence_ok,
                    f"hbm_sim={project_commands} ramulator={ram_commands}")

                coordinate_diffs, cycle_deltas = [], []
                coords_ok = len(project_events) == len(ram_events)
                if coords_ok:
                    for event_index, (project_event, ram_event) in enumerate(zip(project_events, ram_events)):
                        for coordinate in sorted(set(project_event["decoded"]) & set(ram_event["decoded"])):
                            a, b = project_event["decoded"][coordinate], ram_event["decoded"][coordinate]
                            if a != b:
                                coordinate_diffs.append([event_index, coordinate, a, b])
                        cycle_deltas.append(project_event["cycle"] - ram_event["cycle"])
                coords_ok = coords_ok and not coordinate_diffs
                add(checks, f"{standard}_{name}_coordinates", coords_ok,
                    f"differences={coordinate_diffs}")

                tolerance = args.cycle_tolerance if args.cycle_tolerance is not None else max(
                    meta["cycle_tolerance"], 4 if name == "parallel_pseudo_channel" else 0)
                cycles_ok = sequence_ok and len(cycle_deltas) == len(project_events) and all(
                    abs(delta) <= tolerance for delta in cycle_deltas)
                add(checks, f"{standard}_{name}_cycles", cycles_ok,
                    f"deltas(hbm-ramulator)={cycle_deltas} tolerance={tolerance}")

                latency_delta = None
                has_read = any(event.get("kind") == "request" and event.get("type") == "Read"
                               for event in scenario["events"])
                if has_read and sequence_ok:
                    project_latency = number(project_stats, "avg_read_latency")
                    ram_latency = number(ram_result["stats"], "avg_read_latency")
                    latency_delta = project_latency - ram_latency
                    latency_tolerance = max(
                        float(meta["latency_tolerance"]),
                        3.0 if name in {"parallel_pseudo_channel", "write_to_read"} else 0.0)
                    add(checks, f"{standard}_{name}_read_latency",
                        abs(latency_delta) <= latency_tolerance,
                        f"hbm_sim={project_latency} ramulator={ram_latency} delta={latency_delta} "
                        f"tolerance={latency_tolerance}")
                results[name] = {
                    "hbm_sim": project_events, "ramulator2": ram_events,
                    "raw_hbm_sim": raw_project_events,
                    "raw_ramulator2": raw_ram_events,
                    "cycle_deltas": cycle_deltas, "read_latency_delta": latency_delta,
                    "project_stats": {key: project_stats.get(key) for key in
                                      ("avg_read_latency", "row_hits", "row_misses",
                                       "row_conflicts", "completed_reads", "completed_writes")},
                    "ramulator_stats": ram_result["stats"],
                }
            standard_results[standard] = {
                "config": str(config), "preset": None if args.config else meta["preset"],
                "tick_multiplier": multiplier,
                "timing_mapping": meta["timing_map"], "timing_differences": timing_diffs,
                "cycle_tolerance": args.cycle_tolerance if args.cycle_tolerance is not None
                                   else meta["cycle_tolerance"],
                "normalized_geometry": dims,
                "capacity_bytes": capacity,
                "column_conversion": ram["column_conversion"],
                "scenarios": results,
            }

    for check in checks:
        print(f"{'PASS' if check.passed else 'FAIL':4} {check.name}: {check.detail}")
    report = {
        "schema_version": 2,
        "scope": "external_reference_overlap_four_standards_commands_timing_maintenance",
        "relationship": "non_normative_external_reference",
        "external_engine_executed": True,
        "external_engine_entry": "python ramulator module and ControllerUnderTest harness",
        "standards": standard_results,
        "ramulator_root": str(root),
        "ramulator_reference": reference_identity,
        "checks": [asdict(check) for check in checks],
        "project_specific_capabilities": capabilities,
        "passed": all(check.passed for check in checks),
        "known_abstractions": {
            "lpddr_split_activate": (
                "hbm_sim models ACT2 in the inclusive [nAADMin,nAADMax] issue window. "
                "The timing-table comparison maps Ramulator nAAD to hbm_sim nAADMax; "
                "nAADMin is a project execution-granularity bound."),
            "lpddr6_burst": (
                "Only the BL24 short-command overlap is compared; Ramulator BL48 and "
                "min/max array-cycle distinctions are outside hbm_sim's single-nBL abstraction."),
            "hbm_parallel_pseudo_channel": (
                "Ramulator serializes one additional command-interface tick between two "
                "pseudo-channels; the resulting two-request mean-latency difference is 3 ticks."),
        },
        "claim_boundary": (
            "Normalized transaction geometry/capacity and selected shared timing/address/command/controller scenarios are checked. "
            "Ramulator receives address vectors through PassThroughAddrMapper; this is not a full external byte-address-mapper equivalence proof. "
            "Project payload, DFI, multi-stack, power, thermal, provenance, and vendor "
            "behavior remain outside this reference comparison."),
    }
    if args.json_out:
        args.json_out.parent.mkdir(parents=True, exist_ok=True)
        args.json_out.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    failed = [check for check in checks if not check.passed]
    print(f"\nRamulator2.1 four-standard overlap: {len(checks)-len(failed)}/{len(checks)} checks passed")
    print("Ramulator reference: "
          f"commit={reference_identity['git_commit'] or 'unavailable'} "
          f"dirty={reference_identity['git_dirty']}")
    print(f"Project authority: hbm_sim; preserved project-specific capabilities: {len(capabilities)}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
