#!/usr/bin/env bash
# CLI smoke regression：覆盖主要标准 preset、配置文件、trace、输出字段、
# command trace validation 和 timing table dump。精确协议断言放在
# tests/sequence_tests.cpp 中，这里保持快速、稳定、低维护成本。
set -euo pipefail

cd "$(dirname "$0")/.."
if [[ -z "${HBM_SIM_BIN:-}" ]]; then
  if [[ -x ./build/hbm_sim ]]; then
    HBM_SIM_BIN=./build/hbm_sim
  else
    HBM_SIM_BIN=./build-cmake/hbm_sim
  fi
fi

run_and_check() {
  local name="$1"
  shift
  local out
  # 捕获完整 stdout：既能打印给人看，也能用 grep 做字段检查。
  # 如果被测命令退出非 0，set -e 会让 smoke 立即失败。
  out="$("$@" --stats-view diagnostic)"
  echo "==========${name} ==========="
  echo "$out"
  # smoke 只检查关键字段是否存在；协议精确性由 sequence_tests.cpp 覆盖。
  # 不在这里绑定具体数值，是为了 timing preset 调整时不频繁维护 smoke。
  grep -Eq "^completed_reads[[:space:]]*:" <<<"$out"
  grep -Eq "^cycles[[:space:]]*:" <<<"$out"
  grep -Eq "^address_mapping[[:space:]]*:" <<<"$out"
  grep -Eq "^memory_image[[:space:]]*:" <<<"$out"
  grep -Eq "^memory_image_dump[[:space:]]*:" <<<"$out"
  grep -Eq "^memory_csv_dump[[:space:]]*:" <<<"$out"
  grep -Eq "^mismatch_report[[:space:]]*:" <<<"$out"
  grep -Eq "^thermal_map_dump[[:space:]]*:" <<<"$out"
  grep -Eq "^floorplan_enabled[[:space:]]*:" <<<"$out"
  grep -Eq "^power_model_enabled[[:space:]]*:" <<<"$out"
  grep -Eq "^thermal_model_enabled[[:space:]]*:" <<<"$out"
  grep -Eq "^power_source[[:space:]]*:" <<<"$out"
  grep -Eq "^power_scale[[:space:]]*:" <<<"$out"
  grep -Eq "^thermal_ambient_C[[:space:]]*:" <<<"$out"
  grep -Eq "^thermal_cooling_per_cycle[[:space:]]*:" <<<"$out"
  grep -Eq "^thermal_rise_C_per_pJ[[:space:]]*:" <<<"$out"
  grep -Eq "^thermal_grid_cols_tile[[:space:]]*:" <<<"$out"
  grep -Eq "^thermal_grid_rows_tile[[:space:]]*:" <<<"$out"
  grep -Eq "^thermal_coupling[[:space:]]*:" <<<"$out"
  grep -Eq "^thermal_tsvs_per_grid[[:space:]]*:" <<<"$out"
  grep -Eq "^subarrays_per_bank[[:space:]]*:" <<<"$out"
  grep -Eq "^mats_per_subarray_x[[:space:]]*:" <<<"$out"
  grep -Eq "^microbumps_x[[:space:]]*:" <<<"$out"
  grep -Eq "^ecc_shadow[[:space:]]*:" <<<"$out"
  grep -Eq "^ecc_inject_period[[:space:]]*:" <<<"$out"
  grep -Eq "^power_act_pJ[[:space:]]*:" <<<"$out"
  grep -Eq "^power_read_pJ[[:space:]]*:" <<<"$out"
  grep -Eq "^power_write_pJ[[:space:]]*:" <<<"$out"
  grep -Eq "^timing_source_jedec[[:space:]]*:" <<<"$out"
  grep -Eq "^timing_profile[[:space:]]*:" <<<"$out"
  ! grep -Eq "^timing_profile_file[[:space:]]*:" <<<"$out"
  grep -Eq "^init_sequence[[:space:]]*:" <<<"$out"
  grep -Eq "^init_sequence_interval[[:space:]]*:" <<<"$out"
  grep -Eq "^speed_bin_mbps[[:space:]]*:" <<<"$out"
  grep -Eq "^hbm_pairing_matrix[[:space:]]*:" <<<"$out"
  grep -Eq "^lpddr_wck_mode[[:space:]]*:" <<<"$out"
  grep -Eq "^lpddr_wck_training[[:space:]]*:" <<<"$out"
  grep -Eq "^lpddr_ca_parity[[:space:]]*:" <<<"$out"
  grep -Eq "^refresh_temperature[[:space:]]*:" <<<"$out"
  grep -Eq "^system_cycles[[:space:]]*:" <<<"$out"
  grep -Eq "^aggregate_ctrl_cycles[[:space:]]*:" <<<"$out"
  grep -Eq "^if_xfer_rate_Gbps[[:space:]]*:" <<<"$out"
  grep -Eq "^peak_bandwidth_GBps[[:space:]]*:" <<<"$out"
  grep -Eq "^achieved_if_bw_GBps[[:space:]]*:" <<<"$out"
  grep -Eq "^bandwidth_util_pct[[:space:]]*:" <<<"$out"
  grep -Eq "^payload_efficiency_pct[[:space:]]*:" <<<"$out"
  grep -Eq "^interface_command_bits[[:space:]]*:" <<<"$out"
  grep -Eq "^interface_overhead_bits[[:space:]]*:" <<<"$out"
  grep -Eq "^data_checked_reads[[:space:]]*:" <<<"$out"
  grep -Eq "^data_mismatches[[:space:]]*:" <<<"$out"
  grep -Eq "^data_write_commits[[:space:]]*:" <<<"$out"
  grep -Eq "^storage_lines_allocated[[:space:]]*:" <<<"$out"
  grep -Eq "^unique_written_lines[[:space:]]*:" <<<"$out"
  grep -Eq "^storage_topology_lines_scanned[[:space:]]*:" <<<"$out"
  grep -Eq "^storage_topology_scan_skipped[[:space:]]*:" <<<"$out"
  grep -Eq "^request_source[[:space:]]*: streaming" <<<"$out"
  grep -Eq "^command_trace_retained[[:space:]]*:" <<<"$out"
  grep -Eq "^dfi_validation[[:space:]]*:" <<<"$out"
  grep -Eq "^dfi_validation_events[[:space:]]*:" <<<"$out"
  grep -Eq "^dfi_validation_expected_checks[[:space:]]*:" <<<"$out"
  grep -Eq "^memory_backend[[:space:]]*:" <<<"$out"
  grep -Eq "^memory_backend_line_bytes[[:space:]]*:" <<<"$out"
  grep -Eq "^host_requests[[:space:]]*:" <<<"$out"
  grep -Eq "^dram_transactions[[:space:]]*:" <<<"$out"
  grep -Eq "^storage_backend_recommendation[[:space:]]*:" <<<"$out"
  grep -Eq "^storage_channels_touched[[:space:]]*:" <<<"$out"
  grep -Eq "^storage_banks_touched[[:space:]]*:" <<<"$out"
  grep -Eq "^storage_rows_touched[[:space:]]*:" <<<"$out"
  grep -Eq "^storage_subarrays_touched[[:space:]]*:" <<<"$out"
  grep -Eq "^storage_mats_touched[[:space:]]*:" <<<"$out"
  grep -Eq "^storage_microbumps_touched[[:space:]]*:" <<<"$out"
  grep -Eq "^storage_write_line_accesses[[:space:]]*:" <<<"$out"
  grep -Eq "^floorplan_tiles_touched[[:space:]]*:" <<<"$out"
  grep -Eq "^thermal_tiles_touched[[:space:]]*:" <<<"$out"
  grep -Eq "^thermal_grid_cells_touched[[:space:]]*:" <<<"$out"
  grep -Eq "^rowbuf_activations[[:space:]]*:" <<<"$out"
  grep -Eq "^rowbuf_precharges[[:space:]]*:" <<<"$out"
  grep -Eq "^rowbuf_dirty_writebacks[[:space:]]*:" <<<"$out"
  grep -Eq "^rowbuf_reads[[:space:]]*:" <<<"$out"
  grep -Eq "^rowbuf_writes[[:space:]]*:" <<<"$out"
  grep -Eq "^power_events[[:space:]]*:" <<<"$out"
  grep -Eq "^power_energy_pJ[[:space:]]*:" <<<"$out"
  grep -Eq "^thermal_peak_temp_C[[:space:]]*:" <<<"$out"
  grep -Eq "^thermal_vertical_transfers[[:space:]]*:" <<<"$out"
  grep -Eq "^thermal_tsv_transfers[[:space:]]*:" <<<"$out"
  grep -Eq "^ecc_shadow_updates[[:space:]]*:" <<<"$out"
  grep -Eq "^ecc_checked_reads[[:space:]]*:" <<<"$out"
  grep -Eq "^dfi_phase_count[[:space:]]*:" <<<"$out"
  grep -Eq "^dfi_data_lane_bytes[[:space:]]*:" <<<"$out"
  grep -Eq "^dfi_read_latency_nck[[:space:]]*:" <<<"$out"
  grep -Eq "^dfi_write_latency_nck[[:space:]]*:" <<<"$out"
  grep -Eq "^dfi_read_beats[[:space:]]*:" <<<"$out"
  grep -Eq "^dfi_write_beats[[:space:]]*:" <<<"$out"
  grep -Eq "^dfi_beat_bytes[[:space:]]*:" <<<"$out"
  grep -Eq "^lpddr_metadata_bits_req[[:space:]]*:" <<<"$out"
  grep -Eq "^commands.MRW[[:space:]]*:" <<<"$out"
  grep -Eq "^commands.WCK_TRAIN[[:space:]]*:" <<<"$out"
  grep -Eq "^cmd_validation_checked[[:space:]]*:" <<<"$out"
  grep -Eq "^cmd_validation_pair_checks[[:space:]]*:" <<<"$out"
  grep -Eq "^cmd_validation_state_checks[[:space:]]*:" <<<"$out"
  grep -Eq "^cmd_validation_timing_checks[[:space:]]*:" <<<"$out"
  grep -Eq "^dfi_trace[[:space:]]*:" <<<"$out"
  grep -Eq "^dfi_signal_trace[[:space:]]*:" <<<"$out"
  grep -Eq "^response_trace[[:space:]]*:" <<<"$out"
  grep -Eq "^transaction_response_trace[[:space:]]*:" <<<"$out"
  grep -Eq "^response_delivery_mode[[:space:]]*:" <<<"$out"
  grep -Eq "^host_response_queue_capacity[[:space:]]*:" <<<"$out"
  grep -Eq "^transaction_response_queue_capacity[[:space:]]*:" <<<"$out"
  grep -Eq "^host_responses_consumed[[:space:]]*:" <<<"$out"
  grep -Eq "^host_responses_exported[[:space:]]*:" <<<"$out"
  grep -Eq "^transaction_responses_consumed[[:space:]]*:" <<<"$out"
  grep -Eq "^transaction_responses_exported[[:space:]]*:" <<<"$out"
}

