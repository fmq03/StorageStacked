#!/usr/bin/env python3
"""Run auditable, workload-directed HBM/LPDDR architecture sweeps."""

from __future__ import annotations

import argparse
import csv
import html
import re
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
from result_io import count, number, read_result


ORG_KEYS = (
    "channels", "pseudo_channels", "sids", "ranks", "bank_groups",
    "banks_per_group", "rows", "columns", "line_size", "dram_transaction_bytes",
    "stack_height",
)

# 只作为 smoke 的 fixture：smoke 的 heredoc 没有二进制可跑基线预扫。
# 正式扫描永远使用从 resolved config 解析出的基线，并用
# "builtin org fixture" 检查强制本表与解析结果逐字段一致。
STANDARD_ORGS = {
    "hbm3": {"channels": 16, "pseudo_channels": 2, "sids": 2, "ranks": 1,
             "bank_groups": 4, "banks_per_group": 4, "rows": 16384, "columns": 32,
             "line_size": 64, "dram_transaction_bytes": 32, "stack_height": 8},
    "hbm4": {"channels": 32, "pseudo_channels": 2, "sids": 2, "ranks": 1,
             "bank_groups": 2, "banks_per_group": 8, "rows": 16384, "columns": 32,
             "line_size": 64, "dram_transaction_bytes": 32, "stack_height": 8},
    "lpddr5": {"channels": 1, "pseudo_channels": 1, "sids": 1, "ranks": 1,
               "bank_groups": 4, "banks_per_group": 4, "rows": 65536, "columns": 64,
               "line_size": 64, "dram_transaction_bytes": 32, "stack_height": 0},
    "lpddr6": {"channels": 1, "pseudo_channels": 2, "sids": 1, "ranks": 1,
               "bank_groups": 4, "banks_per_group": 4, "rows": 65536, "columns": 64,
               "line_size": 64, "dram_transaction_bytes": 32, "stack_height": 0},
}

# 密度组按各标准的真实规格取点。真实器件变密度时不变的是 channels、
# banks_per_group、columns；变的是 rows，以及（HBM4 的堆叠配置）stack_height
# 所带动的 SID 域数量。取值来自 JEDEC 参考规格，不是从基线推的倍率。
#
# HBM3（16ch × 2pc × SID+BA[3:0]=32 banks/PC，恒 4 banks/BG）：
#   8Gb/8Hi RA[12:0]=8192、16Gb/8Hi RA[13:0]=16384、32Gb/8Hi RA[14:0]=32768。
#   三档 bank 组织完全相同，只有 rows 变。
# HBM4（32ch × 2pc × 8 banks/BG）：
#   32Gb/8Hi  2 SID × 2 BG × 8 = 32 banks/PC，rows 16384
#   24Gb/12Hi 3 SID × 2 BG × 8 = 48 banks/PC，rows 12288（RA[13:12]=11 非法）
#   32Gb/16Hi 4 SID × 2 BG × 8 = 64 banks/PC，rows 16384
# LPDDR6（每 SC 恒 4 BG × 4 = 16 banks、page 2 KB）：
#   8Gb/SC rows 32768、16Gb/SC rows 65536、24Gb/SC rows 98304（= 2^16 + 2^15）
# LPDDR5（恒 4 BG × 4 = 16 banks）：
#   12Gb x16 rows 49152（= 2^15 + 2^14）、16Gb x16 rows 65536
#   16Gb x8  rows 131072 且 columns 32（同密度换宽度：页 1024 B、x8 DQ）
# 组织扰动组：在标准组织基线上把**单个**组织键放大一倍，用于观察组织形状本身
# （而不是密度档）带来的影响。三项各自的容量都是基线的 2 倍，因此彼此可直接比较，
# 差别只在"这 2 倍容量是加在 banks_per_group、bank_groups 还是 columns 上"。
#
# 这三个键在真实器件里都是随密度档固定、不单独变化的（见 DENSITY_VARIANTS 的说明），
# 所以本组是**有意偏离 JEDEC 的研究型扰动**，不是器件规格，读结果时不能当成
# "某款 HBM4 器件"。
#
# 本组与 workload 解耦：请求数与基线对照组相同，只让组织变。因此 bank 数变大时
# 覆盖率必然下降（固定请求数下新增 bank 到不了）。脚本因此只对本组做容量核算和
# 单键漂移校验，**不给带宽趋势结论**——原因见 evaluate()。
ORGANIZATION_VARIANTS = (
    ("bpg_double", "banks_per_group"),
    ("bg_double", "bank_groups"),
    ("cols_double", "columns"),
)

# 混合偏移 case：一次性挪动 banks_per_group / bank_groups / rows / columns 四个键，
# 有增有减，用来观察模型在"多个维度同时偏离且方向不一致"时是否仍然自洽。
#
# 取值由固定种子 20260915 的伪随机序列生成后**冻结成表**，不在运行时随机——
# 否则实验不可复现，resolved.cfg 也无法作为审计依据。生成时施加三条约束：
#   1) bank_groups 取偶数且 >= 2（LPDDR REFdb 相邻 BG 配对校验要求，对所有标准统一）；
#   2) 至少一个键变小、至少一个键变大，保证确实是"有增有减"；
#   3) 容量比落在 [0.4, 4]，避免退化成容量实验而不是组织实验。
# 本组同样是**研究型扰动，不是器件规格**。
MIXED_SHIFT = {
    "hbm3": {"banks_per_group": 2, "bank_groups": 12, "rows": 32768, "columns": 24},
    "hbm4": {"banks_per_group": 6, "bank_groups": 4, "rows": 8192, "columns": 64},
    "lpddr5": {"banks_per_group": 2, "bank_groups": 12, "rows": 131072, "columns": 48},
    "lpddr6": {"banks_per_group": 2, "bank_groups": 8, "rows": 196608, "columns": 48},
}

