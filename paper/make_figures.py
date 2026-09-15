#!/usr/bin/env python3
"""Publication figures; every numerical value comes from paper/data/*.json."""
import argparse
import json
from pathlib import Path
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
from matplotlib.patches import FancyBboxPatch, FancyArrowPatch

BASE = Path(__file__).resolve().parent
FIG = BASE / "figures"
BLUE, ORANGE, GREEN, GRAY = "#245b84", "#c56b25", "#328277", "#727c87"
plt.rcParams.update({"font.family": "DejaVu Sans", "font.size": 8,
    "axes.labelsize": 8, "axes.titlesize": 9, "legend.fontsize": 7,
    "xtick.labelsize": 7, "ytick.labelsize": 7,
    "axes.spines.top": False, "axes.spines.right": False,
    "pdf.fonttype": 42, "ps.fonttype": 42, "savefig.dpi": 300})

def save(fig, name):
    FIG.mkdir(exist_ok=True)
    for ext in ("pdf", "svg", "png"):
        fig.savefig(FIG / f"{name}.{ext}", bbox_inches="tight", pad_inches=0.04)
    plt.close(fig)

def architecture():
    fig = plt.figure(figsize=(7.15, 2.95)); ax = fig.add_axes([0, 0, 1, 1])
    ax.set(xlim=(0, 14), ylim=(0, 5.7)); ax.axis("off")
    def box(x, y, w, h, text, color, fill="white", size=7.5):
        ax.add_patch(FancyBboxPatch((x, y), w, h, boxstyle="round,pad=0.05,rounding_size=0.1",
                                  edgecolor=color, facecolor=fill, linewidth=1.2))
        ax.text(x+w/2, y+h/2, text, ha="center", va="center", fontsize=size, color="#1c2935")
    def arrow(start, end, color=GRAY, both=False):
        ax.add_patch(FancyArrowPatch(start, end, arrowstyle="<->" if both else "-|>",
                                    mutation_scale=9, color=color, linewidth=1.1))
    box(0.1, 3.15, 2, 1.2, "VORTEX SimX\nRV32IMF kernel\n2 ns clock", BLUE, "#edf4fa")
    box(2.65, 3.15, 2, 1.2, "AXI256\nAXI2Flit / UCIe\n250 B FDI", BLUE, "#edf4fa")
    box(5.1, .5, 8.65, 4.95, "", ORANGE, "#fffaf5")
    ax.text(9.4, 5.13, "MEMORY LOGIC DIE  |  4 ns clock", color=ORANGE, ha="center", weight="bold")
    box(5.4, 3.15, 2.1, 1.2, "Descriptor CSR\nEpoch check\nCompletion window", ORANGE)
    box(8.05, 3.15, 2.25, 1.2, "BF16 Kmean banks\n4 × 1024 × 16 bit\n8 KiB resident", ORANGE)
    box(10.85, 3.15, 2.6, 1.2, "4 score lanes\nFP32 accumulation\n32-bit radix Top-K", ORANGE)
    box(8.05, 1.15, 2.25, 1.2, "Local DMA + pooling\n32 B / request\nFP32 → BF16 RNE", GREEN)
    box(5.4, 1.15, 2.1, 1.2, "Host / DMA arbiter\n4 host jobs\n1 local request", GREEN)
    box(10.85, 1.15, 2.6, 1.2, "Query buffer\n128 × BF16\n256 B", BLUE)
    box(.5, 1.15, 3.8, 1.2, "Stacked DRAM model\n8 channels, 32 B transactions\n250,000 fs / native tick", GREEN, "#edf7f5")
    arrow((2.15, 3.75), (2.6, 3.75), BLUE, True)
    arrow((4.7, 3.75), (5.35, 3.75), BLUE, True)
    arrow((7.55, 3.75), (8, 3.75), ORANGE)
    arrow((10.35, 3.75), (10.8, 3.75), ORANGE)
    arrow((9.17, 2.4), (9.17, 3.1), GREEN)
    ax.text(9.32, 2.69, "Kmean", color=GREEN, fontsize=6.5)
    arrow((8, 1.75), (7.55, 1.75), GREEN, True)
    arrow((6.45, 3.1), (6.45, 2.4), GREEN, True)
    arrow((4.35, 1.75), (5.35, 1.75), GREEN, True)
    arrow((10.35, 1.75), (10.8, 1.75), BLUE)
    arrow((12.15, 2.4), (12.15, 3.1), BLUE)
    ax.plot([12.15, 12.15, 6.45], [4.4, 4.72, 4.72], color=ORANGE, lw=1.1)
    arrow((6.45, 4.72), (6.45, 4.4), ORANGE)
    ax.text(9.4, 4.76, "Result mask and ordered indices", ha="center", fontsize=6.5, color=ORANGE)
    ax.text(2.35, 2.65, "Real instruction and data requests", ha="center", color=BLUE, fontsize=7)
    ax.text(7, .08, "One SystemC kernel  |  1 fs time resolution  |  Real memory completions", ha="center", color=GRAY, fontsize=7)
    save(fig, "architecture")