# 覆盖 HBM 双总线、随机/顺序流量，以及 LPDDR split activate + WCK/CAS 路径。
# 这些用例刻意保持请求数较小，作为快速回归，而不是性能 benchmark。
run_and_check hbm4_stream "$HBM_SIM_BIN" --stats-view diagnostic --standard hbm4 --pattern stream --requests 2000 --read-ratio 90
run_and_check hbm3_random "$HBM_SIM_BIN" --stats-view diagnostic --standard hbm3 --pattern random --requests 2000 --read-ratio 75 --seed 11
run_and_check lpddr6_stream "$HBM_SIM_BIN" --stats-view diagnostic --standard lpddr6 --pattern stream --requests 2000
run_and_check lpddr5_stream "$HBM_SIM_BIN" --stats-view diagnostic --standard lpddr5 --pattern stream --requests 2000
run_and_check lpddr6_trace "$HBM_SIM_BIN" --stats-view diagnostic --standard lpddr6 --trace examples/sample.trace --requests 0
run_and_check lpddr6_timed_trace "$HBM_SIM_BIN" --stats-view diagnostic --standard lpddr6 --trace examples/timed.trace --requests 0
run_and_check hbm4_data_trace "$HBM_SIM_BIN" --stats-view diagnostic --standard hbm4 --trace examples/data_check.trace \
  --requests 0 --max-cycles 4000

