#!/usr/bin/env bash
# Parameter sweep: rate x noise, plus a lane sweep, for the UCIe SystemC model.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="${ROOT_DIR}/build/ucie_sc_sim"
OUT="${ROOT_DIR}/results/sweep"
mkdir -p "${OUT}"

[[ -x "${BIN}" ]] || make -C "${ROOT_DIR}"

metric() { awk -F, -v k="$2" '$1==k{print $2}' "$1"; }

SUMMARY="${OUT}/summary.csv"
echo "rate_gtps,lanes,mod,sigma,jitter_ui,seed,delivered_flits,throughput_gbps,capacity_gbps,bandwidth_utilization,avg_latency_ns,p95_latency_ns,p99_latency_ns,nak_count,tx_replay_flits,symbol_error_rate,bit_error_rate,observed_flit_fail_rate,cdr_lock_loss_count,integrity_errors" > "${SUMMARY}"

row() {
  local csv="$1" rate="$2" lanes="$3" mod="$4" sigma="$5"
  local jitter="${6:-0.01}" seed="${7:-7}"
  echo "${rate},${lanes},${mod},${sigma},${jitter},${seed},$(metric "${csv}" delivered_flits),$(metric "${csv}" throughput_gbps),$(metric "${csv}" capacity_gbps),$(metric "${csv}" bandwidth_utilization),$(metric "${csv}" avg_latency_ns),$(metric "${csv}" p95_latency_ns),$(metric "${csv}" p99_latency_ns),$(metric "${csv}" nak_count),$(metric "${csv}" tx_replay_flits),$(metric "${csv}" symbol_error_rate),$(metric "${csv}" bit_error_rate),$(metric "${csv}" observed_flit_fail_rate),$(metric "${csv}" cdr_lock_loss_count),$(metric "${csv}" integrity_errors)" >> "${SUMMARY}"
}

# Rate x noise sweep (PAM4, 16 lanes)
for rate in 24 32 48; do
  for sigma in 0.00 0.05 0.10 0.15 0.18 0.20; do
    csv="${OUT}/pam4_r${rate}_s${sigma}.csv"
    "${BIN}" --flits 3000 --rate-gtps "${rate}" --lanes 16 --sigma "${sigma}" \
      --csv "${csv}" > "${OUT}/pam4_r${rate}_s${sigma}.log" 2>&1 || true
    row "${csv}" "${rate}" 16 pam4 "${sigma}"
  done
done

# Jitter x random-seed sweep at the minimum required 24 GT/s point.
for jitter in 0.00 0.01 0.03 0.05; do
  for seed in 7 11 29; do
    csv="${OUT}/jitter_${jitter}_seed_${seed}.csv"
    "${BIN}" --flits 1500 --rate-gtps 24 --lanes 16 --sigma 0.10 \
      --jitter "${jitter}" --seed "${seed}" --csv "${csv}" \
      > "${OUT}/jitter_${jitter}_seed_${seed}.log" 2>&1 || true
    row "${csv}" 24 16 pam4 0.10 "${jitter}" "${seed}"
  done
done

# Lane sweep at 24 GT/s, low noise
for lanes in 8 16 32 64; do
  csv="${OUT}/lanes_${lanes}.csv"
  "${BIN}" --flits 3000 --rate-gtps 24 --lanes "${lanes}" --sigma 0.05 \
    --csv "${csv}" > "${OUT}/lanes_${lanes}.log" 2>&1 || true
  row "${csv}" 24 "${lanes}" pam4 0.05
done

# NRZ reference points (real UCIe mainband signaling)
for rate in 24 32; do
  csv="${OUT}/nrz_r${rate}.csv"
  "${BIN}" --flits 3000 --rate-gtps "${rate}" --lanes 16 --mod nrz --sigma 0.15 \
    --csv "${csv}" > "${OUT}/nrz_r${rate}.log" 2>&1 || true
  row "${csv}" "${rate}" 16 nrz 0.15
done

echo "Sweep summary written to ${SUMMARY}"
column -t -s, "${SUMMARY}"