DENSITY_VARIANTS = {
    "hbm3": [
        ("8gb_8h", {"rows": 8192}),
        ("16gb_8h", {}),
        ("32gb_8h", {"rows": 32768}),
    ],
    # sids 显式写出而不是依赖引擎从 stack_height 自动推导（12→3、16→4）：
    # 隐式推导会让 trace 生成器按旧 sids 编码地址、被新 sids 的引擎解码，
    # 落点与预期不符；显式声明后容量公式与 organization drift 才能验收它。
    "hbm4": [
        ("32gb_8h", {}),
        ("24gb_12h", {"stack_height": 12, "sids": 3, "rows": 12288}),
        ("32gb_16h", {"stack_height": 16, "sids": 4}),
    ],
    # 只用 x16 档：x8 模式（128Mb × 8DQ）在同密度下把 data_bus_bits 减半，
    # 但引擎的 achieved_bw_GBps 只按 payload/时间 计算、不随位宽变化
    # （src/controller/controller.cpp），把位宽设成 8 会得到 achieved > peak
    # 的物理不可能结果。在接口位宽与可达带宽的耦合明确之前，该档位不成立。
    "lpddr5": [
        ("12gb_x16", {"rows": 49152}),
        ("16gb_x16", {}),
    ],
    "lpddr6": [
        ("8gb_sc", {"rows": 32768}),
        ("16gb_sc", {}),
        ("24gb_sc", {"rows": 98304}),
    ],
}

# 刷新组的研究型压力间隔。取值要能在较短窗口内触发维护，不是 JEDEC 物理间隔；
# nREFI/nREFIpb 还要乘 tick_multiplier 才是实际周期。
REFRESH_TIMINGS = {
    "nRFC": 16, "nRFCpb": 16,
    "nREFI": 32, "nREFIpb": 32,
    "nREFDB2ACT": 16, "nREFDB2REFDBS": 16, "nREFDB2REFDBL": 16,
}

# bank scaling 门禁允许的输出抖动；只做端点比较。
BANK_SCALING_TOLERANCE = 0.99

# probe 覆盖多少个 lane 来校验地址编码。
PROBE_LANES = 8

CSV_FIELDS = (
    "standard", "group", "case", "workload", "perturbation", "overrides", "requests",
    "channels", "pseudo_channels", "sids", "ranks", "bank_groups", "banks_per_group",
    "rows", "columns", "stack_height", "lanes", "banks_per_lane",
    "total_banks", "engaged_banks",
    "coverage", "aggregate_capacity_GiB", "capacity_formula_GiB",
    "capacity_ratio_vs_baseline", "channel_mapper", "avg_read_latency_ticks",
    "achieved_bw_GBps", "bw_per_engaged_bank_GBps", "row_hits", "row_misses",
    "row_conflicts", "row_hit_rate_pct", "row_conflict_rate_pct",
    "refresh_pb_batches", "refresh_ab_batches",
    "cmd_validation", "dfi_validation", "data_mismatches",
    "hit_cycle_limit", "cycles", "wall_seconds",
)


@dataclass(frozen=True)
class Case:
    group: str
    name: str
    overrides: dict[str, str | int]


@dataclass(frozen=True)
class Check:
    status: str
    name: str
    detail: str


@dataclass(frozen=True)
class Observation:
    standard: str
    group: str
    case: str
    row: dict[str, object]
    org: dict[str, int]
    baseline: dict[str, int]
    planned_capacity: int
    reported_capacity: int
    density_gb: float
    capacity_per_instance: int
    stack_height: int


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=ROOT / "build-clang-debug/hbm_sim")
    parser.add_argument("--standards", default="hbm4,lpddr6",
                        help="comma-separated subset of hbm3,hbm4,lpddr5,lpddr6")
    parser.add_argument("--requests", type=int, default=512,
                        help="requests generated for the refresh workload; also the floor "
                             "for the density workload")
    parser.add_argument("--density-requests", type=int, default=0,
                        help="fixed request count for the density workload; 0 derives "
                             "max(--requests, the case's bank count)")
    parser.add_argument("--inject-interval", type=int, default=2,
                        help="arrival spacing used by the refresh workload")
    parser.add_argument("--case-timeout", type=float, default=120.0,
                        help="wall-clock timeout in seconds for one simulator case")
    parser.add_argument("--out", type=Path, default=ROOT / "outputs/experiments/architecture_sweep")
    return parser.parse_args()


def family(standard: str) -> str:
    if standard in {"hbm3", "hbm4"}:
        return "hbm"
    if standard in {"lpddr5", "lpddr6"}:
        return "lpddr"
    raise SystemExit(f"unsupported standard: {standard}")


def transaction_bytes(standard: str) -> int:
    # 两个权威 master 当前对四个标准都使用 32 B DRAM transaction；
    # frontend 的 64 B host line 会再拆成两个 transaction。这里必须使用
    # transaction 粒度编码 column/bank/row，不能误用 host line_size。
    values = {"hbm3": 32, "hbm4": 32, "lpddr5": 32, "lpddr6": 32}
    try:
        return values[standard]
    except KeyError as error:
        raise SystemExit(f"unsupported standard: {standard}") from error


def total_banks(org: dict[str, int]) -> int:
    return (org["channels"] * org["pseudo_channels"] * org["sids"] * org["ranks"] *
            org["bank_groups"] * org["banks_per_group"])


def lanes_of(org: dict[str, int]) -> int:
    return org["channels"] * org["pseudo_channels"] * org["sids"] * org["ranks"]


def capacity_of(org: dict[str, int]) -> int:
    # 容量 = Channel×PC×SID×Rank×BG×Bank×Row×事务Column×事务Byte；不含 stack_height。
    return total_banks(org) * org["rows"] * org["columns"] * org["dram_transaction_bytes"]


def parse_org(text: str) -> dict[str, int]:
    section = text.split("[architecture]", 1)[1].split("\n[", 1)[0]
    values = dict(re.findall(r"^(\w+)\s*=\s*(\S+)", section, re.M))
    org: dict[str, int] = {}
    for key in ORG_KEYS:
        if key not in values:
            raise SystemExit(f"resolved config is missing architecture.{key}")
        org[key] = int(values[key])
    return org


def baseline_org(args: argparse.Namespace, standard: str) -> dict[str, int]:
    # --check-config 在提前返回前写出 resolved config，实测约 7 ms，不跑仿真。
    path = args.out / standard / "baseline.cfg"
    path.parent.mkdir(parents=True, exist_ok=True)
    command = [
        str(args.binary), "--config", str(ROOT / f"configs/{family(standard)}.cfg"),
        "--standard", standard, "--check-config",
        "--dump-resolved-config", str(path),
    ]
    try:
        subprocess.run(command, cwd=ROOT, check=True, text=True,
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=60)
    except (subprocess.CalledProcessError, subprocess.TimeoutExpired) as error:
        output = error.stdout or ""
        if isinstance(output, bytes):
            output = output.decode(errors="replace")
        raise SystemExit(f"baseline pre-pass failed for {standard}: {output}") from error
    return parse_org(path.read_text(encoding="utf-8"))