ordering_dir="$(mktemp -d)"
printf '%s\n' '0x1000 data=00112233' >"$ordering_dir/init.txt"
printf '%s\n' \
  '0 R 0x1000 expect=00112233' \
  '1 W 0x1000 data=aabbccdd' >"$ordering_dir/ordering.trace"
ordering_out="$(
  "$HBM_SIM_BIN" --stats-view diagnostic --standard hbm4 --single-controller \
    --memory-image "$ordering_dir/init.txt" \
    --trace "$ordering_dir/ordering.trace" --requests 0 \
    --cmd-trace "$ordering_dir/commands.csv" --validate-dfi-trace
)"
grep -Eq "^data_mismatches[[:space:]]*: 0" <<<"$ordering_out"
grep -Eq "^dfi_validation_expected_checks[[:space:]]*: [1-9]" <<<"$ordering_out"
ordering_rd="$(awk -F, '$3 == "RD" || $3 == "RDA" {print $1; exit}' "$ordering_dir/commands.csv")"
ordering_wr="$(awk -F, '$3 == "WR" || $3 == "WRA" {print $1; exit}' "$ordering_dir/commands.csv")"
[[ -n "$ordering_rd" && -n "$ordering_wr" && "$ordering_rd" -lt "$ordering_wr" ]]

printf '%s\n' '0 R 0x1000 expect=ffffffff' >"$ordering_dir/wrong.trace"
set +e
"$HBM_SIM_BIN" --stats-view diagnostic --standard hbm4 --single-controller \
  --memory-image "$ordering_dir/init.txt" \
  --trace "$ordering_dir/wrong.trace" --requests 0 \
  >"$ordering_dir/wrong.out" 2>"$ordering_dir/wrong.err"
wrong_rc=$?
set -e
[[ "$wrong_rc" -eq 3 ]]
grep -Eq "^data_mismatches[[:space:]]*: 1" "$ordering_dir/wrong.out"
"$HBM_SIM_BIN" --stats-view diagnostic --standard hbm4 --single-controller \
  --memory-image "$ordering_dir/init.txt" \
  --trace "$ordering_dir/wrong.trace" --requests 0 \
  --allow-data-mismatch >"$ordering_dir/allowed.out"
grep -Eq "^data_mismatches[[:space:]]*: 1" "$ordering_dir/allowed.out"
memory_dump="$(mktemp)"
memory_csv="$(mktemp)"
mismatch_report="$(mktemp)"
thermal_map="$(mktemp)"
run_and_check hbm4_memory_image "$HBM_SIM_BIN" --stats-view diagnostic --standard hbm4 \
  --memory-image examples/memory_image.txt --trace examples/data_check.trace \
  --requests 0 --max-cycles 4000 \
  --dump-memory-image "$memory_dump" --dump-memory-csv "$memory_csv" --mismatch-report "$mismatch_report" \
  --dump-thermal-map "$thermal_map"
grep -Eq "^0x" "$memory_dump"
grep -Eq "^address,data,init,initialized" "$memory_csv"
grep -Eq "00112233a155a377a5ada7efccddeeff" "$memory_csv"
grep -Eq "^# cycle request_id address" "$mismatch_report"
grep -Eq "^# stack layer thermal_x thermal_y" "$thermal_map"
rm -f "$memory_dump" "$memory_csv" "$mismatch_report" "$thermal_map"

# 用户输出路径的父目录应由 CLI 自动创建，避免每个实验脚本重复 mkdir。
auto_output_root="$(mktemp -d)"
rm -rf "$auto_output_root"

# CLI 的异步路径必须逐拍接受普通请求和独立 maintenance，并在线消费完整
# HostResponse；CSV 行数应等于 host 请求数，而不是拆分后的 DRAM transaction 数。
response_dir="$(mktemp -d)"
response_out="$response_dir/run.out"
response_csv="$response_dir/host_responses.csv"
"$HBM_SIM_BIN" --stats-view diagnostic --standard hbm4 --requests 4 --read-ratio 100 \
  --init-sequence hbm4 --max-cycles 10000 \
  --response-trace "$response_csv" >"$response_out"
grep -Eq '^host_request_id,type,system_address,transaction_count' "$response_csv"
[[ "$(($(wc -l <"$response_csv") - 1))" -eq 4 ]]
grep -Eq '^host_responses_exported[[:space:]]*: 4' "$response_out"
grep -Eq '^remaining_requests[[:space:]]*: 0' "$response_out"
grep -Eq '^hit_cycle_limit[[:space:]]*: false' "$response_out"

# Both 模式同时导出乱序子事务和重组后的 HostResponse。容量 1 会把 ready
# 约束传播到完成收集；CLI 每拍在线消费，因此不会靠无限队列掩盖死锁。
transaction_csv="$response_dir/transaction_responses.csv"
progress_log="$response_dir/progress.log"
"$HBM_SIM_BIN" --stats-view diagnostic --standard hbm4 --requests 4 --read-ratio 100 \
  --max-cycles 10000 --stats-view diagnostic --progress-interval 20 \
  --stats-json "$response_dir/result.json" \
  --response-delivery-mode both --host-response-queue-capacity 1 \
  --transaction-response-queue-capacity 1 \
  --response-trace "$response_csv" \
  --transaction-response-trace "$transaction_csv" \
  >"$response_out" 2>"$progress_log"
[[ "$(($(wc -l <"$response_csv") - 1))" -eq 4 ]]
[[ "$(($(wc -l <"$transaction_csv") - 1))" -eq 8 ]]
grep -Eq '^request_id,host_request_id,transaction_index,transaction_count' \
  "$transaction_csv"
grep -Eq '^run_status[[:space:]]*: completed$' "$response_out"
python3 - "$response_dir/result.json" <<'PY'
import json, sys
result = json.load(open(sys.argv[1]))
assert result['run_status'] == 'completed'
metrics = result['diagnostics']
assert metrics['response_delivery_mode'] == 'both'
assert metrics['host_responses_consumed'] == 4
assert metrics['transaction_responses_consumed'] == 8
PY
grep -Eq '^progress cycle=[0-9]+ completed_reads=' "$progress_log"
if "$HBM_SIM_BIN" --stats-view diagnostic --standard hbm4 --response-delivery-mode transaction \
    --response-trace "$response_csv" --check-config \
    >"$response_dir/incompatible.out" 2>&1; then
  echo "expected incompatible host trace/transaction response mode to fail" >&2
  exit 1
