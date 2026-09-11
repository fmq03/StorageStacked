#!/usr/bin/env python3
"""Check that AXI RAM latency feeds back into actual gem5 CPU execution."""
import json
from pathlib import Path
import sys
from check import read_csv


def stats(path):
    result = {}
    for line in path.read_text().splitlines():
        words = line.split()
        if words and words[0] in ("simTicks", "simInsts"):
            result[words[0]] = int(words[1])
    return result


def compare(root):
    cases = {}
    for latency in (3, 9):
        directory = root / ("cpu_l%d" % latency)
        protocol = json.loads((directory / "protocol_summary.json").read_text())
        transactions = read_csv(directory / "transactions.csv")
        s = stats(directory / "stats.txt")
        assert "CPU AXI PASS bytes=192 checksum=24416 boundary64=ok" in (directory / "run.log").read_text()
        assert protocol["accepted"] == protocol["completed"] == len(transactions) == 452
        assert protocol["drained"] and protocol["max_outstanding"] == 1
        assert all(c["stall_cycles"] == 0 for c in protocol["channels"].values())
        assert sum(t["command"] == "W" for t in transactions) == 258
        assert sum(t["command"] == "R" for t in transactions) == 194
        assert all(t["segments"] == 1 and t["status"] == 1 for t in transactions)
        cases[latency] = dict(stats=s, protocol=protocol, transactions=transactions)
    a, b = cases[3], cases[9]
    assert a["stats"]["simInsts"] == b["stats"]["simInsts"]
    assert a["protocol"]["period_ticks"] == b["protocol"]["period_ticks"] == 2000000
    extra = (9 - 3) * a["protocol"]["period_ticks"]
    for x, y in zip(a["transactions"], b["transactions"]):
        assert all(x[k] == y[k] for k in ("id", "command", "address", "bytes", "segments", "status"))
        measured = (y["axi_done_tick"] - y["accepted_tick"]) - (x["axi_done_tick"] - x["accepted_tick"])
        assert measured == extra, ("AXI latency mismatch", x["id"], measured, extra)
    # This single-outstanding, uncached workload has no overlap between target
    # accesses. An extra 12 ns per access must delay its exit by 452 * 12 ns.
    delta = b["stats"]["simTicks"] - a["stats"]["simTicks"]
    assert delta == 452 * extra, ("CPU timing feedback mismatch", delta, 452 * extra)
    result = dict(passed=True, transactions_per_run=452, writes=258, reads=194,
                  simulated_instructions=a["stats"]["simInsts"],
                  latency_3_finish_ns=a["stats"]["simTicks"]/1e6,
                  latency_9_finish_ns=b["stats"]["simTicks"]/1e6,
                  per_transaction_delta_ns=extra/1e6,
                  cpu_finish_delta_ns=delta/1e6,
                  expected_cpu_finish_delta_ns=452*extra/1e6,
                  scope="X86TimingSimpleCPU, uncached target, no artificial AXI stalls")
    (root / "comparison.json").write_text(json.dumps(result, indent=2)+"\n")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    compare(Path(sys.argv[1]))
