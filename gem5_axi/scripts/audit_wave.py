#!/usr/bin/env python3
"""Audit native SystemC VCD independently of the C++ monitor.

This waveform records post-update values at each physical timestamp. AXI
handshakes sample the values immediately before a rising edge. Delta events
collapsed by VCD cannot be reconstructed; this is not a full AXI VIP.
"""
import argparse
import collections
import csv
import json
from pathlib import Path
from xml.sax.saxutils import escape

CHANNELS = {
    "AW": ("awaddr", "awid", "awlen", "awsize", "awburst"),
    "W": ("wdata", "wstrb", "wlast"),
    "B": ("bid", "bresp"),
    "AR": ("araddr", "arid", "arlen", "arsize", "arburst"),
    "R": ("rid", "rdata", "rresp", "rlast"),
}
FIELDS = "tick cycle channel id address len size data strb last resp".split()
WINDOW = 48000000


def vcd_groups(path):
    names, changes, tick = {}, {}, 0
    scopes = []
    with path.open() as f:
        for line in f:
            words = line.split()
            if not words:
                continue
            if words[0] == "$scope":
                scopes.append(words[2])
            elif words[0] == "$upscope":
                scopes.pop()
            elif words[0] == "$var":
                scope = scopes[1:] if scopes and scopes[0] == "SystemC" else scopes
                names[words[3]] = ".".join(scope + [words[4]])
            elif line.startswith("#"):
                yield tick, changes
                tick, changes = int(line[1:]), {}
            elif words[0][0] in "bB" and len(words) == 2 and words[1] in names:
                bits = words[0][1:]
                changes[names[words[1]]] = int(bits, 2) if set(bits) <= {"0", "1"} else None
            elif words[0][0] in "01xXzZ" and words[0][1:] in names:
                bit = words[0][0]
                changes[names[words[0][1:]]] = int(bit) if bit in "01" else None
        yield tick, changes


def row(tick, cycle, channel, s):
    r = dict.fromkeys(FIELDS, 0)
    r.update(tick=tick, cycle=cycle, channel=channel)
    p = channel.lower()
    if channel in ("AW", "AR"):
        r.update(id=s[p+"id"], address=s[p+"addr"], len=s[p+"len"], size=s[p+"size"])
        assert s[p+"burst"] == 1
    elif channel == "W":
        r.update(data=s["wdata"], strb=s["wstrb"], last=s["wlast"])
    elif channel == "B":
        r.update(id=s["bid"], resp=s["bresp"])
    else:
        r.update(id=s["rid"], data=s["rdata"], resp=s["rresp"], last=s["rlast"])
    return r


def audit(directory, output):
    state, actual, cycle = {}, [], 0
    held = collections.Counter()
    changes_count = collections.Counter()
    points = collections.defaultdict(list)
    first_valid = {}
    edge_rows = []
    writes, beats = collections.deque(), collections.deque()
    completed = collections.defaultdict(collections.deque)
    reads = collections.defaultdict(collections.deque)
    fields = sorted({n for c, fs in CHANNELS.items()
                     for n in fs + (c.lower()+"valid", c.lower()+"ready")})
    for tick, changes in vcd_groups(directory / "axi_wave.vcd"):
        before = state.copy()
        state.update(changes)
        edge = state.get("ACLK") == 1 and before.get("ACLK") != 1
        if edge:
            cycle += 1
        for name, value in changes.items():
            if tick <= WINDOW:
                points[name].append((tick, value))
            if name in fields and value != before.get(name):
                changes_count[name] += 1
                assert tick == 0 or edge, ("off-edge output transition", tick, name)
                if name.endswith("valid") and value == 1:
                    first_valid.setdefault(name, tick)
        if not before.get("ARESETn"):
            continue
        for channel, payload in CHANNELS.items():
            p = channel.lower()
            v, r = before[p+"valid"], before[p+"ready"]
            assert v in (0, 1) and r in (0, 1), ("unknown handshake value", tick, channel)
            if v and (not edge or not r):
                assert state[p+"valid"] == 1, ("VALID withdrawn", tick, channel)
                assert all(before[n] == state[n] for n in payload), ("unstable payload", tick, channel)
            if edge and v and not r:
                held[channel] += 1
        if edge:
            transferred = {}
            for channel in CHANNELS:
                p = channel.lower()
                if before[p+"valid"] and before[p+"ready"]:
                    r = row(tick, cycle, channel, before)
                    actual.append(r)
                    transferred[channel] = r
            if "AW" in transferred:
                a = transferred["AW"]
                writes.append(dict(id=a["id"], remaining=a["len"]+1))
            if "W" in transferred:
                beats.append(transferred["W"])
            while writes and beats:
                a, w = writes[0], beats.popleft()
                assert w["last"] == (a["remaining"] == 1), ("WLAST", tick)
                a["remaining"] -= 1
                if not a["remaining"]:
                    completed[a["id"]].append(tick)
                    writes.popleft()
            if "B" in transferred:
                b = transferred["B"]
                assert completed[b["id"]], ("B before AW/all W", tick, b["id"])
                completed[b["id"]].popleft()
            if "AR" in transferred:
                a = transferred["AR"]
                reads[a["id"]].append(a["len"]+1)
            if "R" in transferred:
                r = transferred["R"]
                assert reads[r["id"]], ("R before AR", tick)
                n = reads[r["id"]][0]
                assert r["last"] == (n == 1), ("RLAST", tick)
                if n == 1:
                    reads[r["id"]].popleft()
                else:
                    reads[r["id"]][0] -= 1
            if tick <= WINDOW:
                e = dict(tick=tick, handshakes=" ".join(transferred))
                for n in ("awvalid", "awready", "wvalid", "wready", "wlast", "bvalid", "bready", "wdata"):
                    e[n+"_before"] = before[n]
                    e[n+"_after"] = state[n]
                edge_rows.append(e)
        # Check response VALID itself, not merely a later successful handshake.
        if state["bvalid"]:
            assert completed[state["bid"]], ("early BVALID", tick, state["bid"])
        if state["rvalid"]:
            assert reads[state["rid"]], ("early RVALID", tick, state["rid"])
    assert not writes and not beats
    assert not any(completed.values()) and not any(reads.values())
    with (directory / "axi_events.csv").open() as f:
        expected = [{k: v if k == "channel" else int(v) for k, v in r.items()}
                    for r in csv.DictReader(f)]
    assert actual == expected, "VCD handshakes differ from C++ monitor CSV"
    summary = json.loads((directory / "protocol_summary.json").read_text())
    counts = collections.Counter(r["channel"] for r in actual)
    for c in CHANNELS:
        assert summary["channels"][c]["handshakes"] == counts[c]
        assert summary["channels"][c]["stall_cycles"] == held[c]
    output.mkdir(parents=True, exist_ok=True)
    for name, rows in (("handshakes_from_vcd.csv", actual), ("first_48ns_edges.csv", edge_rows)):
        with (output / name).open("w", newline="") as f:
            writer = csv.DictWriter(f, fieldnames=list(rows[0]))
            writer.writeheader()
            writer.writerows(rows)
    result = dict(passed=True, source=str(directory), sampled_rising_edges=cycle,
                  handshakes=dict(counts), stalled_edges=dict(held),
                  first_valid_ns={k: v/1e6 for k, v in first_valid.items()},
                  ready_transitions={n: changes_count[n] for n in fields if n.endswith("ready")},
                  first_handshake_ns={c: next(r["tick"]/1e6 for r in actual if r["channel"] == c)
                                      for c in CHANNELS},
                  checked=["VCD/CSV equality", "VALID/payload hold", "clocked output transitions",
                           "BVALID after AW/all W", "RVALID after AR", "WLAST/RLAST", "drained"])
    (output / "summary.json").write_text(json.dumps(result, indent=2)+"\n")
    return result, points, actual