fi
grep -F 'host response_trace requires response_delivery_mode=host or both' \
  "$response_dir/incompatible.out" >/dev/null
rm -rf "$response_dir"

bounded_random_out="$(mktemp)"
"$HBM_SIM_BIN" --stats-view diagnostic --standard hbm4 --pattern random --requests 16 \
  --random-address-space-bytes 4096 --stats-view diagnostic \
  >"$bounded_random_out"
grep -Eq '^random_address_space_bytes[[:space:]]*: 4096$' "$bounded_random_out"
grep -Eq '^remaining_requests[[:space:]]*: 0$' "$bounded_random_out"
rm -f "$bounded_random_out"

# Maintenance 与 burst trace 必须采用和普通请求相同的严格非负解析规则。
invalid_trace="$(mktemp)"
printf '0 M REFab 0x0 stack=-1\n' >"$invalid_trace"
if "$HBM_SIM_BIN" --stats-view diagnostic --standard hbm4 --trace "$invalid_trace" --requests 0 \
    >/tmp/hbm_sim_bad_trace.out 2>&1; then
  echo "expected a negative maintenance stack id to fail" >&2
  exit 1
fi
grep -F "has invalid stack id: -1" /tmp/hbm_sim_bad_trace.out >/dev/null
printf '0 BR 0x0 len=64garbage\n' >"$invalid_trace"
if "$HBM_SIM_BIN" --stats-view diagnostic --standard hbm4 --trace "$invalid_trace" --requests 0 \
    >/tmp/hbm_sim_bad_trace.out 2>&1; then
  echo "expected a malformed burst length to fail" >&2
  exit 1
fi
grep -F "has invalid burst length: 64garbage" /tmp/hbm_sim_bad_trace.out >/dev/null
rm -f "$invalid_trace" /tmp/hbm_sim_bad_trace.out
"$HBM_SIM_BIN" --stats-view diagnostic --standard hbm4 --requests 2 \
  --cmd-trace "$auto_output_root/nested/commands.csv" \
  --dfi-trace "$auto_output_root/nested/dfi.csv" \
  --dump-timing-table "$auto_output_root/nested/timing.csv" >/dev/null
[[ -s "$auto_output_root/nested/commands.csv" ]]
[[ -s "$auto_output_root/nested/dfi.csv" ]]
[[ -s "$auto_output_root/nested/timing.csv" ]]
rm -rf "$auto_output_root"

memory_bin="$(mktemp /tmp/hbm_sim_memory.XXXXXX.bin)"
golden_ok="$(mktemp)"
"$HBM_SIM_BIN" --stats-view diagnostic --standard hbm4 --trace examples/data_check.trace \
  --requests 0 --max-cycles 4000 --dump-memory-image "$memory_bin" >/dev/null
"$HBM_SIM_BIN" --stats-view diagnostic --standard hbm4 --memory-image "$memory_bin" \
  --requests 0 --max-cycles 4000 --verify-golden "$memory_bin" >"$golden_ok"
grep -Eq "^golden_verified[[:space:]]*: 1" "$golden_ok"
grep -Eq "^golden_mismatches[[:space:]]*: 0" "$golden_ok"
grep -Eq "^golden_uninitialized_lines[[:space:]]*:" "$golden_ok"
grep -Eq "^golden_actual_uninitialized_lines[[:space:]]*:" "$golden_ok"

partial_zero_trace="$(mktemp)"
partial_zero_bin="$(mktemp /tmp/hbm_sim_partial_zero.XXXXXX.bin)"
golden_bad_out="$(mktemp)"
golden_bad_err="$(mktemp)"
printf 'W 0x1000 data=00000000\n' >"$partial_zero_trace"
"$HBM_SIM_BIN" --stats-view diagnostic --standard hbm4 --trace "$partial_zero_trace" \
  --requests 0 --max-cycles 4000 --dump-memory-image "$partial_zero_bin" >/dev/null
if "$HBM_SIM_BIN" --stats-view diagnostic --standard hbm4 --requests 0 --max-cycles 4000 \
    --verify-golden "$partial_zero_bin" >"$golden_bad_out" 2>"$golden_bad_err"; then
  cat "$golden_bad_out"
  cat "$golden_bad_err"
  echo "expected golden verification to fail on init-mask mismatch" >&2
  exit 1
fi
grep -Eq "^golden_mismatches[[:space:]]*: 1" "$golden_bad_out"
grep -Eq "actual_init_mask" "$golden_bad_err"
grep -Eq "golden verification failed" "$golden_bad_err"
rm -f "$memory_bin" "$golden_ok" "$partial_zero_trace" "$partial_zero_bin" \
  "$golden_bad_out" "$golden_bad_err"

run_and_check hbm4_storage_model_cli "$HBM_SIM_BIN" --stats-view diagnostic --standard hbm4 \
  --requests 64 --power-scale 1.25 --thermal-ambient-c 37 \
  --thermal-cooling-per-cycle 0.0001 --thermal-rise-c-per-pj 0.00003 \
  --thermal-coupling true --thermal-tsvs-per-grid 8 \
  --subarrays-per-bank 8 --mats-per-subarray-x 2 --mats-per-subarray-y 2 \
  --microbumps-x 4 --microbumps-y 4 --ecc-inject-period 0
if "$HBM_SIM_BIN" --stats-view diagnostic --standard hbm4 --requests 1 --power-scale -1 >/tmp/hbm_sim_bad_power_scale.out 2>&1; then
  cat /tmp/hbm_sim_bad_power_scale.out
  echo "expected negative power scale to fail" >&2
  exit 1
fi
grep -Eq "power_scale must be >= 0" /tmp/hbm_sim_bad_power_scale.out
rm -f /tmp/hbm_sim_bad_power_scale.out

