#!/usr/bin/env bash
# Functional test suite for the UCIe SystemC link simulator.
set -uo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="${ROOT_DIR}/build/ucie_sc_sim"
OUT="${ROOT_DIR}/results/tests"
mkdir -p "${OUT}"

[[ -x "${BIN}" ]] || make -C "${ROOT_DIR}"

PASS=0
FAIL=0

metric() { awk -F, -v k="$2" '$1==k{print $2}' "$1"; }

check() {
  local name="$1" cond="$2"
  if eval "${cond}"; then
    echo "PASS  ${name}"
    PASS=$((PASS+1))
  else
    echo "FAIL  ${name}   (${cond})"
    FAIL=$((FAIL+1))
  fi
}

# ---------------------------------------------------------------- T1: clean link
# No impairments: all flits delivered in order, zero retries, utilization near
# the 92.19% flit-format ceiling, integrity clean.
CSV="${OUT}/t1.csv"
"${BIN}" --flits 3000 --sigma 0 --jitter 0 --isi1 0 --isi2 0 --skew 0 \
  --csv "${CSV}" > "${OUT}/t1.log" 2>&1
rc=$?
check "T1 clean link: exit 0"              "[[ ${rc} -eq 0 ]]"
check "T1 delivered all"                   "[[ \$(metric ${CSV} delivered_flits) -eq 3000 ]]"
check "T1 requests reached memory"         "[[ \$(metric ${CSV} requests_delivered) -eq 3000 ]]"
check "T1 reverse responses sent"          "[[ \$(metric ${CSV} reverse_tx_new_flits) -eq 3000 ]]"
check "T1 both PHY directions exercised"   "[[ \$(metric ${CSV} total_phy_frames) -eq 6000 ]]"
check "T1 zero replays"                    "[[ \$(metric ${CSV} tx_replay_flits) -eq 0 ]]"
check "T1 zero NAKs"                       "[[ \$(metric ${CSV} nak_count) -eq 0 ]]"
check "T1 zero integrity errors"           "[[ \$(metric ${CSV} integrity_errors) -eq 0 ]]"
check "T1 utilization >= 0.90"             "awk 'BEGIN{exit !($(metric ${CSV} bandwidth_utilization) >= 0.90)}'"
check "T1 sequence wrap crossed cleanly"    "[[ \$(metric ${CSV} delivered_flits) -gt 256 && \$(metric ${CSV} seq_fail_count) -eq 0 ]]"
check "T1 exact forward latency ~= 80 UI"   "awk 'BEGIN{x=$(metric ${CSV} forward_avg_latency_ns); exit !(x > 3.332 && x < 3.335)}'"
check "T1 exact round-trip latency ~= 192 UI" \
      "awk 'BEGIN{x=$(metric ${CSV} avg_latency_ns); exit !(x > 7.999 && x < 8.002)}'"
check "T1 P50/P95/P99 equal clean latency" \
      "awk 'BEGIN{a=$(metric ${CSV} avg_latency_ns); p50=$(metric ${CSV} p50_latency_ns); p95=$(metric ${CSV} p95_latency_ns); p99=$(metric ${CSV} p99_latency_ns); exit !(a==p50 && a==p95 && a==p99)}'"
check "T1 clean SER and BER are zero"       "awk 'BEGIN{exit !($(metric ${CSV} symbol_error_rate)==0 && $(metric ${CSV} bit_error_rate)==0)}'"
check "T1 link state active"                "[[ \$(metric ${CSV} link_state) == active ]]"

# ------------------------------------------------- T2: 24 GT/s rate requirement
# UI must equal 1/24 GT/s and capacity = lanes * 2 * 24 Gbps at PAM4.
CSV="${OUT}/t2.csv"
"${BIN}" --flits 500 --rate-gtps 24 --lanes 16 --sigma 0 --jitter 0 --isi1 0 --isi2 0 --skew 0 \
  --csv "${CSV}" > "${OUT}/t2.log" 2>&1
check "T2 exit 0"                          "[[ $? -eq 0 ]]"
check "T2 capacity 768 Gbps (16 lanes x 2 x 24)" \
      "awk 'BEGIN{c=$(metric ${CSV} capacity_gbps); exit !(c > 767.9 && c < 768.1)}'"