def plot(panels, output):
    """Small engineering timing diagram, drawn from actual VCD transitions."""
    signals = ("ACLK", "ARESETn", "awvalid", "awready", "wvalid", "wready", "wlast", "bvalid", "bready")
    panel_h, width, left, scale = 410, 1260, 130, 21
    height = 70 + panel_h * len(panels)
    svg = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" viewBox="0 0 {width} {height}">',
           '<rect width="100%" height="100%" fill="white"/>',
           '<style>text{font-family:monospace;font-size:13px}.title{font-size:18px;font-weight:bold}</style>',
           '<text x="20" y="26" class="title">Actual VCD: stress enabled vs --no-stalls (all other parameters identical)</text>',
           '<text x="20" y="49">Lines: post-update values. Orange circles: handshakes sampling values BEFORE each rising edge.</text>']
    for index, (label, points, handshakes) in enumerate(panels):
        top = 80 + index * panel_h
        svg.append(f'<text x="20" y="{top}" class="title">{escape(label)}</text>')
        for ns in range(0, 49, 2):
            x = left+ns*scale
            svg.append(f'<path d="M{x},{top+15}V{top+355}" stroke="#e2e8f0"/>')
            svg.append(f'<text x="{x-5}" y="{top+378}">{ns}</text>')
        svg.append(f'<text x="{left+49*scale}" y="{top+378}">ns</text>')
        for i, signal in enumerate(signals):
            low = top+44+i*35
            high = low-18
            svg.append(f'<text x="15" y="{low-4}">{signal}</text>')
            values = points[signal]
            prev = values[0][1]
            segments = [f'M{left},{high if prev else low}']
            for tick, value in values[1:]:
                x = left+tick/1e6*scale
                segments += [f'H{x}', f'V{high if value else low}']
            segments.append(f'H{left+48*scale}')
            color = "#185a9d" if signal.endswith("valid") else "#278466"
            svg.append(f'<path d="{" ".join(segments)}" fill="none" stroke="{color}" stroke-width="1.6"/>')
            channel = {"awvalid": "AW", "wvalid": "W", "bvalid": "B"}.get(signal)
            if channel:
                for r in handshakes:
                    if r["channel"] == channel and r["tick"] <= WINDOW:
                        x = left+r["tick"]/1e6*scale
                        svg.append(f'<circle cx="{x}" cy="{high}" r="3.3" fill="#e27b16"><title>{channel} handshake at {r["tick"]/1e6} ns</title></circle>')
    svg.append("</svg>")
    output.write_text("\n".join(svg))


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("directories", nargs="+", type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    summaries, panels = {}, []
    for directory in args.directories:
        summary, points, handshakes = audit(directory, args.output / directory.name)
        summaries[directory.name] = summary
        if directory.name in ("directed", "wave_clean"):
            panels.append((directory.name, points, handshakes))
        print(directory.name, "PASS", sum(summary["handshakes"].values()), "handshakes")
    (args.output / "summary.json").write_text(json.dumps(summaries, indent=2)+"\n")
    if len(panels) == 2:
        plot(panels, args.output / "comparison.svg")