# 无符号选项不能把 -1 包装成 UINT64_MAX；整数/浮点参数也必须完整消费
# 输入 token，且拒绝 NaN/Inf，避免错误配置生成貌似正常的统计。
if "$HBM_SIM_BIN" --stats-view diagnostic --standard hbm4 --requests -1 --check-config \
    >/tmp/hbm_sim_bad_unsigned.out 2>&1; then
  echo "expected a negative unsigned option to fail" >&2
  exit 1
fi
grep -F "invalid non-negative integer: -1" /tmp/hbm_sim_bad_unsigned.out >/dev/null
if "$HBM_SIM_BIN" --stats-view diagnostic --standard hbm4 --read-ratio 50garbage --check-config \
    >/tmp/hbm_sim_bad_integer.out 2>&1; then
  echo "expected an integer with a trailing suffix to fail" >&2
  exit 1
fi
grep -F "invalid integer: 50garbage" /tmp/hbm_sim_bad_integer.out >/dev/null
if "$HBM_SIM_BIN" --stats-view diagnostic --standard hbm4 --power-scale nan --check-config \
    >/tmp/hbm_sim_bad_float.out 2>&1; then
  echo "expected a non-finite floating-point option to fail" >&2
  exit 1
fi
grep -F "invalid finite number: nan" /tmp/hbm_sim_bad_float.out >/dev/null
rm -f /tmp/hbm_sim_bad_unsigned.out /tmp/hbm_sim_bad_integer.out \
  /tmp/hbm_sim_bad_float.out
run_and_check lpddr6_efficiency "$HBM_SIM_BIN" --stats-view diagnostic --standard lpddr6 --requests 128 \
  --lpddr-efficiency static --metadata-bits-per-request 16
run_and_check lpddr6_link_cli "$HBM_SIM_BIN" --stats-view diagnostic --standard lpddr6 --requests 32 \
  --lpddr-dbi true --lpddr-dbi-bits 8 --lpddr-link-protection true \
  --lpddr-link-ecc true --lpddr-link-ecc-bits 16 --low-power power_down \
  --low-power-entry-cycles 2 --low-power-exit-cycles 4
run_and_check hbm4_link_crc_cli "$HBM_SIM_BIN" --stats-view diagnostic --standard hbm4 --requests 32 \
  --hbm-link-crc crc16 --hbm-link-crc-bits 16 --hbm-ras-metadata-bits 16 \
  --hbm-link-retry true --refresh-postpone-limit 2 --refresh-pullin-limit 1 \
  --refresh-credit-limit 4

# schema-v2 主配置：四种标准均可选择；CLI 始终高于配置文件且与参数顺序无关。
config_tmp_dir="$(mktemp -d)"
for standard in hbm3 hbm4; do
  "$HBM_SIM_BIN" --stats-view diagnostic --config configs/hbm.cfg --standard "$standard" --check-config \
    | grep -Eq '^config_validation=pass$'
  "$HBM_SIM_BIN" --stats-view diagnostic --config configs/hbm.cfg --standard "$standard" \
    --dump-resolved-config "$config_tmp_dir/resolved_${standard}.cfg" --check-config >/dev/null
  "$HBM_SIM_BIN" --stats-view diagnostic --config "$config_tmp_dir/resolved_${standard}.cfg" --check-config \
    | grep -Eq '^config_validation=pass$'
done
for standard in lpddr5 lpddr6; do
  "$HBM_SIM_BIN" --stats-view diagnostic --config configs/lpddr.cfg --standard "$standard" --check-config \
    | grep -Eq '^config_validation=pass$'
  "$HBM_SIM_BIN" --stats-view diagnostic --config configs/lpddr.cfg --standard "$standard" \
    --dump-resolved-config "$config_tmp_dir/resolved_${standard}.cfg" --check-config >/dev/null
  "$HBM_SIM_BIN" --stats-view diagnostic --config "$config_tmp_dir/resolved_${standard}.cfg" --check-config \
    | grep -Eq '^config_validation=pass$'
done
"$HBM_SIM_BIN" --stats-view diagnostic --requests 5 --config configs/hbm.cfg --standard hbm4 \
  --dump-resolved-config "$config_tmp_dir/resolved.cfg" --check-config >/dev/null
grep -Eq '^requests[[:space:]]*=[[:space:]]*5$' "$config_tmp_dir/resolved.cfg"
"$HBM_SIM_BIN" --stats-view diagnostic --config "$config_tmp_dir/resolved.cfg" --requests 1 \
  --max-cycles 100000 --check-config | grep -Eq '^config_validation=pass$'

printf '%s\n' '[controller.scheduler]' 'type = not_implemented' \
  >"$config_tmp_dir/unknown_scheduler.cfg"
if "$HBM_SIM_BIN" --stats-view diagnostic --config configs/hbm.cfg \
    --config "$config_tmp_dir/unknown_scheduler.cfg" --check-config \
    >"$config_tmp_dir/unknown_scheduler.out" 2>&1; then
  echo "expected an unknown scheduler to fail" >&2
  exit 1
fi
grep -F "invalid scheduler: not_implemented" "$config_tmp_dir/unknown_scheduler.out" >/dev/null
grep -F "implemented: fcfs, frfcfs" "$config_tmp_dir/unknown_scheduler.out" >/dev/null

if "$HBM_SIM_BIN" --stats-view diagnostic --config configs/hbm.cfg --standard hbm4 \
    --preset does_not_exist --check-config >"$config_tmp_dir/unknown_preset.out" 2>&1; then
  echo "expected an unknown preset to fail" >&2
  exit 1
fi
grep -F "preset 'does_not_exist' does not exist" "$config_tmp_dir/unknown_preset.out" >/dev/null

printf '%s\n' '[override]' 'channels = 24' 'nCL = 28' \
  >"$config_tmp_dir/exploration.cfg"
"$HBM_SIM_BIN" --stats-view diagnostic --config configs/hbm.cfg --config "$config_tmp_dir/exploration.cfg" \
  --validation-mode exploratory --check-config | grep -Eq '^config_validation=pass$'
if "$HBM_SIM_BIN" --stats-view diagnostic --config configs/hbm.cfg \
    --config "$config_tmp_dir/exploration.cfg" --validation-mode standard --check-config \
    >"$config_tmp_dir/standard_override.out" 2>&1; then
  echo "expected standard mode to reject preset deviations" >&2
  exit 1