def numeric():
    records = json.loads((BASE / "data/measurements.json").read_text())
    synthesis = json.loads((BASE / "data/synthesis.json").read_text())
    verification = json.loads((BASE / "data/verification.json").read_text())
    assert len(records) == 10 and all(r["passed"] for r in records)
    gate = sorted((r for r in records if r["mode"] == "logic_die"), key=lambda r:r["chunk_log2"])
    soft = sorted((r for r in records if r["mode"] == "vortex_software"), key=lambda r:r["chunk_log2"])
    b = np.array([1 << r["chunk_log2"] for r in gate]); x = np.arange(len(b))
    host = lambda rr: np.array([r["vortex_read_bytes"] + r["vortex_write_bytes"] for r in rr]) / 1024
    hg, hs = host(gate), host(soft)
    dma = np.array([r["local_dma_bytes"] for r in gate]) / 1024
    fig, axs = plt.subplots(1, 2, figsize=(7.15, 2.55), layout="constrained")
    axs[0].bar(x-.17, hs, .34, label="VORTEX software", color=BLUE)
    axs[0].bar(x+.17, hg, .34, label="Logic Die offload", color=ORANGE)
    axs[0].set(ylabel="VORTEX request span (KiB)", title="(a) Traffic crossing the host interface")
    axs[0].legend(frameon=False, loc="upper left")
    axs[1].bar(x, dma-1, .56, label="K: one PREPARE", color=GREEN)
    axs[1].bar(x, np.ones(len(x)), .56, bottom=dma-1, label="Q: four QUERY commands", color="#f1b564")
    axs[1].set(ylabel="Local DMA read payload (KiB)", title="(b) Memory-side traffic")
    axs[1].legend(frameon=False, loc="upper left")
    for ax in axs:
        ax.set_xticks(x, b); ax.set_xlabel("Tokens per block B"); ax.grid(axis="y", alpha=.18); ax.set_axisbelow(True)
    save(fig, "traffic")

    prep = np.array([r["prepare_cycles"] * .004 for r in gate])
    query = np.array([sum(q["cycles"] for q in r["query_results"]) * .004 for r in gate])
    total = np.array([r["elapsed_fs"] / 1e9 for r in gate]); other = total-prep-query
    assert np.all(other >= 0)
    fig, ax = plt.subplots(figsize=(3.45, 2.5), layout="constrained")
    ax.bar(x, prep, .58, label="PREPARE", color=GREEN)
    ax.bar(x, query, .58, bottom=prep, label="4 × QUERY", color=ORANGE)
    ax.bar(x, other, .58, bottom=prep+query, label="Control / transfer remainder", color="#bbc3cb")
    ax.set_xticks(x, b); ax.set(xlabel="Tokens per block B", ylabel="Kernel interval (µs)")
    ax.legend(frameon=False, loc="upper left"); ax.grid(axis="y", alpha=.18); ax.set_axisbelow(True)
    save(fig, "latency")

    weights = synthesis["weight_banks_area_um2"]
    topk = synthesis["topk_area_um2"]
    remainder = synthesis["area_um2"]-weights-topk
    fig, ax = plt.subplots(figsize=(3.45, 2.0), layout="constrained")
    labels = ["Weight banks and selection logic", "Radix Top-K", "Other control and arithmetic"]
    values = np.array([weights, topk, remainder]) / 1e6
    left = 0
    for value, label, color in zip(values, labels, [ORANGE, BLUE, GRAY]):
        ax.barh(0, value, left=left, color=color, height=.45, label=label)
        left += value
    ax.set_yticks([]); ax.set_ylim(-.4, 1.8)
    ax.legend(frameon=False, loc="upper left", fontsize=7)
    ax.set_xlabel("Mapped standard-cell area (mm²)")
    ax.grid(axis="x", alpha=.18); ax.set_axisbelow(True)
    save(fig, "area")

    macros = {"GateMinUs": f"{total.min():.3f}", "GateMaxUs": f"{total.max():.3f}",
        "HostGateKiB": f"{hg[0]:.2f}", "HostSoftMinKiB": f"{hs.min():.2f}",
        "HostSoftMaxKiB": f"{hs.max():.2f}", "HostReductionMin": f"{100*(1-hg/hs).min():.1f}",
        "HostReductionMax": f"{100*(1-hg/hs).max():.1f}",
        "QueryMinUs": f"{min(q['cycles']*.004 for r in gate for q in r['query_results']):.3f}",
        "QueryMaxUs": f"{max(q['cycles']*.004 for r in gate for q in r['query_results']):.3f}",
        "AreaMM": f"{synthesis['area_um2']/1e6:.6f}",
        "BankAreaPct": f"{100*weights/synthesis['area_um2']:.1f}",
        "SetupSlackPs": f"{1000*synthesis['worst_slack_ns']:.3f}",
        "HoldSlackPs": f"{1000*synthesis['worst_hold_slack_ns']:.3f}",
        "LeafCells": str(synthesis["leaf_cells"]), "RtlCases": str(verification["rtl_cases"]),
        "RtlQueries": str(verification["rtl_queries"])}
    out = BASE / "generated"; out.mkdir(exist_ok=True)
    (out / "numbers.tex").write_text("% Auto-generated from verified data.\n" + "".join(
        f"\\newcommand{{\\{k}}}{{{v}}}\n" for k,v in macros.items()))
    rows = [f"{bi} & {g['elapsed_fs']/1e9:.3f} & {s['elapsed_fs']/1e9:.3f} & "
            f"{g['prepare_cycles']*.004:.3f} & {host([s])[0]:.2f} \\\\\n"
            for bi,g,s in zip(b,gate,soft)]
    header = (r"\begin{tabular}{@{}rrrrr@{}}" + "\n" + r"\toprule" + "\n"
              + r"$B$ & 门控/$\mu$s & 软件/$\mu$s & 准备/$\mu$s & 软件请求/KiB\\\midrule" + "\n")
    (out / "measurements_table.tex").write_text(header + "".join(rows) + r"\bottomrule" + "\n" + r"\end{tabular}" + "\n")

def main():
    p = argparse.ArgumentParser(); p.add_argument("--architecture-only", action="store_true"); a = p.parse_args()
    architecture()
    if not a.architecture_only: numeric()

if __name__ == "__main__": main()