def cases(standard: str, baseline: dict[str, int]) -> list[Case]:
    # 密度组三点按各标准的真实规格取点（见 DENSITY_VARIANTS）；其中覆盖为空的
    # 那点是基线对照组，用于 baseline stability 检查。refresh 组只改 refresh_policy。
    # 组织扰动组每项只放大一个组织键。没有覆盖空值的标准说明表写错了，
    # 直接报错而不是静默跳过对照。
    return (
        [Case("density", name, dict(overrides))
         for name, overrides in DENSITY_VARIANTS[standard]] +
        [Case("refresh", "per_bank", {"refresh_policy": "per_bank"}),
         Case("refresh", "all_bank", {"refresh_policy": "all_bank"})] +
        [Case("organization", name, {key: baseline[key] * 2})
         for name, key in ORGANIZATION_VARIANTS] +
        [Case("organization", "mixed_shift", dict(MIXED_SHIFT[standard]))]
    )


def control_case(standard: str) -> str:
    # 覆盖为空的那点必须复现解析出的基线，用作 baseline stability 对照。
    controls = [name for name, overrides in DENSITY_VARIANTS[standard] if not overrides]
    if len(controls) != 1:
        raise SystemExit(f"{standard}: DENSITY_VARIANTS must declare exactly one "
                         f"baseline control case, found {controls}")
    return controls[0]


def declared_org_fields(case: Case) -> set[str]:
    return {key for key in case.overrides if key in ORG_KEYS}


def effective_org(standard: str, case: Case,
                  baseline: dict[str, int] | None = None) -> dict[str, int]:
    org = dict(STANDARD_ORGS[standard] if baseline is None else baseline)
    for key in declared_org_fields(case):
        org[key] = int(case.overrides[key])
    return org


def common_overrides(standard: str, case: Case) -> dict[str, str | int]:
    # 只保留单个 stack。不再强制 single_controller/channels/pc/sids：
    # 标准组织本身就是基线；且 single_controller 与 channels>1 组合会绕过
    # MemorySystem::make_channel_spec 的 channel 本地化，在同一份报告容量下
    # 仿真的是另一台机器（引擎只给警告，不拦截）。
    values: dict[str, str | int] = {
        "model_name": f"architecture_sweep_{standard}_{case.name}",
        "stack_count": 1,
        "mem_phy_mode": "direct",
        "address_mapping": "default",
        # configs/hbm.cfg 默认 xor，会按 (line ^ line>>6 ^ line>>12) % channels
        # 覆盖解码出的 channel 字段。该哈希多对一、需在 row 上搜索才能反解，
        # 而 row 正是 geometry 组要测的变量，故显式改回 decoded。
        "channel_mapper": "decoded",
        "supports_refresh": "true" if case.group == "refresh" else "false",
        "max_cycles": 10_000_000,
    }
    values.update(case.overrides)
    if case.group == "refresh":
        values.update(REFRESH_TIMINGS)
        values["timing_override_source"] = "research"
    return values


def overlay_text(standard: str, case: Case) -> str:
    lines = ["[override]"]
    lines.extend(f"{key} = {value}" for key, value in common_overrides(standard, case).items())
    return "\n".join(lines) + "\n"


def default_address(*, row: int, column: int, bank: int, bank_group: int,
                    org: dict[str, int], tx_bytes: int,
                    pseudo_channel: int = 0, sid: int = 0, rank: int = 0,
                    channel: int = 0) -> int:
    # AddressMapper::Default 的低位顺序是 column, bank, BG, PC, SID, rank,
    # channel, row。byte address 先除以 dram_transaction_bytes 得到 line。
    line = column
    stride = org["columns"]
    line += stride * bank
    stride *= org["banks_per_group"]
    line += stride * bank_group
    stride *= org["bank_groups"]
    line += stride * pseudo_channel
    stride *= org["pseudo_channels"]
    line += stride * sid
    stride *= org["sids"]
    line += stride * rank
    stride *= org["ranks"]
    line += stride * channel
    stride *= org["channels"]
    return (line + stride * row) * tx_bytes


def decode_line(line: int, org: dict[str, int]) -> dict[str, int]:
    # default_address 的逆运算，供 probe 校验引擎解码坐标。
    decoded: dict[str, int] = {}
    for key in ("columns", "banks_per_group", "bank_groups", "pseudo_channels",
                "sids", "ranks", "channels"):
        decoded[key] = line % org[key]
        line //= org[key]
    # 键名与 ORG_KEYS 保持一致，probe 才能按同一张坐标表比对。
    decoded["rows"] = line % org["rows"]
    return decoded


def split_lane(lane: int, org: dict[str, int]) -> tuple[int, int, int, int]:
    # 与地址映射同序：低位是 pseudo_channel，再 SID、rank，最高是 channel。
    pseudo_channel = lane % org["pseudo_channels"]
    lane //= org["pseudo_channels"]
    sid = lane % org["sids"]
    lane //= org["sids"]
    rank = lane % org["ranks"]
    lane //= org["ranks"]
    channel = lane % org["channels"]
    return channel, rank, sid, pseudo_channel