fi
grep -F "strict conformance mode rejects" "$config_tmp_dir/standard_override.out" >/dev/null
# Behavioral training/recovery defaults are not a verified JEDEC timing table.
if "$HBM_SIM_BIN" --stats-view diagnostic --config configs/lpddr.cfg --standard lpddr6 \
    --validation-mode standard --check-config >"$config_tmp_dir/lpddr_research.out" 2>&1; then
  echo "expected standard mode to reject LPDDR6 research control timing" >&2
  exit 1
fi
grep -F 'standard mode requires all model-required timing' \
  "$config_tmp_dir/lpddr_research.out" >/dev/null
if "$HBM_SIM_BIN" --stats-view diagnostic --config configs/lpddr.cfg --standard lpddr6 \
    --validation-mode device --check-config >"$config_tmp_dir/device_without_vendor.out" 2>&1; then
  echo "expected device mode without vendor timing to fail" >&2
  exit 1
fi
grep -F "requires a named vendor_profile and vendor-sourced timing" \
  "$config_tmp_dir/device_without_vendor.out" >/dev/null

printf '%s\n' '[override]' 'lpddr_ca_parity_enabled = false' \
  >"$config_tmp_dir/wrong_family.cfg"
if "$HBM_SIM_BIN" --stats-view diagnostic --config configs/hbm.cfg --config "$config_tmp_dir/wrong_family.cfg" \
    --check-config >"$config_tmp_dir/wrong_family.out" 2>&1; then
  echo "expected an LPDDR-only key in an HBM model to fail" >&2
  exit 1
fi
grep -F "has no executable semantics for HBM4" "$config_tmp_dir/wrong_family.out" >/dev/null
rm -rf "$config_tmp_dir"

run_and_check hbm4_config "$HBM_SIM_BIN" --stats-view diagnostic --config configs/hbm.cfg --standard hbm4 --requests 128
multistack_out="$(mktemp)"
"$HBM_SIM_BIN" --stats-view diagnostic --config configs/hbm.cfg --standard hbm4 --stack-count 6 \
  --requests 24 >"$multistack_out"
grep -Eq "^stack_count[[:space:]]*: 6" "$multistack_out"
grep -Eq "^active_stacks[[:space:]]*: 6" "$multistack_out"
grep -Eq "^controller_count[[:space:]]*: 192" "$multistack_out"
grep -Eq "^stack_5_reads[[:space:]]*: [1-9]" "$multistack_out"
rm -f "$multistack_out"
multistack_qos_out="$(mktemp)"
"$HBM_SIM_BIN" --stats-view diagnostic --config configs/hbm.cfg --standard hbm4 --stack-count 6 \
  --trace examples/multistack_qos.trace --requests 0 --max-cycles 4000 >"$multistack_qos_out"
grep -Eq "^active_stacks[[:space:]]*: 2" "$multistack_qos_out"
grep -Eq "^qos_priority_dispatches[[:space:]]*: [1-9]" "$multistack_qos_out"
grep -Eq "^data_mismatches[[:space:]]*: 0" "$multistack_qos_out"
rm -f "$multistack_qos_out"

# 不能只用 qos_priority_dispatches 证明发生了优先级重排；
# 用同一 Stack/同一拍的确定 trace 直接检查 host 完成顺序。
qos_order_dir="$(mktemp -d)"
for qos_policy in fcfs strict_priority; do
  "$HBM_SIM_BIN" --stats-view diagnostic --config configs/hbm.cfg --standard hbm4 \
    --trace examples/qos_priority.trace --requests 0 --max-cycles 4000 \
    --stack-dispatch-width 1 --stack-qos-policy "$qos_policy" \
    --response-trace "$qos_order_dir/$qos_policy.csv" >/dev/null
done
fcfs_order="$(tail -n +2 "$qos_order_dir/fcfs.csv" | cut -d, -f1 | paste -sd, -)"
priority_order="$(tail -n +2 "$qos_order_dir/strict_priority.csv" | cut -d, -f1 | paste -sd, -)"
test "$fcfs_order" = "0,1,2,3"
test "$priority_order" = "2,3,1,0"
rm -rf "$qos_order_dir"

run_and_check hbm4_storage_config "$HBM_SIM_BIN" --stats-view diagnostic --config configs/hbm.cfg \
  --standard hbm4 --requests 128
backend_dir="$(mktemp -d)"
run_and_check hbm4_mmap_backend "$HBM_SIM_BIN" --stats-view diagnostic --standard hbm4 \
  --trace examples/data_check.trace --requests 0 --max-cycles 4000 \
  --memory-image examples/memory_image.txt --memory-backend mmap_sparse \
  --memory-capacity-bytes 1048576 --memory-data-file "$backend_dir/mmap.bin"
run_and_check hbm4_chunk_backend "$HBM_SIM_BIN" --stats-view diagnostic --standard hbm4 \
  --trace examples/data_check.trace --requests 0 --max-cycles 4000 \
  --memory-image examples/memory_image.txt --memory-backend chunk_file \
  --memory-capacity-bytes 1048576 --memory-data-file "$backend_dir/chunk.bin" \
  --memory-chunk-size 65536 --memory-chunk-cache-entries 2
backend_golden="$backend_dir/golden.bin"
"$HBM_SIM_BIN" --stats-view diagnostic --standard hbm4 --trace examples/data_check.trace --requests 0 \
  --max-cycles 4000 --memory-image examples/memory_image.txt \
  --dump-memory-image "$backend_golden" >/dev/null
backend_verify="$backend_dir/verify.out"
"$HBM_SIM_BIN" --stats-view diagnostic --standard hbm4 --requests 0 --max-cycles 4000 \
  --memory-backend chunk_file --memory-capacity-bytes 1048576 \
  --memory-data-file "$backend_dir/chunk.bin" --memory-chunk-size 65536 \
  --memory-chunk-cache-entries 2 --verify-golden "$backend_golden" >"$backend_verify"
grep -Eq "^golden_verified[[:space:]]*: 2" "$backend_verify"
grep -Eq "^golden_mismatches[[:space:]]*: 0" "$backend_verify"
rm -rf "$backend_dir"
run_and_check hbm4_dramsim3_idd_grid "$HBM_SIM_BIN" --stats-view diagnostic --config configs/hbm.cfg --standard hbm4 \
  --requests 64 --power-source dramsim3_idd \
  --thermal-grid-cols-per-tile 4 --thermal-grid-rows-per-tile 4
