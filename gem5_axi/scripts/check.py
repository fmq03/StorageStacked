#!/usr/bin/env python3
"""Independent checker of recorded AXI handshakes and gem5/TLM lifetimes.

The supplied workloads separate overlapping reads from writes. Thus a read
scoreboard can use completed write effects without assuming a RAM read snapshot
policy for simultaneous conflicting accesses.
"""
import argparse
import collections
import csv
import json
from pathlib import Path


def read_csv(path):
    with path.open() as f:
        return [{k: v if k in ("channel", "command") else int(v)
                 for k, v in row.items()} for row in csv.DictReader(f)]


def check(directory, latency=3, directed=False):
    directory = Path(directory)
    events = read_csv(directory / "axi_events.csv")
    txns = read_csv(directory / "transactions.csv")
    summary = json.loads((directory / "protocol_summary.json").read_text())
    period = summary["period_ticks"]
    data_bits = summary.get('axi_data_bits', 64)  # historical captures
    assert data_bits in (64, 256)
    data_bytes = data_bits // 8
    assert summary["ticks_per_second"] == 10**15
    assert summary["drained"]
    assert summary["accepted"] == summary["completed"] == len(txns) > 0
    # Wire IDs may be reused after END_RESP. Associate each addressed handshake
    # with exactly one live parent before using IDs as internal scoreboard keys.
    if len({t["id"] for t in txns}) != len(txns):
        original = [dict(t) for t in txns]
        for event in events:
            if event["channel"] == "W":
                continue
            matches = [i for i, t in enumerate(original)
                       if t["id"] == event["id"]
                       and t["accepted_tick"] < event["tick"] <= t["axi_done_tick"]]
            assert len(matches) == 1, "wire ID has no unique live parent"
            event["id"] = matches[0] + 1
        for i, t in enumerate(txns):
            t["id"] = i + 1
    by_channel = {c: [e for e in events if e["channel"] == c]
                  for c in ("AW", "W", "B", "AR", "R")}
    assert all(events[i]["tick"] <= events[i + 1]["tick"]
               for i in range(len(events) - 1))
    for c, es in by_channel.items():
        assert len(es) == summary["channels"][c]["handshakes"]
        assert all(e["tick"] % period == 0 for e in es), "off-edge handshake"
    config = json.loads((directory / 'config.json').read_text())['systemc_kernel']['system']['axi']
    base, size = int(config['base']), int(config['size'])
    narrow = limit256 = masked = aw_leads = w_leads = errors = full_width = 0
    writes, changes = [], []
    write_beats = iter(by_channel["W"])
    previous_effect = -period
    parent_bursts = collections.defaultdict(list)
    parent_done = collections.defaultdict(list)
    b_responses = collections.defaultdict(collections.deque)
    for b in by_channel["B"]:
        b_responses[b["id"]].append(b)

    def geometry(a):
        nonlocal narrow, limit256, full_width
        count = 1 << a["size"]
        beats = a["len"] + 1
        assert count <= data_bytes and 1 <= beats <= 256
        assert a["address"] % count == 0
        assert (a["address"] & 4095) + beats * count <= 4096, "4KiB crossing"
        narrow += count < data_bytes
        full_width += count == data_bytes
        limit256 += beats == 256
        return count, beats

    for a in by_channel["AW"]:
        count, beats = geometry(a)
        consumed, bad = [], False
        for beat in range(beats):
            w = next(write_beats, None)
            assert w is not None, "missing W beat"
            assert w["last"] == (beat == beats - 1), "incorrect WLAST"
            addr = a["address"] + beat * count
            lane = addr % data_bytes
            legal = ((1 << count) - 1) << lane
            assert w["strb"] & ~legal == 0, "WSTRB outside transfer lanes"
            masked += w["strb"] != legal
            effect = max(a["tick"], w["tick"], previous_effect + period)
            previous_effect = effect
            consumed.append(w)
            ok = base <= addr and addr + count <= base + size
            bad |= not ok
            if ok:
                for j in range(count):
                    if w["strb"] & (1 << (lane + j)):
                        changes.append((effect, addr + j,
                                        (w["data"] >> (8 * (lane + j))) & 255))
        aw_leads += a["tick"] < consumed[0]["tick"]
        w_leads += a["tick"] > consumed[0]["tick"]
        assert b_responses[a["id"]], "missing B"
        b = b_responses[a["id"]].popleft()
        assert b["resp"] == (3 if bad else 0), "incorrect BRESP"
        assert b["tick"] >= previous_effect + (latency + 1) * period, "early B"
        errors += bad
        parent_bursts[a["id"]].append(a)
        parent_done[a["id"]].append(b["tick"])
        writes.append((a, consumed, b))
    assert next(write_beats, None) is None, "orphan W"
    assert all(not q for q in b_responses.values()), "orphan B"

    # Match R by RID (allow interleaving between IDs), then compare every valid
    # byte against the write-strobe scoreboard, not the RAM's private memory.
    r_responses = collections.defaultdict(collections.deque)
    for r in by_channel["R"]:
        r_responses[r["id"]].append(r)
    reads = []
    for a in by_channel["AR"]:
        count, beats = geometry(a)
        for beat in range(beats):
            assert r_responses[a["id"]], "missing R"
            r = r_responses[a["id"]].popleft()
            assert r["last"] == (beat == beats - 1), "incorrect RLAST"
            assert r["tick"] >= a["tick"] + (latency + 1) * period, "early R"
            addr = a["address"] + beat * count
            ok = base <= addr and addr + count <= base + size
            assert r["resp"] == (0 if ok else 3), "incorrect RRESP"
            errors += not ok
            reads.append((r["tick"], addr, count, r["data"], ok))
        parent_bursts[a["id"]].append(a)
        parent_done[a["id"]].append(r["tick"])
    assert all(not q for q in r_responses.values()), "orphan R"
    memory = collections.defaultdict(int)
    changes.sort()
    pos = checked_bytes = 0
    for tick, addr, count, value, ok in sorted(reads):
        while pos < len(changes) and changes[pos][0] <= tick:
            _, wa, wd = changes[pos]
            memory[wa] = wd
            pos += 1
        if ok:
            for j in range(count):
                got = (value >> (8 * ((addr % data_bytes) + j))) & 255
                assert got == memory[addr + j], "RDATA mismatch at %#x" % (addr + j)
                checked_bytes += 1

    max_latency = 0
    for t in txns:
        bursts = sorted(parent_bursts[t["id"]], key=lambda a: a["tick"])
        assert len(bursts) == t["segments"]
        cursor = t["address"]
        for a in bursts:
            assert a["address"] == cursor, "burst gap/overlap"
            cursor += (a["len"] + 1) * (1 << a["size"])
            assert a["tick"] > t["accepted_tick"], "AXI before TLM admission"
        assert cursor == t["address"] + t["bytes"]
        assert t["begin_tick"] <= t["accepted_tick"] <= t["axi_done_tick"] <= t["end_resp_tick"]
        assert max(parent_done[t["id"]]) == t["axi_done_tick"]
        max_latency = max(max_latency, t["end_resp_tick"] - t["begin_tick"])
    coverage = dict(narrow_bursts=narrow, full_width_bursts=full_width, bursts_256_beats=limit256,
                    masked_write_beats=masked, aw_before_w=aw_leads,
                    w_before_aw=w_leads, error_responses=errors,
                    checked_read_bytes=checked_bytes)
    if directed:
        tester = json.loads((directory / "tester_summary.json").read_text())
        packets = read_csv(directory / "packet_lifecycle.csv")
        assert tester["passed"] and tester["requests"] == tester["responses"] == 17
        assert len(packets) == len(txns) == 17
        assert tester["request_retries"] > 0
        by_serial = {p["serial"]: p for p in packets}
        for t in txns:
            p = by_serial[t["substream"] - 100]
            assert t["stream"] == 7
            assert (t["address"], t["bytes"], t["command"]) == (p["address"], p["bytes"], p["command"])
            assert t["begin_tick"] == p["accepted_tick"] + 250000, "lost/doubled headerDelay"
            assert t["accepted_tick"] > t["begin_tick"] + (500000 if p["command"] == "W" else 0)
            assert t["end_resp_tick"] == p["response_tick"], "END_RESP before gem5 accepts response"
            assert (t["status"] != 1) == bool(p["error"])
            assert p["first_attempt_tick"] <= p["accepted_tick"]
            intervals = [p["accepted_tick"] - p["first_attempt_tick"],
                         t["begin_tick"] - p["accepted_tick"],
                         t["accepted_tick"] - t["begin_tick"],
                         t["axi_done_tick"] - t["accepted_tick"],
                         p["response_tick"] - t["axi_done_tick"]]
            assert sum(intervals) == p["response_tick"] - p["first_attempt_tick"]
        assert narrow > 0 and limit256 > 0 and masked > 0 and errors == 2
        assert full_width > 0
        split_boundary = any(
            any(a["address"] == base + 4096 for a in bs)
            and any(a["address"] < base + 4096 for a in bs)
            for bs in parent_bursts.values())
        assert split_boundary, "4KiB split not exercised"
    result = dict(passed=True, axi_data_bits=data_bits, transactions=len(txns), axi_handshakes=len(events),
                  max_tlm_latency_ns=max_latency / 10**6, coverage=coverage)
    (directory / "check_summary.json").write_text(json.dumps(result, indent=2) + "\n")
    return result


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("directory", type=Path)
    parser.add_argument("--latency", type=int, default=3)
    parser.add_argument("--directed", action="store_true")
    args = parser.parse_args()
    print(json.dumps(check(args.directory, args.latency, args.directed), indent=2))