def split_flat_bank(flat: int, org: dict[str, int]) -> tuple[int, int]:
    bank = flat % org["banks_per_group"]
    bank_group = (flat // org["banks_per_group"]) % org["bank_groups"]
    return bank, bank_group


def walk(index: int, org: dict[str, int]) -> dict[str, int]:
    # lane = index % lanes 与被扫的 banks_per_group 无关，保证三个 bank case
    # 呈现给机器的 lane 数和每 lane 请求数相同，唯一差别是 bank 复用。
    lane_count = lanes_of(org)
    lane = index % lane_count
    bank_slot = index // lane_count
    per_lane = org["bank_groups"] * org["banks_per_group"]
    bank, bank_group = split_flat_bank(bank_slot % per_lane, org)
    channel, rank, sid, pseudo_channel = split_lane(lane, org)
    return {"row": bank_slot // per_lane, "column": 0, "bank": bank,
            "bank_group": bank_group, "channel": channel, "rank": rank,
            "sid": sid, "pseudo_channel": pseudo_channel}


def requests_for(args: argparse.Namespace, standard: str, case: Case,
                 baseline: dict[str, int]) -> int:
    if case.group == "organization":
        # 与基线对照组用同一份 workload：只有组织变、负载不变。这是本组唯一
        # 干净的比较方式；若按各自的 bank 数放大请求数，每 lane 负载会跟着变，
        # 就分不清差异来自组织还是来自负载。
        if args.density_requests > 0:
            return args.density_requests
        return max(args.requests, total_banks(baseline))
    if case.group != "density":
        return args.requests
    if args.density_requests > 0:
        return args.density_requests
    # 密度档之间的 bank/lane 数会变（HBM4 的 SID 域数量随 stack_height 变）。
    # 取 max(requests, 该档 total_banks) 使每 lane 请求数在各档之间相等，
    # 带宽差异才归因于并行度而不是负载不均。
    return max(args.requests, total_banks(effective_org(standard, case, baseline)))


def write_trace(path: Path, standard: str, case: Case, requests: int,
                inject_interval: int, *, org: dict[str, int] | None = None) -> None:
    resolved = effective_org(standard, case, org)
    tx_bytes = resolved["dram_transaction_bytes"]
    lane_count = lanes_of(resolved)
    lines: list[str] = []
    for index in range(requests):
        if case.group in {"density", "organization"}:
            # 同时到达并轮转全部 (lane, bank)；同一 bank 每轮访问一个从未使用过的
            # row，避免 row wrap 产生的 FR-FCFS 命中把"可并行 bank 数"与
            # "调度器重排行命中"混在一起。组织扰动组复用同一走法，使组织是
            # 两组之间唯一的差别。
            fields = walk(index, resolved)
            arrival = 0
        else:
            # 持续负载让自动 REFpb/REFdb 与 REFab 都能介入调度。
            fields = walk(index, resolved)
            fields["row"] = (index // lane_count) % 4
            fields["column"] = 0
            arrival = index * inject_interval
        address = default_address(org=resolved, tx_bytes=tx_bytes, **fields)
        lines.append(f"{arrival} R 0x{address:x}")
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def run_simulator(args: argparse.Namespace, standard: str, case: Case,
                  command: list[str], case_dir: Path) -> None:
    try:
        completed = subprocess.run(
            command, cwd=ROOT, check=True, text=True,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            timeout=args.case_timeout,
        )
    except subprocess.TimeoutExpired as error:
        output = error.stdout or ""
        if isinstance(output, bytes):
            output = output.decode(errors="replace")
        (case_dir / "stats.txt").write_text(output, encoding="utf-8")
        raise SystemExit(
            f"case timed out after {args.case_timeout:g}s: {standard} {case.group}/{case.name}; "
            f"partial output: {case_dir / 'stats.txt'}"
        ) from error
    except subprocess.CalledProcessError as error:
        (case_dir / "stats.txt").write_text(error.stdout or "", encoding="utf-8")
        raise SystemExit(
            f"case failed: {standard} {case.group}/{case.name}; "
            f"output: {case_dir / 'stats.txt'}"
        ) from error
    (case_dir / "stats.txt").write_text(completed.stdout, encoding="utf-8")


def simulator_command(args: argparse.Namespace, standard: str, overlay: str,
                      trace_path: Path, case_dir: Path, extra: list[str]) -> list[str]:
    # 每个 case 都启用命令与 DFI 验证器：容量核算只能证明"配置被正确接受"，
    # 组织扰动组要成立还需要"发出的命令序列在该组织下仍然协议合法"。
    # 实测开销约 +36%（单 case 0.39s → 0.53s）。
    return [
        str(args.binary), "--config", str(ROOT / f"configs/{family(standard)}.cfg"),
        "--standard", standard, "--config", overlay,
        "--trace", str(trace_path), "--requests", "0",
        "--inject-interval", str(args.inject_interval),
        "--validate-cmd-trace", "--validate-dfi-trace",
        "--dump-resolved-config", str(case_dir / "resolved.cfg"),
        "--stats-view", "diagnostic", "--stats-json", str(case_dir / "result.json"),
    ] + extra


def run_case(args: argparse.Namespace, standard: str, case: Case,
             baseline: dict[str, int], ordinal: int, total: int) -> Observation:
    case_dir = args.out / standard / f"{case.group}_{case.name}"
    case_dir.mkdir(parents=True, exist_ok=True)
    requests = requests_for(args, standard, case, baseline)
    trace_path = case_dir / "workload.trace"
    write_trace(trace_path, standard, case, requests, args.inject_interval, org=baseline)
    print(f"[{ordinal}/{total}] {standard.upper()} {case.group}/{case.name} "
          f"({requests} requests)", flush=True)
    started = time.monotonic()
    with tempfile.NamedTemporaryFile("w", suffix=".cfg", encoding="utf-8") as overlay:
        overlay.write(overlay_text(standard, case))
        overlay.flush()
        run_simulator(args, standard, case,
                      simulator_command(args, standard, overlay.name, trace_path,
                                        case_dir, []), case_dir)
    elapsed = time.monotonic() - started

    stats = read_result(case_dir / "result.json", require_completed=True)
    observed_org = parse_org((case_dir / "resolved.cfg").read_text(encoding="utf-8"))
    resolved = effective_org(standard, case, baseline)

    hits = number(stats, "row_hits")
    misses = number(stats, "row_misses")
    conflicts = number(stats, "row_conflicts")
    decisions = hits + misses + conflicts
    reported_capacity = count(stats, "aggregate_capacity_bytes")
    planned_capacity = capacity_of(resolved)
    baseline_capacity = capacity_of(baseline)
    lane_count = lanes_of(observed_org)
    per_lane = observed_org["bank_groups"] * observed_org["banks_per_group"]
    banks = lane_count * per_lane
    engaged = min(requests, banks)
    bandwidth = number(stats, "achieved_bw_GBps")
    row = {
        "standard": standard.upper(), "group": case.group, "case": case.name,
        "workload": ("bank_rotation" if case.group in {"density", "organization"}
                     else "refresh_pressure"),
        "perturbation": ";".join(f"{k}={v}" for k, v in case.overrides.items()) or "none",
        "overrides": ";".join(f"{k}={v}" for k, v in common_overrides(standard, case).items()),
        "requests": requests,
        "channels": observed_org["channels"],
        "pseudo_channels": observed_org["pseudo_channels"],
        "sids": observed_org["sids"], "ranks": observed_org["ranks"],
        "bank_groups": observed_org["bank_groups"],
        "banks_per_group": observed_org["banks_per_group"],
        "rows": observed_org["rows"], "columns": observed_org["columns"],
        "stack_height": observed_org["stack_height"],
        "lanes": lane_count, "banks_per_lane": per_lane, "total_banks": banks,
        "engaged_banks": engaged, "coverage": engaged / banks if banks else 0.0,
        "aggregate_capacity_GiB": reported_capacity / 2**30,
        "capacity_formula_GiB": planned_capacity / 2**30,
        "capacity_ratio_vs_baseline": planned_capacity / baseline_capacity,
        "channel_mapper": stats.get("channel_mapper", ""),
        "avg_read_latency_ticks": number(stats, "avg_read_latency"),
        "achieved_bw_GBps": bandwidth,
        "bw_per_engaged_bank_GBps": bandwidth / engaged if engaged else 0.0,
        "row_hits": int(hits), "row_misses": int(misses), "row_conflicts": int(conflicts),
        "row_hit_rate_pct": 100.0 * hits / decisions if decisions else 0.0,
        "row_conflict_rate_pct": 100.0 * conflicts / decisions if decisions else 0.0,
        "refresh_pb_batches": int(number(stats, "refresh_pb_batches")),
        "refresh_ab_batches": int(number(stats, "refresh_ab_batches")),
        "cmd_validation": stats.get("cmd_validation", ""),
        "dfi_validation": stats.get("dfi_validation", ""),
        "data_mismatches": int(number(stats, "data_mismatches")),
        "hit_cycle_limit": stats["hit_cycle_limit"].lower(),
        "cycles": int(number(stats, "cycles")),
        "wall_seconds": round(elapsed, 3),
    }
    return Observation(
        standard=standard.upper(), group=case.group, case=case.name, row=row,
        org=observed_org, baseline=baseline, planned_capacity=planned_capacity,
        reported_capacity=reported_capacity, density_gb=number(stats, "density_gb"),
        capacity_per_instance=count(stats, "capacity_per_instance_bytes"),
        stack_height=count(stats, "stack_height"))


def probe_address_encoding(args: argparse.Namespace, standard: str,
                           baseline: dict[str, int]) -> Check:
    # 用 --cmd-trace 导出的显式 DRAM 坐标校验 Python 侧地址编码与引擎解码一致。
    # 注意不能用 storage_*_touched：那些计数在 ch=1 与 ch=32 下实测都是 0，
    # 拿它做断言是空检查。
    label = f"address encoding: {standard.upper()}"
    case_dir = args.out / standard / "probe"
    case_dir.mkdir(parents=True, exist_ok=True)
    resolved = dict(baseline)
    lane_count = lanes_of(resolved)
    probe_count = min(lane_count, PROBE_LANES)
    step = max(1, lane_count // probe_count)
    tx_bytes = resolved["dram_transaction_bytes"]
    lines: list[str] = []
    for index in range(probe_count):
        lane = (index * step) % lane_count
        channel, rank, sid, pseudo_channel = split_lane(lane, resolved)
        # 每条约请求用不同 row，使 row 字段也进入校验。
        address = default_address(row=index + 1, column=0, bank=0, bank_group=0,
                                  pseudo_channel=pseudo_channel, sid=sid, rank=rank,
                                  channel=channel, org=resolved, tx_bytes=tx_bytes)
        lines.append(f"0 R 0x{address:x}")
    trace_path = case_dir / "workload.trace"
    trace_path.write_text("\n".join(lines) + "\n", encoding="utf-8")
    csv_path = case_dir / "cmd_trace.csv"
    case = Case("bank", "probe", {})
    with tempfile.NamedTemporaryFile("w", suffix=".cfg", encoding="utf-8") as overlay:
        overlay.write(overlay_text(standard, case))
        overlay.flush()
        run_simulator(args, standard, case,
                      simulator_command(args, standard, overlay.name, trace_path,
                                        case_dir, ["--cmd-trace", str(csv_path)]),
                      case_dir)
    if not csv_path.is_file():
        return Check("FAIL", label, "engine did not write a command trace")
    mismatches: list[str] = []
    seen = 0
    coordinate_keys = (("column", "columns"), ("bank", "banks_per_group"),
                       ("bank_group", "bank_groups"), ("pseudo_channel", "pseudo_channels"),
                       ("sid", "sids"), ("rank", "ranks"),
                       ("channel", "channels"), ("row", "rows"))
    with csv_path.open(newline="", encoding="utf-8") as stream:
        for entry in csv.DictReader(stream):
            address = int(entry["address"], 16)
            expected = decode_line(address // tx_bytes, resolved)
            seen += 1
            for csv_key, org_key in coordinate_keys:
                if int(entry[csv_key]) != expected[org_key]:
                    mismatches.append(f"addr=0x{address:x} {csv_key}={entry[csv_key]}"
                                      f" expected {expected[org_key]}")
            if len(mismatches) >= 3:
                break
    if not seen:
        return Check("FAIL", label, "command trace is empty")
    if mismatches:
        return Check("FAIL", label, "; ".join(mismatches[:3]))
    return Check("PASS", label,
                 f"{seen} issued commands decode consistently with default_address(); "
                 f"covered {probe_count} of {lane_count} lanes")


def declared_of(observation: Observation) -> set[str]:
    # 该 case 在 perturbation 列里声明自己改了哪些组织键。
    return {item.split("=")[0] for item in
            str(observation.row["perturbation"]).split(";") if "=" in item}


def evaluate(observations: list[Observation]) -> list[Check]:
    checks: list[Check] = []
    for observation in observations:
        row = observation.row
        label = f"{observation.standard} {observation.group}/{observation.case}"
        valid = (int(row["cycles"]) > 0 and row["hit_cycle_limit"] == "false" and
                 int(row["data_mismatches"]) == 0)
        checks.append(Check("PASS" if valid else "FAIL", f"completion: {label}",
                            f"cycles={row['cycles']}, cycle_limit={row['hit_cycle_limit']}, "
                            f"mismatches={row['data_mismatches']}"))
        # 命令与 DFI 验证器逐 case 启用。组织扰动组（尤其 mixed_shift）的结果不能
        # 当器件数据读，它要成立的是"模型在这些组织下仍然自洽"——容量算术只证明
        # 配置被正确接受，还需要命令序列本身在该组织下协议合法。
        protocol_ok = row["cmd_validation"] == "pass" and row["dfi_validation"] == "pass"
        checks.append(Check("PASS" if protocol_ok else "FAIL",
                            f"protocol validation: {label}",
                            f"command={row['cmd_validation']}, dfi={row['dfi_validation']}"))
        # 验收标准：Python 按容量公式算出的组织容量必须等于引擎报告的容量。
        # 两者都是整数，不需要浮点容差。
        planned = observation.planned_capacity
        reported = observation.reported_capacity
        checks.append(Check("PASS" if planned == reported else "FAIL",
                            f"capacity: {label}",
                            f"formula={planned} ({planned / 2**30:.3f} GiB) "
                            f"reported={reported} ({reported / 2**30:.3f} GiB) "
                            f"ratio_vs_baseline="
                            f"{planned / capacity_of(observation.baseline):.3f}"))

    for standard in sorted({o.standard for o in observations}):
        subset = [o for o in observations if o.standard == standard]
        baseline = subset[0].baseline

        fixture = STANDARD_ORGS[standard.lower()]
        differences = [f"{k}: fixture={fixture[k]} parsed={baseline[k]}"
                       for k in ORG_KEYS if fixture[k] != baseline[k]]
        checks.append(Check("PASS" if not differences else "FAIL",
                            f"builtin org fixture: {standard}",
                            "; ".join(differences) or
                            "smoke fixture matches the parsed standard organization"))

        # “单维度扰动”的可执行断言：每个 case 的生效组织必须只在其声明的字段上
        # 偏离基线。直接比对 resolved.cfg 的解析结果，不依赖 result.json 的
        # changes 口径（后者是相对内置 profile 的 diff）。
        drift: list[str] = []
        for observation in subset:
            declared = declared_of(observation)
            for key in ORG_KEYS:
                if key in declared:
                    continue
                if observation.org[key] != baseline[key]:
                    drift.append(f"{observation.group}/{observation.case}: "
                                 f"{key} {baseline[key]}->{observation.org[key]}")
        checks.append(Check("PASS" if not drift else "FAIL",
                            f"organization drift: {standard}",
                            "; ".join(drift[:4]) or
                            "every case differs from the baseline only in its "
                            "declared dimension"))

        control_name = control_case(standard.lower())
        control = next(o for o in subset if o.case == control_name)
        off_baseline = [f"{k}: control={control.org[k]} baseline={baseline[k]}"
                        for k in ORG_KEYS if control.org[k] != baseline[k]]
        checks.append(Check("PASS" if not off_baseline else "FAIL",
                            f"baseline stability: {standard}",
                            "; ".join(off_baseline) or
                            f"{control_name} reproduces the parsed baseline "
                            f"field-for-field"))

        # density_gb 是 Gibit 口径（HBM 按每 die、LPDDR 按每通道子通道），
        # 不是容量；这里把它与容量口径的关系固定下来，防止被当容量读。
        # 用基线对照 case 报告，避免把某个密度档位的密度当成标准密度。
        sample = control
        is_lpddr = standard.lower().startswith("lpddr")
        divisor = (sample.org["channels"] * sample.org["pseudo_channels"] * sample.org["ranks"]
                   if is_lpddr else sample.stack_height)
        basis = ("channels x subchannels x ranks" if is_lpddr else "stack_height")
        expected_density = (sample.capacity_per_instance / 134217728.0 / divisor
                            if divisor else 0.0)
        density_ok = divisor > 0 and abs(expected_density - sample.density_gb) <= 1e-9
        checks.append(Check("PASS" if density_ok else "FAIL",
                            f"density basis: {standard}",
                            f"density_gb={sample.density_gb:g} = "
                            f"capacity_per_instance_bytes x 8 / 2^30 / ({basis}={divisor}); "
                            f"Gibit per {'channel/subchannel' if is_lpddr else 'die'}, "
                            f"not a capacity - capacity is aggregate_capacity_bytes"))

        density_rows = sorted((o for o in subset if o.group == "density"),
                              key=lambda o: int(o.row["total_banks"]))
        points = "; ".join(
            f"{o.case}: {o.row['total_banks']} banks / {o.row['lanes']} lanes, "
            f"rows={o.row['rows']}, {float(o.row['aggregate_capacity_GiB']):.2f} GiB, "
            f"bw={float(o.row['achieved_bw_GBps']):.3f}" for o in density_rows)

        # 组织形状不变的密度档位（只声明 rows）之间，行为指标应逐位相同：
        # 按 JEDEC 这些器件的 bank 组织与列宽在全密度档固定，只有容量和地址
        # 范围变。把它写成正向断言，避免"三点没差异"被当成空洞通过。
        shape_fixed = [o for o in density_rows if declared_of(o) <= {"rows"}]
        if len(shape_fixed) >= 2:
            first = shape_fixed[0].row
            same = all(
                (o.row["cycles"], o.row["row_hit_rate_pct"],
                 o.row["row_conflict_rate_pct"]) ==
                (first["cycles"], first["row_hit_rate_pct"],
                 first["row_conflict_rate_pct"]) for o in shape_fixed)
            checks.append(Check("PASS" if same else "FAIL",
                                f"density behavior invariance: {standard}",
                                f"{len(shape_fixed)} variants share one organization shape "
                                f"(only rows differ): cycles/hit/conflict "
                                f"{'identical' if same else 'DIFFER'}; per JEDEC these "
                                f"densities differ only in capacity. {points}"))
        else:
            checks.append(Check("PASS", f"density behavior invariance: {standard}",
                                f"no rows-only variant pair to compare. {points}"))

        # 只有 bank/lane 数随密度变的家族才有可分辨的并行度轴。HBM3 与 LPDDR
        # 的 bank 组织在全密度档固定，bank scaling 在这些标准上无信号可言。
        distinct = {int(o.row["total_banks"]) for o in density_rows}
        # 带宽比较只在各档每 lane 负载相等且非零时成立：lane 数随密度档变，
        # 若把请求数固定成一个常数，档位越高每 lane 分到的请求越少，
        # 带宽下降只是负载摊薄，不是并行度变差。门禁必须先校验这个前提。
        per_lane = {int(o.row["requests"]) // max(1, int(o.row["lanes"]))
                    for o in density_rows}
        comparable = len(per_lane) == 1 and next(iter(per_lane)) > 0
        if len(distinct) >= 2 and not comparable:
            checks.append(Check("PASS", f"density scaling: {standard}",
                                f"not comparable: request count is pinned at "
                                f"{density_rows[0].row['requests']} while lanes differ "
                                f"({density_rows[0].row['lanes']}..{density_rows[-1].row['lanes']}), "
                                f"so per-lane load is {sorted(per_lane)} - raise "
                                f"--density-requests (use 0 for auto). {points}"))
        elif len(distinct) >= 2:
            low, high = density_rows[0], density_rows[-1]
            scaled = (float(high.row["achieved_bw_GBps"]) + 1e-9 >=
                      BANK_SCALING_TOLERANCE * float(low.row["achieved_bw_GBps"]))
            checks.append(Check("PASS" if scaled else "FAIL",
                                f"density scaling: {standard}",
                                f"{low.row['total_banks']} -> {high.row['total_banks']} banks: "
                                f"{float(low.row['achieved_bw_GBps']):.3f} -> "
                                f"{float(high.row['achieved_bw_GBps']):.3f} GB/s "
                                f"(per-lane load held equal at {next(iter(per_lane))}). {points}"))
        else:
            checks.append(Check("PASS", f"density scaling: {standard}",
                                f"bank organization is fixed at {next(iter(distinct))} banks "
                                f"across all density variants, so there is no "
                                f"bank-parallelism axis to resolve. {points}"))

        by_case = {o.case: o.row for o in subset if o.group == "refresh"}
        per_bank, all_bank = by_case["per_bank"], by_case["all_bank"]
        refresh_ok = (int(per_bank["refresh_pb_batches"]) > 0 and
                      int(per_bank["refresh_ab_batches"]) == 0 and
                      int(all_bank["refresh_ab_batches"]) > 0 and
                      int(all_bank["refresh_pb_batches"]) == 0)
        checks.append(Check("PASS" if refresh_ok else "FAIL", f"refresh scope: {standard}",
                            f"per-bank(pb={per_bank['refresh_pb_batches']},"
                            f"ab={per_bank['refresh_ab_batches']}); "
                            f"all-bank(pb={all_bank['refresh_pb_batches']},"
                            f"ab={all_bank['refresh_ab_batches']})"))
    return checks


def write_csv(path: Path, observations: list[Observation]) -> None:
    with path.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(CSV_FIELDS))
        writer.writeheader()
        writer.writerows(observation.row for observation in observations)


def write_checks(path: Path, checks: list[Check]) -> None:
    with path.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.writer(stream)
        writer.writerow(["status", "check", "detail"])
        writer.writerows((check.status, check.name, check.detail) for check in checks)


def write_summary(path: Path, observations: list[Observation], checks: list[Check],
                  args: argparse.Namespace) -> None:
    lines = [
        "# Architecture sweep result", "",
        "每个 case 以该标准**自己的完整标准组织**为基线。密度组按各标准的真实 JEDEC",
        "规格取点，真实器件变密度时不变的是 `channels`、`banks_per_group`、`columns`，",
        "变的是 `rows`，以及（HBM4 的堆叠配置）`stack_height` 所带动的 SID 域数量；",
        "refresh 组只改 `refresh_policy`。",
        f"密度组每个 case 生成 max(--requests={args.requests}, 该档 bank 数) 个请求，"
        "使各档每 lane 负载相等，带宽差异才归因于并行度；"
        f"refresh 组固定 {args.requests} 个请求（inject_interval={args.inject_interval}）。",
        "`channel_mapper` 显式设为 `decoded`：`configs/hbm.cfg` 默认的 `xor` 会按地址哈希覆盖",
        "解码出的 channel，实验无法控制落点。",
        "workload.trace 与 resolved.cfg 是审计依据。", "",
        "## Automated checks", "", "| status | check | detail |", "|---|---|---|",
    ]
    lines.extend(f"| {c.status} | {c.name} | {c.detail} |" for c in checks)
    lines.extend([
        "", "## Trend interpretation", "",
        "以下解释只比较同一标准、同一组内的定向 workload。密度组的可分辨性取决于该家族"
        "是否在密度档之间有 bank 组织变化：按 JEDEC，HBM3 与 LPDDR 的 bank 组织在全密度档"
        "固定，因此这些标准的密度轴只改变容量与地址范围，行为指标逐位相同——脚本对此做"
        "正向断言，而不是给出趋势。refresh 只对 REFpb/REFdb/REFab scope 做硬性门禁，"
        "不预设 per-bank 必然快于 all-bank。", "",
    ])
    for standard in sorted({o.standard for o in observations}):
        subset = [o for o in observations if o.standard == standard]
        base = subset[0].baseline
        density = sorted((o for o in subset if o.group == "density"),
                         key=lambda o: int(o.row["total_banks"]))
        refresh = {o.case: o.row for o in subset if o.group == "refresh"}
        per_bank, all_bank = refresh["per_bank"], refresh["all_bank"]
        bank_counts = {int(o.row["total_banks"]) for o in density}
        organization = sorted((o for o in subset if o.group == "organization"),
                              key=lambda o: o.case)
        control = next(o for o in subset
                       if o.case == control_case(standard.lower()))
        lines.extend([
            f"### {standard}", "",
            f"- 标准基线：{base['channels']}ch × {base['pseudo_channels']}pc × "
            f"{base['sids']}sid × {base['ranks']}rank × {base['bank_groups']}bg × "
            f"{base['banks_per_group']}bpg × {base['rows']}rows × {base['columns']}cols × "
            f"{base['dram_transaction_bytes']}B，stack_height={base['stack_height']}，"
            f"= {capacity_of(base) / 2**30:.3f} GiB。",
            f"- 密度：{density[0].case}→{density[-1].case}，容量 "
            f"{float(density[0].row['aggregate_capacity_GiB']):.2f}→"
            f"{float(density[-1].row['aggregate_capacity_GiB']):.2f} GiB，bank "
            f"{density[0].row['total_banks']}→{density[-1].row['total_banks']}，带宽 "
            f"{float(density[0].row['achieved_bw_GBps']):.3f}→"
            f"{float(density[-1].row['achieved_bw_GBps']):.3f} GB/s。"
            + ("该标准的 bank 组织在所有密度档位相同，因此没有可分辨的并行度轴，"
               "三点差异只体现在容量上。"
               if len(bank_counts) < 2 else
               "bank 数随密度档变化，带宽差异即并行度差异。"),
            f"- Refresh：per-bank 触发 {per_bank['refresh_pb_batches']} 个 PB batch，"
            f"延迟/带宽 {float(per_bank['avg_read_latency_ticks']):.2f} tick / "
            f"{float(per_bank['achieved_bw_GBps']):.3f} GB/s；all-bank 触发 "
            f"{all_bank['refresh_ab_batches']} 个 AB batch，延迟/带宽 "
            f"{float(all_bank['avg_read_latency_ticks']):.2f} tick / "
            f"{float(all_bank['achieved_bw_GBps']):.3f} GB/s。间隔是 research 型压力值，"
            "取值以保证在短窗口内可观测，不是物理间隔。",
            f"- 组织扰动：以基线对照（{control.case}，{control.row['total_banks']} banks、"
            f"cov={float(control.row['coverage']):.2f}、"
            f"{float(control.row['achieved_bw_GBps']):.3f} GB/s）为参照，"
            + "；".join(
                f"{o.case} 改 {'/'.join(sorted(declared_of(o))) or '—'} 后 "
                f"{o.row['total_banks']} banks、cov={float(o.row['coverage']):.2f}、"
                f"{float(o.row['achieved_bw_GBps']):.3f} GB/s"
                for o in organization)
            + "。本组只改组织、不改 workload，因此 bank 数翻倍时覆盖率必然降到约一半"
              "（固定请求数下新增的 bank 到不了）。脚本因此只对本组做容量核算与单键"
              "漂移校验，**不给出带宽趋势结论**：在未覆盖到的 bank 上，带宽差异既可能"
              "来自并行度，也可能只是负载被摊薄。", "",
        ])
    lines.extend([
        "", "## Measurements", "",
        "| standard | group | case | perturbation | requests | rows | cols | sids | "
        "stackH | banks | lanes | cap/GiB | ratio | BW/GBps | hit/% | conflict/% | "
        "REFpb | REFab | wall/s |",
        "|---|---|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|",
    ])
    for observation in observations:
        row = observation.row
        lines.append(
            f"| {row['standard']} | {row['group']} | {row['case']} | `{row['perturbation']}` | "
            f"{row['requests']} | {row['rows']} | {row['columns']} | {row['sids']} | "
            f"{row['stack_height']} | {row['total_banks']} | {row['lanes']} | "
            f"{float(row['aggregate_capacity_GiB']):.3f} | "
            f"{float(row['capacity_ratio_vs_baseline']):.3f} | "
            f"{float(row['achieved_bw_GBps']):.3f} | {float(row['row_hit_rate_pct']):.2f} | "
            f"{float(row['row_conflict_rate_pct']):.2f} | {row['refresh_pb_batches']} | "
            f"{row['refresh_ab_batches']} | {float(row['wall_seconds']):.3f} |"
        )
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def write_html(path: Path, observations: list[Observation], checks: list[Check]) -> None:
    serialized_checks = "\n".join(
        f"<tr><td class='{c.status.lower()}'>{c.status}</td><td>{html.escape(c.name)}</td>"
        f"<td>{html.escape(c.detail)}</td></tr>" for c in checks
    )
    serialized_rows = "\n".join(
        f"<tr><td>{html.escape(str(o.row['standard']))}</td>"
        f"<td>{html.escape(str(o.row['group']))}</td>"
        f"<td>{html.escape(str(o.row['case']))}</td>"
        f"<td>{html.escape(str(o.row['perturbation']))}</td>"
        f"<td>{float(o.row['aggregate_capacity_GiB']):.3f}</td>"
        f"<td>{float(o.row['capacity_ratio_vs_baseline']):.3f}</td>"
        f"<td>{float(o.row['avg_read_latency_ticks']):.2f}</td>"
        f"<td>{float(o.row['achieved_bw_GBps']):.3f}</td>"
        f"<td>{float(o.row['row_hit_rate_pct']):.2f}</td>"
        f"<td>{float(o.row['row_conflict_rate_pct']):.2f}</td></tr>" for o in observations
    )
    document = f"""<!doctype html><meta charset=\"utf-8\"><title>Architecture sweep</title>
<style>body{{font:14px system-ui;background:#0b1020;color:#e6edf7;padding:28px}}table{{border-collapse:collapse;width:100%;margin-bottom:28px}}th,td{{border:1px solid #334155;padding:8px;text-align:right}}th:first-child,td:first-child,th:nth-child(2),td:nth-child(2),th:nth-child(3),td:nth-child(3),th:nth-child(4),td:nth-child(4){{text-align:left}}tr:hover{{background:#17213b}}.pass{{color:#4ade80}}.fail{{color:#f87171}}</style>
<h1>HBM/LPDDR architecture sweep</h1><p>密度组按各标准的真实 JEDEC 规格取点；容量由容量公式自动核算，不是器件标定值。</p>
<p>按 JEDEC，HBM3 与 LPDDR 的 bank 组织在全密度档固定，其密度轴只改变容量与地址范围，脚本对三点行为的一致性做正向断言而非给出趋势；只有 bank 数随密度变的家族（HBM4 的 SID 域数量随 stack_height 变）才有可分辨的并行度轴。详细逐标准解释见同目录 summary.md。</p>
<h2>Automated checks</h2><table><thead><tr><th>status</th><th>check</th><th>detail</th></tr></thead><tbody>{serialized_checks}</tbody></table>
<h2>Measurements</h2><table><thead><tr><th>standard</th><th>group</th><th>case</th><th>perturbation</th><th>cap/GiB</th><th>ratio</th><th>latency/tick</th><th>BW/GBps</th><th>row hit/%</th><th>conflict/%</th></tr></thead><tbody>{serialized_rows}</tbody></table>"""
    path.write_text(document, encoding="utf-8")


def main() -> int:
    args = parse_args()
    if not args.binary.is_file():
        raise SystemExit(f"binary not found: {args.binary}")
    if (args.requests < 1 or args.inject_interval < 1 or args.case_timeout <= 0 or
            args.density_requests < 0):
        raise SystemExit("--requests, --inject-interval and --case-timeout must be positive; "
                         "--density-requests must be non-negative")
    standards = [item.strip().lower() for item in args.standards.split(",") if item.strip()]
    if not standards or len(standards) != len(set(standards)):
        raise SystemExit("--standards must contain a non-empty, unique list")
    for standard in standards:
        family(standard)
    args.out.mkdir(parents=True, exist_ok=True)

    baselines = {standard: baseline_org(args, standard) for standard in standards}
    all_cases = {standard: cases(standard, baselines[standard]) for standard in standards}
    total = sum(len(all_cases[standard]) for standard in standards)
    observations: list[Observation] = []
    probes: list[Check] = []
    ordinal = 0
    for standard in standards:
        for case in all_cases[standard]:
            ordinal += 1
            observations.append(run_case(args, standard, case, baselines[standard],
                                         ordinal, total))
        probes.append(probe_address_encoding(args, standard, baselines[standard]))
    checks = evaluate(observations) + probes
    write_csv(args.out / "results.csv", observations)
    write_checks(args.out / "checks.csv", checks)
    write_summary(args.out / "summary.md", observations, checks, args)
    write_html(args.out / "trends.html", observations, checks)
    failures = [check for check in checks if check.status == "FAIL"]
    if failures:
        for check in failures:
            print(f"FAIL: {check.name}: {check.detail}", file=sys.stderr)
        print(f"architecture sweep failed {len(failures)} check(s): {args.out / 'checks.csv'}")
        return 1
    print(f"architecture sweep complete: {args.out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