if "$HBM_SIM_BIN" --stats-view diagnostic --standard hbm4 --requests 1 \
  --power-source dramsim3_idd --idd-devices-per-rank 0 \
  >/tmp/hbm_sim_bad_idd_devices.out 2>&1; then
  cat /tmp/hbm_sim_bad_idd_devices.out
  echo "expected zero IDD devices_per_rank to fail" >&2
  exit 1
fi
grep -Eq "idd_devices_per_rank must be > 0 for IDD power mode" \
  /tmp/hbm_sim_bad_idd_devices.out
rm -f /tmp/hbm_sim_bad_idd_devices.out
run_and_check hbm3_config "$HBM_SIM_BIN" --stats-view diagnostic --config configs/hbm.cfg --standard hbm3 --requests 128
run_and_check lpddr5_config "$HBM_SIM_BIN" --stats-view diagnostic --config configs/lpddr.cfg --standard lpddr5 --requests 128
run_and_check lpddr6_config "$HBM_SIM_BIN" --stats-view diagnostic --config configs/lpddr.cfg --standard lpddr6 --requests 128
run_and_check hbm4_jedec_template "$HBM_SIM_BIN" --stats-view diagnostic --config configs/hbm.cfg \
  --standard hbm4 --requests 64
run_and_check hbm4_synthetic_research "$HBM_SIM_BIN" --stats-view diagnostic \
  --config experiments/local/hx_hbm4_9000_48gb_16hi.cfg --requests 32 --stats-view diagnostic
run_and_check hbm3_reference "$HBM_SIM_BIN" --stats-view diagnostic --config configs/validation/hbm3.cfg --standard hbm3 \
  --preset ramulator2_reference_1ch --requests 64
run_and_check lpddr5_reference "$HBM_SIM_BIN" --stats-view diagnostic --config configs/validation/lpddr5.cfg --standard lpddr5 \
  --preset ramulator2_reference_1ch --requests 64
run_and_check lpddr6_link_protection "$HBM_SIM_BIN" --stats-view diagnostic --config configs/lpddr.cfg \
  --standard lpddr6 --preset link_protection --requests 64
run_and_check lpddr6_synthetic_research "$HBM_SIM_BIN" --stats-view diagnostic \
  --config experiments/local/lpddr6_synthetic_linkprot.cfg --requests 32
run_and_check lpddr6_lowdvfs "$HBM_SIM_BIN" --stats-view diagnostic --config configs/lpddr.cfg \
  --standard lpddr6 --preset low_dvfs_4267 --requests 32
run_and_check lpddr6_ca_parity "$HBM_SIM_BIN" --stats-view diagnostic --standard lpddr6 --requests 32 \
  --lpddr-wck always_on --lpddr-ca-parity true --validate-cmd-trace
if "$HBM_SIM_BIN" --stats-view diagnostic --standard lpddr6 --requests 1 --lpddr-ca-parity true >/tmp/hbm_sim_bad_ca_parity.out 2>&1; then
  cat /tmp/hbm_sim_bad_ca_parity.out
  echo "expected lpddr ca parity without WCK always_on to fail" >&2
  exit 1
fi
grep -Eq "CA parity requires lpddr_wck_mode=always_on" /tmp/hbm_sim_bad_ca_parity.out
rm -f /tmp/hbm_sim_bad_ca_parity.out
run_and_check hbm4_init_sequence "$HBM_SIM_BIN" --stats-view diagnostic --standard hbm4 --requests 16 \
  --init-sequence hbm4 --hbm-link-retry true --validate-cmd-trace
run_and_check lpddr6_full_init_sequence "$HBM_SIM_BIN" --stats-view diagnostic --standard lpddr6 --requests 16 \
  --init-sequence lpddr6_full --lpddr-link-protection true --lpddr-link-ecc true \
  --validate-cmd-trace

# CLI 帮助列出的较老标准初始化名称必须保持可执行，避免帮助、配置模板与生成器漂移。
for init_case in "hbm3 hbm3" "lpddr5 lpddr5"; do
  read -r init_standard init_sequence <<<"$init_case"
  init_out="$("$HBM_SIM_BIN" --stats-view diagnostic --standard "$init_standard" --requests 0 \
    --init-sequence "$init_sequence" --max-cycles 100000 --stats-view diagnostic)"
  grep -Eq '^commands\.MRW[[:space:]]*: [1-9]' <<<"$init_out"
  grep -Eq '^remaining_requests[[:space:]]*: 0' <<<"$init_out"
  grep -Eq '^hit_cycle_limit[[:space:]]*: false' <<<"$init_out"
done
cross_family_init_out="$(mktemp)"
if "$HBM_SIM_BIN" --stats-view diagnostic --standard hbm4 --init-sequence lpddr6 --check-config \
    >"$cross_family_init_out" 2>&1; then
  echo "expected a cross-family init sequence to fail config validation" >&2
  exit 1
fi
grep -F "init_sequence 'lpddr6' is incompatible with base standard HBM4" \
  "$cross_family_init_out" >/dev/null
rm -f "$cross_family_init_out"
run_and_check hbm4_closed_cap "$HBM_SIM_BIN" --stats-view diagnostic --standard hbm4 --requests 64 \
  --row-policy closed_cap --row-policy-cap 1
run_and_check hbm4_addr_mapping "$HBM_SIM_BIN" --stats-view diagnostic --standard hbm4 --requests 64 \
  --addr-mapping RoBaRaCoCh --channel-mapper decoded
run_and_check hbm4_all_bank_rfm "$HBM_SIM_BIN" --stats-view diagnostic --standard hbm4 --requests 16 \
  --rfm-policy all_bank --rfm-act-threshold 1 --validate-cmd-trace

