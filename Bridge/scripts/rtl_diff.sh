#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."
mkdir -p sim

cpp_log=sim/rtl_diff_cpp.log
rtl_log=sim/rtl_diff_rtl.log

./build/full_link_256 --rp 1 >"$cpp_log" 2>&1
./build/obj_rtl_full_link/Vstorage_bridge_top --rp 1 >"$rtl_log" 2>&1

grep -q 'ALL FULL-LINK TESTS PASSED' "$cpp_log"
grep -q 'ALL FULL-LINK TESTS PASSED' "$rtl_log"

cpp_cases=$(grep -c '^\[PASS\] TC' "$cpp_log")
rtl_cases=$(grep -c '^\[PASS\] TC' "$rtl_log")
test "$cpp_cases" -eq 12
test "$rtl_cases" -eq 12

cpp_summary=$(grep '^DIFF:' "$cpp_log")
rtl_summary=$(grep '^DIFF:' "$rtl_log")
if [[ "$cpp_summary" != "$rtl_summary" ]]; then
    echo "C++/RTL semantic summaries differ" >&2
    echo "C++: $cpp_summary" >&2
    echo "RTL: $rtl_summary" >&2
    exit 1
fi

echo "[PASS] C++ and RTL completed the same 12 deterministic cases"
echo "[PASS] $rtl_summary"
echo "Detailed logs: $cpp_log, $rtl_log"