check "T2 rate recorded 24 GT/s"           "awk 'BEGIN{exit !($(metric ${CSV} lane_rate_gtps) == 24)}'"

# --------------------------------------------- T3: error injection -> NAK/retry
# 5% flit error probability: retries must occur, link must still deliver
# everything correctly and in order.
CSV="${OUT}/t3.csv"
"${BIN}" --flits 2000 --extra-flit-error 0.05 --csv "${CSV}" > "${OUT}/t3.log" 2>&1
check "T3 exit 0"                          "[[ $? -eq 0 ]]"
check "T3 delivered all"                   "[[ \$(metric ${CSV} delivered_flits) -eq 2000 ]]"
check "T3 NAKs occurred"                   "[[ \$(metric ${CSV} nak_count) -gt 0 ]]"
check "T3 replays occurred"                "[[ \$(metric ${CSV} tx_replay_flits) -gt 0 ]]"
check "T3 forward replays occurred"        "[[ \$(metric ${CSV} forward_tx_replay_flits) -gt 0 ]]"
check "T3 reverse replays occurred"        "[[ \$(metric ${CSV} reverse_tx_replay_flits) -gt 0 ]]"
check "T3 ACK and NAK exercised"            "[[ \$(metric ${CSV} ack_count) -gt 0 && \$(metric ${CSV} nak_count) -gt 0 ]]"
check "T3 crc failures observed"           "[[ \$(metric ${CSV} crc_fail_count) -gt 0 ]]"
check "T3 zero integrity errors"           "[[ \$(metric ${CSV} integrity_errors) -eq 0 ]]"

# ------------------------------------------------ T4: moderate channel noise
# AWGN sigma high enough to cause symbol errors, but recoverable: everything
# delivered, retry machinery visibly active, no data corruption reaches sink.
CSV="${OUT}/t4.csv"
"${BIN}" --flits 2000 --sigma 0.20 --csv "${CSV}" > "${OUT}/t4.log" 2>&1
check "T4 exit 0"                          "[[ $? -eq 0 ]]"
check "T4 delivered all"                   "[[ \$(metric ${CSV} delivered_flits) -eq 2000 ]]"
check "T4 symbol errors observed"          "awk 'BEGIN{exit !($(metric ${CSV} symbol_error_rate) > 0)}'"
check "T4 bit errors observed"             "awk 'BEGIN{exit !($(metric ${CSV} bit_error_rate) > 0)}'"
check "T4 replays occurred"                "[[ \$(metric ${CSV} tx_replay_flits) -gt 0 ]]"
check "T4 zero integrity errors"           "[[ \$(metric ${CSV} integrity_errors) -eq 0 ]]"

# ------------------------------------------------------------- T5: NRZ mode
# NRZ at 32 GT/s: capacity = lanes * 1 * 32; still delivers cleanly.
CSV="${OUT}/t5.csv"
"${BIN}" --flits 1000 --mod nrz --rate-gtps 32 --sigma 0 --jitter 0 --isi1 0 --isi2 0 --skew 0 \
  --csv "${CSV}" > "${OUT}/t5.log" 2>&1
check "T5 exit 0"                          "[[ $? -eq 0 ]]"
check "T5 capacity 512 Gbps (16 lanes x 1 x 32)" \
      "awk 'BEGIN{c=$(metric ${CSV} capacity_gbps); exit !(c > 511.9 && c < 512.1)}'"
check "T5 utilization >= 0.90"             "awk 'BEGIN{exit !($(metric ${CSV} bandwidth_utilization) >= 0.90)}'"
check "T5 zero integrity errors"           "[[ \$(metric ${CSV} integrity_errors) -eq 0 ]]"

# ------------------------------------------------------ T6: compact 68B format
CSV="${OUT}/t6.csv"
"${BIN}" --flits 1000 --format compact68 --sigma 0 --jitter 0 --isi1 0 --isi2 0 --skew 0 \
  --csv "${CSV}" > "${OUT}/t6.log" 2>&1