cmd_trace="$(mktemp)"
timing_table="$(mktemp)"
dfi_trace="$(mktemp)"
dfi_signal_trace="$(mktemp)"
run_and_check hbm4_cmd_trace "$HBM_SIM_BIN" --stats-view diagnostic --standard hbm4 --requests 8 \
  --scheduler fcfs --row-policy closed_page --channel-mapper round_robin \
  --cmd-trace "$cmd_trace" --dfi-trace "$dfi_trace" --dfi-signal-trace "$dfi_signal_trace" \
  --dump-timing-table "$timing_table" \
  --dfi-phase-count 2 --dfi-data-lane-bytes 16 --validate-cmd-trace --validate-dfi-trace
grep -Eq "^cycle,request_id,command,bus,stack_id,channel" "$cmd_trace"
grep -Eq ",RDA,|,WRA," "$cmd_trace"
grep -Eq "^cycle,phase,kind,request_id,command,bus,beat,beat_count,beat_bytes" "$dfi_trace"
grep -Eq ",READ_DATA,|,WRITE_DATA," "$dfi_trace"
grep -Eq "^cycle,phase,request_id,kind,command,stack_id,dfi_cs_n,dfi_address" "$dfi_signal_trace"
grep -Eq ",1,[^,]*,[^,]*,[^,]*," "$dfi_signal_trace"
grep -Eq "^preset,name,value_nck,source" "$timing_table"
grep -Eq "nCL" "$timing_table"
rm -f "$cmd_trace" "$timing_table" "$dfi_trace" "$dfi_signal_trace"

# 手册公开的 JEDEC 风格 tRRD_S/tRRD_L 拼写必须和无下划线别名等价。
timing_alias_cfg="$(mktemp)"
timing_alias_table="$(mktemp)"
cat >"$timing_alias_cfg" <<'CFG'
[override]
tCK_ps = 500
tRRD_S_ns = 3.0
tRRD_L_ns = 4.0
CFG
"$HBM_SIM_BIN" --stats-view diagnostic --config configs/hbm.cfg --config "$timing_alias_cfg" \
  --standard hbm4 --requests 0 --dump-timing-table "$timing_alias_table" \
  >/dev/null
grep -Eq '^"?HBM4"?,"?nRRDS"?,6,' "$timing_alias_table"
grep -Eq '^"?HBM4"?,"?nRRDL"?,8,' "$timing_alias_table"
rm -f "$timing_alias_cfg" "$timing_alias_table"

# 显式维护 trace：差分工具用它构造可复现的 refresh/RFM 场景。
maintenance_trace="$(mktemp)"
maintenance_cmds="$(mktemp)"
maintenance_out="$(mktemp)"
cat >"$maintenance_trace" <<'TRACE'
0 M REFpb 0x0
1000 M RFMpb 0x40
TRACE
"$HBM_SIM_BIN" --stats-view diagnostic --config configs/validation/hbm4.cfg --standard hbm4 \
  --preset ramulator2_reference_1ch \
  --trace "$maintenance_trace" --cmd-trace "$maintenance_cmds" \
  --validate-cmd-trace >"$maintenance_out"
grep -F ",REFpb," "$maintenance_cmds" >/dev/null
grep -F ",RFMpb," "$maintenance_cmds" >/dev/null
grep -Eq "^maintenance_served[[:space:]]*: 2" "$maintenance_out"
rm -f "$maintenance_trace" "$maintenance_cmds" "$maintenance_out"

dfi_real_trace="$(mktemp)"
dfi_real_signal_trace="$(mktemp)"
cat >"$dfi_real_trace" <<'TRACE'
0 W 0x1000 data=00112233445566778899aabbccddeeff
200 R 0x1000 expect=00112233445566778899aabbccddeeff
TRACE
run_and_check hbm4_dfi_real_payload "$HBM_SIM_BIN" --stats-view diagnostic --standard hbm4 \
  --trace "$dfi_real_trace" --requests 0 \
  --dfi-signal-trace "$dfi_real_signal_trace" \
  --dfi-phase-count 2 --dfi-data-lane-bytes 16 --validate-dfi-trace
grep -F "WRITE_DATA,WR" "$dfi_real_signal_trace" | \
  grep -F "00112233445566778899aabbccddeeff" | \
  grep -F "request_payload" >/dev/null
grep -F "WRITE_DATA,WR" "$dfi_real_signal_trace" | \
  grep -F "00000000000000000000000000000000" >/dev/null
grep -F "READ_DATA,RD" "$dfi_real_signal_trace" | \
  grep -F "00112233445566778899aabbccddeeff" | \
  grep -F "memory_image" >/dev/null
rm -f "$dfi_real_trace" "$dfi_real_signal_trace"

golden_a="$(mktemp)"
golden_b="$(mktemp)"
"$HBM_SIM_BIN" --stats-view diagnostic --standard hbm4 --requests 16 --validate-cmd-trace >"$golden_a"
"$HBM_SIM_BIN" --stats-view diagnostic --standard hbm4 --requests 16 --validate-cmd-trace >"$golden_b"
python3 ./tools/compare_stats.py "$golden_a" "$golden_b" \
  --keys completed_reads,completed_writes,commands.ACT,cmd_validation_checked,achieved_bw_GBps \
  --abs-tol 0 --rel-tol 0
rm -f "$golden_a" "$golden_b"

# 突发轨迹：带 pattern 和 check 的 BW/BR 语法。
burst_trace="$(mktemp)"
cat > "$burst_trace" << 'BURSTEOF'
BW 0x2000 len=512 pattern=request_id
BR 0x2000 len=512 check=last_write
BW 0x4000 len=128 pattern=zero
BR 0x4000 len=128 check=last_write
BW 0x5000 len=128 pattern=ff
BR 0x5000 len=128 check=last_write
BURSTEOF
burst_out="$(mktemp)"
"$HBM_SIM_BIN" --stats-view diagnostic --standard lpddr5 --trace "$burst_trace" --max-cycles 1000 >"$burst_out"
grep -Eq "^burst_trace_lines[[:space:]]*: 6" "$burst_out"
grep -Eq "^burst_split_requests[[:space:]]*: 24" "$burst_out"
grep -Eq "^burst_write_requests[[:space:]]*: 12" "$burst_out"
grep -Eq "^burst_read_requests[[:space:]]*: 12" "$burst_out"
grep -Eq "^data_mismatches[[:space:]]*: 0" "$burst_out"
rm -f "$burst_trace" "$burst_out"
rm -rf "$ordering_dir"

echo "smoke tests passed"
