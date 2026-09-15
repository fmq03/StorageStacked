#!/usr/bin/env python3
"""Export compact paper data only after source and evidence checks pass."""
import argparse
import hashlib
import json
import re
import shutil
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
from provenance import verify_inputs

def read(path):
    return json.loads(path.read_text())

def digest(path):
    h = hashlib.sha256()
    with path.open("rb") as f:
        for block in iter(lambda: f.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()

def main():
    p = argparse.ArgumentParser()
    p.add_argument("--system", default="results/experiments-final")
    p.add_argument("--dc", default="results/dc-final")
    p.add_argument("--acceptance", default="results/acceptance")
    a = p.parse_args()
    system, dc, acceptance = [ROOT / x for x in (a.system, a.dc, a.acceptance)]
    inputs = read(system / "inputs.json")
    verify_inputs(inputs)
    records = read(system / "measurements.json")
    expected = {(i, m) for i in range(5) for m in ("logic_die", "vortex_software")}
    assert len(records) == 10
    assert {(r["chunk_log2"], r["mode"]) for r in records} == expected
    trace_hashes, checks = {}, {}
    for r in records:
        assert r["passed"] and r["chunks"] == 32 and r["top_k"] == 12 and r["queries"] == 4
        name = f"c32-b{1 << r['chunk_log2']}-{r['mode']}"
        run = system / name
        actual = read(run / "summary.json")
        assert all(r[k] == v for k, v in actual.items())
        assert r["fixture_sha256"] == digest(system / f"c32-b{1 << r['chunk_log2']}.bin")
        offline, memory = read(run / "offline_verification.json"), read(run / "memsim_core.json")
        assert offline["passed"] and memory["passed"]
        assert memory["submitted"] == memory["returned"]
        assert memory["command_errors"] == memory["dfi_errors"] == 0
        if r["mode"] == "logic_die":
            assert r["local_dma_bytes"] == 32 * (1 << r["chunk_log2"]) * 128 * 2 + 4 * 256
            assert all(q["dma_beats"] == 8 for q in r["query_results"])
        checks[name] = {"offline": offline, "memory": memory}
        trace_hashes[name] = {f: digest(run / f) for f in
                             ("axi.csv", "axi_logic_die.vcd", "flits_forward.csv",
                              "flits_reverse.csv", "memory.csv", "summary.json")}
        r["artifact_directory"] = str(run.relative_to(ROOT))
    dc_inputs, synthesis = read(dc / "inputs.json"), read(dc / "summary.json")
    for name, sha in dc_inputs["rtl_sha256"].items():
        assert sha == inputs["files"][name] == digest(ROOT / name)
    assert synthesis["clock_ns"] == dc_inputs["clock_ns"] == 4.0
    assert synthesis["unmapped_cells"] == 0
    area_report = (dc / "area.rpt").read_text()
    reported_area = float(re.search(r"Total cell area:\s*([\d.]+)", area_report)[1])
    assert abs(reported_area - synthesis["area_um2"]) < 0.1
    banks = [float(x) for x in re.findall(r"^cells\[\d\]\.weights\s+([\d.]+)", area_report, re.M)]
    assert len(banks) == 4
    synthesis["weight_banks_area_um2"] = sum(banks)
    synthesis["topk_area_um2"] = float(re.search(r"^topk\s+([\d.]+)", area_report, re.M)[1])
    synthesis["setup_met"] = synthesis["worst_slack_ns"] >= 0
    synthesis["hold_met"] = synthesis["worst_hold_slack_ns"] >= 0
    synthesis["physical_signoff"] = False
    rtl = read(acceptance / "rtl/summary.json")
    negative = read(acceptance / "negative_controls.json")
    online = read(acceptance / "online-api/api_check.json")
    assert rtl["passed"] and all(c["passed"] for c in rtl["cases"])
    assert len(rtl["cases"]) == 24 and any(c["chunk_log2"] == 12 for c in rtl["cases"])
    assert negative["passed"] and online["passed"]
    acceptance_summary = read(acceptance / "summary.json")
    assert acceptance_summary["passed"]
    for name, sha in acceptance_summary["evidence_sha256"].items():
        assert digest(acceptance / name) == sha
    small = read(acceptance / "system/measurements.json")
    assert len(small) == 4 and all(r["passed"] for r in small)
    data = ROOT / "paper/data"
    data.mkdir(exist_ok=True)
    exported = {"measurements.json": records, "system_inputs.json": inputs,
                "synthesis.json": synthesis, "synthesis_inputs.json": dc_inputs,
                "rtl.json": rtl, "system_checks.json": checks,
                "trace_hashes.json": trace_hashes,
                "memory_config.json": read(system / "c32-b1-logic_die/memsim_config.json"),
                "verification.json": {"rtl_cases": len(rtl["cases"]), "rtl_queries": 3 * len(rtl["cases"]),
                    "native_memory_tests": 8, "online_api": online, "negative_controls": negative,
                    "quick_system_cases": 4, "paper_system_cases": 10,
                    "acceptance": acceptance_summary,
                    "synthesis_reports_sha256": {f: digest(dc / f) for f in
                        ("area.rpt", "timing.rpt", "hold.rpt", "constraints.rpt", "logic_die.sdc", "logic_die.ddc")}}}
    for name, value in exported.items():
        (data / name).write_text(json.dumps(value, indent=2, ensure_ascii=False) + "\n")
    print("Exported verified paper data:", data)

if __name__ == "__main__":
    main()