check "T6 exit 0"                          "[[ $? -eq 0 ]]"
check "T6 delivered all"                   "[[ \$(metric ${CSV} delivered_flits) -eq 1000 ]]"
check "T6 payload 64B"                     "[[ \$(metric ${CSV} payload_bytes) -eq 64 ]]"
check "T6 flit efficiency >= 0.94"          "awk 'BEGIN{exit !($(metric ${CSV} flit_efficiency) >= 0.94)}'"
check "T6 zero integrity errors"           "[[ \$(metric ${CSV} integrity_errors) -eq 0 ]]"

# ---------------------------------------------- T7: retry buffer backpressure
# Tiny retry buffer + long feedback delay: buffer-full backpressure must be
# hit, yet delivery must stay correct and complete.
CSV="${OUT}/t7.csv"
"${BIN}" --flits 1000 --retry-buffer 4 --feedback-delay 512 --csv "${CSV}" > "${OUT}/t7.log" 2>&1
check "T7 exit 0"                          "[[ $? -eq 0 ]]"
check "T7 delivered all"                   "[[ \$(metric ${CSV} delivered_flits) -eq 1000 ]]"
check "T7 buffer-full backpressure hit"    "[[ \$(metric ${CSV} retry_buffer_full_events) -gt 0 ]]"
check "T7 public FDI backpressure hit"      "[[ \$(metric ${CSV} fdi_backpressure_events) -gt 0 ]]"
check "T7 max occupancy == 4"              "[[ \$(metric ${CSV} max_retry_buffer_occupancy) -eq 4 ]]"
check "T7 zero integrity errors"           "[[ \$(metric ${CSV} integrity_errors) -eq 0 ]]"

# ------------------------------------------------------- T8: determinism
"${BIN}" --flits 800 --sigma 0.18 --seed 11 --csv "${OUT}/t8a.csv" > /dev/null 2>&1
"${BIN}" --flits 800 --sigma 0.18 --seed 11 --csv "${OUT}/t8b.csv" > /dev/null 2>&1
check "T8 same seed -> identical results"  "cmp -s ${OUT}/t8a.csv ${OUT}/t8b.csv"
"${BIN}" --flits 800 --sigma 0.18 --seed 12 --csv "${OUT}/t8c.csv" > /dev/null 2>&1
check "T8 different seed -> different run" "! cmp -s ${OUT}/t8a.csv ${OUT}/t8c.csv"

# ------------------------------------------------------- T9: 32 GT/s PAM4
CSV="${OUT}/t9.csv"
"${BIN}" --flits 1000 --rate-gtps 32 --sigma 0.05 --csv "${CSV}" > "${OUT}/t9.log" 2>&1
check "T9 exit 0"                          "[[ $? -eq 0 ]]"
check "T9 32GT/s delivered all"            "[[ \$(metric ${CSV} delivered_flits) -eq 1000 ]]"
check "T9 utilization >= 0.90"             "awk 'BEGIN{exit !($(metric ${CSV} bandwidth_utilization) >= 0.90)}'"

# -------------------------------- T10: external FDI payload + VC/transaction ID
# Input comes from a file instead of the deterministic generator. The memory
# endpoint echoes each payload through the independent reverse business link.
CSV="${OUT}/t10.csv"
RESP="${OUT}/t10_responses.csv"
"${BIN}" --format compact68 --payload-file "${ROOT_DIR}/tests/external_payloads.hex" \
  --response-file "${RESP}" --sigma 0 --jitter 0 --isi1 0 --isi2 0 --skew 0 \
  --csv "${CSV}" > "${OUT}/t10.log" 2>&1
check "T10 external payload: exit 0"        "[[ $? -eq 0 ]]"
check "T10 external mode recorded"          "[[ \$(metric ${CSV} input_mode) == external ]]"
check "T10 file count overrides --flits"    "[[ \$(metric ${CSV} flits_requested) -eq 3 ]]"
check "T10 all requests reached memory"     "[[ \$(metric ${CSV} requests_delivered) -eq 3 ]]"
check "T10 all responses returned"          "[[ \$(metric ${CSV} delivered_flits) -eq 3 ]]"
check "T10 forward link exercised"          "[[ \$(metric ${CSV} forward_tx_new_flits) -eq 3 ]]"
check "T10 reverse link exercised"          "[[ \$(metric ${CSV} reverse_tx_new_flits) -eq 3 ]]"
check "T10 payload/ID/VC round trip exact"  "cmp -s ${ROOT_DIR}/tests/external_payloads.expected ${RESP}"
check "T10 zero integrity errors"           "[[ \$(metric ${CSV} integrity_errors) -eq 0 ]]"

