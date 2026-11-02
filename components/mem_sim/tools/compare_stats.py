#!/usr/bin/env python3
"""Compare hbm_sim schema 1/2 JSON or historical/diagnostic key-value files.

Compact human reports are not machine inputs. Compare selected metrics with
explicit tolerances after normalizing protocol, workload and units.
"""

from __future__ import annotations

import argparse
import math
import sys
from pathlib import Path
from result_io import read_result


def parse_stats(path: Path) -> dict[str, str]:
    """Parse ``key : value`` lines while ignoring banners and free text."""
    return read_result(path)


def maybe_float(value: str) -> float | None:
    """Return a float only for plain numeric fields; otherwise return None."""
    try:
        number = float(value)
    except ValueError:
        return None
    if math.isnan(number) or math.isinf(number):
        return None
    return number


def split_keys(value: str | None) -> list[str]:
    if value is None or value.strip() == "":
        return []
    return [item.strip() for item in value.split(",") if item.strip()]


def compare_values(base: str, candidate: str, abs_tol: float, rel_tol: float) -> tuple[bool, str]:
    base_num = maybe_float(base)
    cand_num = maybe_float(candidate)
    if base_num is None or cand_num is None:
        return base == candidate, "exact" if base == candidate else "different"

    delta = cand_num - base_num
    limit = max(abs_tol, rel_tol * max(abs(base_num), 1.0))
    ok = abs(delta) <= limit
    rel = abs(delta) / max(abs(base_num), 1.0)
    return ok, f"delta={delta:.6g}, rel={rel:.6g}, limit={limit:.6g}"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("baseline", type=Path, help="baseline hbm_sim key-value output")
    parser.add_argument("candidate", type=Path, help="candidate hbm_sim key-value output")
    parser.add_argument(
        "--keys",
        help="comma-separated keys to compare; default compares keys common to both files",
    )
    parser.add_argument(
        "--ignore",
        default="",
        help="comma-separated keys to ignore when --keys is omitted",
    )
    parser.add_argument("--abs-tol", type=float, default=0.0, help="absolute numeric tolerance")
    parser.add_argument("--rel-tol", type=float, default=0.0, help="relative numeric tolerance")
    args = parser.parse_args()

    baseline = parse_stats(args.baseline)
    candidate = parse_stats(args.candidate)
    ignore = set(split_keys(args.ignore))
    requested = split_keys(args.keys)
    if requested:
        keys = requested
    else:
        keys = sorted((baseline.keys() & candidate.keys()) - ignore)

    missing = [key for key in keys if key not in baseline or key not in candidate]
    failed: list[str] = []
    for key in missing:
        failed.append(f"{key}: missing in {'baseline' if key not in baseline else 'candidate'}")

    compared = 0
    for key in keys:
        if key in missing or key not in baseline or key not in candidate:
            continue
        compared += 1
        ok, detail = compare_values(baseline[key], candidate[key], args.abs_tol, args.rel_tol)
        status = "PASS" if ok else "FAIL"
        print(f"{status:4} {key}: baseline={baseline[key]} candidate={candidate[key]} ({detail})")
        if not ok:
            failed.append(f"{key}: {detail}")

    print(f"compared={compared} failed={len(failed)}")
    if failed:
        for item in failed:
            print(f"error: {item}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
