# Copyright 2026 Google LLC
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

import csv
import sys
import os
import argparse
import logging
from typing import Dict, Tuple


def read_results(results_dir: str) -> Dict[str, Tuple[str, str, str]]:
    """Reads uvm_results.csv from a directory and returns {target: (status, reason, log_path)}."""
    csv_path = os.path.join(results_dir, "uvm_results.csv")
    if not os.path.exists(csv_path):
        logging.error(f"Error: Could not find {csv_path}")
        sys.exit(1)
    data = {}
    with open(csv_path, 'r') as f:
        reader = csv.DictReader(f)
        required_fields = ['Target', 'Status', 'Reason', 'Log Path']
        if reader.fieldnames is None or not all(field in reader.fieldnames
                                                for field in required_fields):
            missing = [
                field for field in required_fields
                if not reader.fieldnames or field not in reader.fieldnames
            ]
            logging.error(
                f"Error: CSV at {csv_path} is missing required columns: {missing}"
            )
            sys.exit(1)
        for row in reader:
            try:
                # Handle relative log paths in CSV if present
                log_path = row['Log Path']
                if not os.path.isabs(log_path):
                    log_path = os.path.join(results_dir, log_path)
                data[row['Target']] = (row['Status'], row['Reason'], log_path)
            except KeyError as e:
                logging.warning(
                    f"Warning: Skipping malformed row in {csv_path}: {e}"
                )
    return data


def compare(baseline_path: str, new_path: str):
    logging.info(f"Comparing Baseline: {baseline_path}")
    logging.info(f"       vs New Run : {new_path}")
    logging.info("-" * 60)
    base_data = read_results(baseline_path)
    new_data = read_results(new_path)
    all_targets = set(base_data.keys()) | set(new_data.keys())
    regressions = []
    fixes = []
    reason_changes = []
    missing_in_new = []
    new_in_new = []
    for target in all_targets:
        if target not in base_data:
            new_in_new.append(target)
            continue
        if target not in new_data:
            missing_in_new.append(target)
            continue
        b_status, b_reason, b_log = base_data[target]
        n_status, n_reason, n_log = new_data[target]
        if b_status == "PASS" and n_status == "FAIL":
            regressions.append((target, n_reason, b_log, n_log))
        elif b_status == "FAIL" and n_status == "PASS":
            fixes.append((target, b_reason, b_log, n_log))
        elif b_status == n_status and b_status == "FAIL" and b_reason != n_reason:
            reason_changes.append((target, b_reason, n_reason, b_log, n_log))
    # Output Report
    if regressions:
        logging.info(f"\n🔴 REGRESSIONS ({len(regressions)}):")
        for t, r, b_log, n_log in regressions:
            logging.info(f"  - {t}")
            logging.info(f"    Reason: {r}")
            logging.info(f"    Diff: diff {b_log} {n_log}")
    if fixes:
        logging.info(f"\n🟢 FIXES ({len(fixes)}):")
        for t, r, b_log, n_log in fixes:
            logging.info(f"  - {t}")
            logging.info(f"    Was: {r}")
            logging.info(f"    Diff: diff {b_log} {n_log}")
    if reason_changes:
        logging.info(f"\n🟡 ERROR MESSAGE CHANGED ({len(reason_changes)}):")
        for t, old, new, b_log, n_log in reason_changes:
            logging.info(f"  - {t}")
            logging.info(f"    Old: {old}")
            logging.info(f"    New: {new}")
            logging.info(f"    Diff: diff {b_log} {n_log}")
    if missing_in_new:
        logging.info(f"\n⚪ MISSING IN NEW RUN ({len(missing_in_new)}):")
        for t in missing_in_new:
            logging.info(f"  - {t}")
    if new_in_new:
        logging.info(f"\n🔵 NEW TESTS ({len(new_in_new)}):")
        for t in new_in_new:
            logging.info(f"  - {t}")
    if not (regressions or fixes or reason_changes or missing_in_new
            or new_in_new):
        logging.info("\n✅ No changes found. Results are identical.")


def main():
    # Configure logging
    logging.basicConfig(level=logging.INFO, format='%(message)s')
    parser = argparse.ArgumentParser(
        description="Compare two UVM regression output directories."
    )
    parser.add_argument(
        "baseline", help="Path to baseline regression directory"
    )
    parser.add_argument("new", help="Path to new regression directory")
    args = parser.parse_args()
    compare(args.baseline, args.new)


if __name__ == "__main__":
    main()
