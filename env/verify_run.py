#!/usr/bin/env python3
"""Check HETTrace against AXI traffic and downstream latency feedback."""
import csv
import json
from pathlib import Path
import re
import sys

from hettrace.reader import CHAN_R, CHAN_W, read_header, read_records
from hettrace.validate import format_report, validate_dir

root = Path(sys.argv[1])


def rows(path):
    with path.open() as f:
        return list(csv.DictReader(f))


def finish(directory):
    return int(re.findall(r"EXIT: .* tick (\d+)", (directory / "run.log").read_text())[-1])


trace_checks = {}
for directory in sorted((root / "aou").iterdir()) + sorted((root / "feedback").iterdir()):
    if not (directory / "hettrace").is_dir():
        continue
    trace = directory / "hettrace"
    issues, summaries = validate_dir(str(trace), require_heterogeneous=False,
                                    ticks_per_second=10**15)
    (trace / "validation.txt").write_text(format_report(issues, summaries) + "\n")
    assert not [i for i in issues if i.level == "ERROR"], issues
    header = read_header(str(trace / "host.hettrace"))
    assert header.ticks_per_second == 10**15 and header.clock_period_ticks == 500000
    records = list(read_records(str(trace / "host.hettrace")))
    transactions = rows(directory / "transactions.csv")
    for channel, command in ((CHAN_W, "W"), (CHAN_R, "R")):
        recorded = sum(r.size for r in records if r.chan == channel)
        actual = sum(int(t["bytes"]) for t in transactions if t["command"] == command)
        assert recorded == actual, (directory.name, command, recorded, actual)
    assert records and max(r.tick for r in records) <= finish(directory)
    synthetic_ids = {r.axi_id for r in records}
    if directory.name == "id_wrap":
        assert len(synthetic_ids) == 256 and max(synthetic_ids) == 255, "Synthetic ID reuse was not exercised"
    trace_checks[str(directory.relative_to(root))] = {
        "passed": True, "records": len(records), "transactions": len(transactions),
        "ticks_per_second": header.ticks_per_second,
        "unique_synthetic_ids": len(synthetic_ids),
    }
assert len(trace_checks) == 8, "Expected six AoU traces and two CPU feedback traces"

observed, control = root / "aou/directed", root / "observer_off"
compared = ("axi_events.csv", "aou_events.csv", "ucie_soc.csv", "ucie_mem.csv")
for name in compared:
    assert (observed / name).read_bytes() == (control / name).read_bytes(), name
assert finish(observed) == finish(control)

slow, fast = root / "feedback/cpu_l9", root / "feedback/cpu_l3"
slow_tx, fast_tx = rows(slow / "transactions.csv"), rows(fast / "transactions.csv")
assert len(slow_tx) == len(fast_tx) == 452
deltas = []
for a, b in zip(fast_tx, slow_tx):
    assert all(a[k] == b[k] for k in ("command", "address", "bytes", "status"))
    assert a["status"] == b["status"] == "1"
    deltas.append((int(b["axi_done_tick"]) - int(b["accepted_tick"])) -
                  (int(a["axi_done_tick"]) - int(a["accepted_tick"])))
assert min(deltas) > 0, "Downstream latency did not delay every target access"
cpu_delta = finish(slow) - finish(fast)
assert cpu_delta > 0 and cpu_delta == sum(deltas), (cpu_delta, sum(deltas))
for directory in (fast, slow):
    assert "CPU AXI PASS bytes=192 checksum=24416 boundary64=ok" in (directory / "run.log").read_text()
    protocol = json.loads((directory / "protocol_summary.json").read_text())
    assert protocol["drained"] and protocol["max_outstanding"] == 1


def instructions(directory):
    return int(re.search(r"^simInsts\s+(\d+)", (directory / "stats.txt").read_text(), re.M)[1])


assert instructions(fast) == instructions(slow), "CPU instruction counts differ"

result = {
    "passed": True, "scope": "gem5_new CPU/monitor -> AXI2Flit -> UCIe -> SimpleBurstMemory",
    "aou": json.loads((root / "aou/summary.json").read_text()),
    "hettrace": trace_checks,
    "observer_transparent": {"passed": True, "identical_files": compared},
    "cpu_aou_feedback": {
        "passed": True, "transactions": len(deltas), "cpu_finish_delta_ns": cpu_delta / 1e6,
        "simulated_instructions": instructions(fast),
        "transaction_delta_min_ns": min(deltas) / 1e6,
        "transaction_delta_max_ns": max(deltas) / 1e6,
    },
    "not_exercised_by_this_suite": ["Vortex", "CoralNPU", "mem_sim"],
}
(root / "summary.json").write_text(json.dumps(result, indent=2) + "\n")
print(json.dumps(result, indent=2))
