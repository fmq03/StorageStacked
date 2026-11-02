#!/usr/bin/env bash
set -euo pipefail

phy_bin=${HBM_SIM_BIN:?HBM_SIM_BIN is required}
phy_source=${HBM_SIM_SOURCE_DIR:?HBM_SIM_SOURCE_DIR is required}
phy_tmp_dir=$(mktemp -d)
trap 'rm -rf -- "$phy_tmp_dir"' EXIT

value_of() {
  local key=$1
  awk -F: -v wanted="$key" '$1 ~ "^" wanted "[ ]*$" {gsub(/[[:space:]]/, "", $2); print $2; exit}'
}

for standard in hbm3 hbm4 lpddr5 lpddr6; do
  if [[ "$standard" == hbm* ]]; then
    family_config=hbm.cfg
  else
    family_config=lpddr.cfg
  fi
  output=$(
    "$phy_bin" --stats-view diagnostic \
      --config "$phy_source/configs/$family_config" \
      --standard "$standard" \
      --requests 24 \
      --read-ratio 50 \
      --max-cycles 500000 \
      --dfi-trace "$phy_tmp_dir/${standard}.csv" \
      --validate-dfi-trace
  )
  [[ $(value_of mem_phy_mode <<<"$output") == behavioral ]]
  [[ $(value_of dfi_validation <<<"$output") == pass ]]
  [[ $(value_of data_mismatches <<<"$output") == 0 ]]
  [[ $(value_of remaining_requests <<<"$output") == 0 ]]
  [[ $(value_of remaining_pending <<<"$output") == 0 ]]
  [[ $(value_of hit_cycle_limit <<<"$output") == false ]]
  [[ $(value_of phy_read_requests <<<"$output") == $(value_of phy_read_completions <<<"$output") ]]
  [[ $(value_of phy_write_requests <<<"$output") == $(value_of phy_write_completions <<<"$output") ]]
done

multi_output=$(
  "$phy_bin" --stats-view diagnostic \
    --config "$phy_source/configs/hbm.cfg" --standard hbm4 --stack-count 6 \
    --requests 24 \
    --read-ratio 50 \
    --max-cycles 500000
)
[[ $(value_of mem_phy_mode <<<"$multi_output") == behavioral ]]
[[ $(value_of controller_count <<<"$multi_output") == 192 ]]
[[ $(value_of active_stacks <<<"$multi_output") == 6 ]]
[[ $(value_of remaining_requests <<<"$multi_output") == 0 ]]
[[ $(value_of remaining_pending <<<"$multi_output") == 0 ]]
[[ $(value_of data_mismatches <<<"$multi_output") == 0 ]]

# 同一 stack-local 地址在不同 stack 必须保存独立 payload。此场景还验证从
# Behavioral PHY completion 回填的实际 DFI 数据，而不是只检查“请求都完成”。
printf '%s\n' \
  '0 W 0x0000 stack=0 qos=0 data=000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f' \
  '1 W 0x0000 stack=5 qos=7 data=f0f1f2f3f4f5f6f7f8f9fafbfcfdfeffe0e1e2e3e4e5e6e7e8e9eaebecedeeef' \
  '400 R 0x0000 stack=0 qos=0 expect=000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f' \
  '401 R 0x0000 stack=5 qos=7 expect=f0f1f2f3f4f5f6f7f8f9fafbfcfdfeffe0e1e2e3e4e5e6e7e8e9eaebecedeeef' \
  > "$phy_tmp_dir/multistack_data.trace"
multi_data_output=$(
  "$phy_bin" --stats-view diagnostic \
    --config "$phy_source/configs/hbm.cfg" --standard hbm4 --stack-count 6 \
    --trace "$phy_tmp_dir/multistack_data.trace" \
    --requests 0 --max-cycles 500000 \
    --validate-cmd-trace --validate-dfi-trace \
    --cmd-trace "$phy_tmp_dir/multistack_commands.csv" \
    --dfi-trace "$phy_tmp_dir/multistack_dfi.csv" \
    --dfi-signal-trace "$phy_tmp_dir/multistack_dfi_signal.csv"
)
[[ $(value_of cmd_validation <<<"$multi_data_output") == pass ]]
[[ $(value_of dfi_validation <<<"$multi_data_output") == pass ]]
[[ $(value_of active_stacks <<<"$multi_data_output") == 2 ]]
[[ $(value_of data_checked_reads <<<"$multi_data_output") == 2 ]]
[[ $(value_of data_mismatches <<<"$multi_data_output") == 0 ]]
[[ $(value_of data_write_commits <<<"$multi_data_output") == 2 ]]
python3 - "$phy_tmp_dir/multistack_commands.csv" \
  "$phy_tmp_dir/multistack_dfi_signal.csv" <<'PY'
import csv
import sys

commands_path, signals_path = sys.argv[1:]
with open(commands_path, newline='', encoding='utf-8') as source:
    commands = list(csv.DictReader(source))
assert {row['stack_id'] for row in commands} == {'0', '5'}, commands
assert {row['command'] for row in commands} >= {'ACT', 'RD', 'WR'}, commands

with open(signals_path, newline='', encoding='utf-8') as source:
    signals = list(csv.DictReader(source))
writes = [row for row in signals if row['kind'] == 'WRITE_DATA']
reads = [row for row in signals if row['kind'] == 'READ_DATA']
assert {row['stack_id'] for row in writes} == {'0', '5'}, writes
assert {row['stack_id'] for row in reads} == {'0', '5'}, reads
assert all(row['payload_source'] == 'request_payload' and row['dfi_wrdata']
           for row in writes), writes
assert all(row['payload_source'] == 'memory_image' and row['dfi_rddata']
           for row in reads), reads
assert {row['dfi_rddata'] for row in reads} == {
    '000102030405060708090a0b0c0d0e0f',
    '101112131415161718191a1b1c1d1e1f',
    'f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff',
    'e0e1e2e3e4e5e6e7e8e9eaebecedeeef',
}, reads
PY

echo "phy smoke passed"