# ----------------------------------------- T11: forced CDR loss/relock coverage
CSV="${OUT}/t11.csv"
"${BIN}" --flits 400 --sigma 0.18 --cdr-ser 0 --cdr-bad-flits 1 \
  --cdr-relock 32 --cdr-noise-mult 1 --csv "${CSV}" > "${OUT}/t11.log" 2>&1
check "T11 CDR scenario: exit 0"             "[[ $? -eq 0 ]]"
check "T11 CDR lock loss triggered"          "[[ \$(metric ${CSV} cdr_lock_loss_count) -gt 0 ]]"
check "T11 degraded state reported"          "[[ \$(metric ${CSV} link_state) == degraded ]]"
check "T11 all responses delivered"          "[[ \$(metric ${CSV} delivered_flits) -eq 400 ]]"
check "T11 zero integrity errors"            "[[ \$(metric ${CSV} integrity_errors) -eq 0 ]]"

# ------------------------------------------------ T12: deskew failure/recovery
CSV="${OUT}/t12.csv"
"${BIN}" --flits 20 --lanes 1 --sigma 0 --jitter 0 --isi1 0 --isi2 0 \
  --skew 1 --deskew 0 --csv "${CSV}" > "${OUT}/t12.log" 2>&1
check "T12 deskew scenario: exit 0"          "[[ $? -eq 0 ]]"
check "T12 deskew failures observed"         "[[ \$(metric ${CSV} deskew_failures) -gt 0 ]]"
check "T12 replay recovered all traffic"     "[[ \$(metric ${CSV} delivered_flits) -eq 20 && \$(metric ${CSV} tx_replay_flits) -gt 0 ]]"
check "T12 zero integrity errors"            "[[ \$(metric ${CSV} integrity_errors) -eq 0 ]]"

# ----------------------------------------------- T13: 48 GT/s high-rate point
CSV="${OUT}/t13.csv"
"${BIN}" --flits 1000 --rate-gtps 48 --sigma 0 --jitter 0 --isi1 0 --isi2 0 --skew 0 \
  --csv "${CSV}" > "${OUT}/t13.log" 2>&1
check "T13 48GT/s scenario: exit 0"           "[[ $? -eq 0 ]]"
check "T13 rate recorded 48 GT/s"             "awk 'BEGIN{exit !($(metric ${CSV} lane_rate_gtps)==48)}'"
check "T13 capacity 1536 Gbps"                "awk 'BEGIN{x=$(metric ${CSV} capacity_gbps); exit !(x>1535.9 && x<1536.1)}'"
check "T13 utilization >= 0.90"               "awk 'BEGIN{exit !($(metric ${CSV} bandwidth_utilization)>=0.90)}'"
check "T13 bidirectional full load"           "[[ \$(metric ${CSV} forward_tx_new_flits) -eq 1000 && \$(metric ${CSV} reverse_tx_new_flits) -eq 1000 ]]"
check "T13 zero integrity errors"             "[[ \$(metric ${CSV} integrity_errors) -eq 0 ]]"

# -------------------------------------------- T14: watchdog / Failed state
CSV="${OUT}/t14.csv"
"${BIN}" --flits 100 --max-time 10 --csv "${CSV}" > "${OUT}/t14.log" 2>&1
rc=$?
check "T14 watchdog returns incomplete code"  "[[ ${rc} -eq 2 ]]"
check "T14 watchdog flag asserted"            "[[ \$(metric ${CSV} watchdog_fired) -eq 1 ]]"
check "T14 Failed link state reported"        "[[ \$(metric ${CSV} link_state) == failed ]]"
check "T14 traffic stopped incomplete"        "[[ \$(metric ${CSV} delivered_flits) -lt 100 ]]"
check "T14 no false integrity error"          "[[ \$(metric ${CSV} integrity_errors) -eq 0 ]]"

echo
echo "=============================="
echo "PASS=${PASS} FAIL=${FAIL}"
echo "=============================="
[[ ${FAIL} -eq 0 ]]
