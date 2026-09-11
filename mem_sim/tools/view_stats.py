#!/usr/bin/env python3
"""Render selected hbm_sim ``key : value`` statistics as an aligned table."""

from __future__ import annotations

import argparse
from pathlib import Path
from result_io import read_result


def parse_stats(path: Path) -> dict[str, str]:
    return read_result(path)


def split_csv(value: str) -> list[str]:
    return [item.strip() for item in value.split(",") if item.strip()]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("files", nargs="+", type=Path, help="hbm_sim stats files")
    parser.add_argument(
        "--keys",
        required=True,
        help="comma-separated keys, in the order in which they should be displayed",
    )
    parser.add_argument(
        "--labels",
        help="optional comma-separated column labels; defaults to the file paths",
    )
    args = parser.parse_args()

    missing_files = [path for path in args.files if not path.is_file()]
    if missing_files:
        parser.error("file not found: " + ", ".join(str(path) for path in missing_files))

    keys = split_csv(args.keys)
    if not keys:
        parser.error("--keys must contain at least one key")
    labels = split_csv(args.labels) if args.labels else [str(path) for path in args.files]
    if len(labels) != len(args.files):
        parser.error("--labels must contain exactly one label per input file")

    parsed = [parse_stats(path) for path in args.files]
    rows = [["metric", *labels]]
    rows.extend([key, *(stats.get(key, "-") for stats in parsed)] for key in keys)
    widths = [max(len(row[index]) for row in rows) for index in range(len(rows[0]))]

    for row_index, row in enumerate(rows):
        print("  ".join(value.ljust(widths[index]) for index, value in enumerate(row)).rstrip())
        if row_index == 0:
            print("  ".join("-" * width for width in widths))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
