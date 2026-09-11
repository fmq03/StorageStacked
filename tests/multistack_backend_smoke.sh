#!/usr/bin/env bash
set -euo pipefail

# HBM4 双 Stack 后端回归：每个 Stack 必须拥有独立文件，并能跨进程恢复数据。
backend_bin=${HBM_SIM_BIN:?HBM_SIM_BIN is required}
backend_source=${HBM_SIM_SOURCE_DIR:?HBM_SIM_SOURCE_DIR is required}
backend_tmp=$(mktemp -d)
trap 'rm -rf -- "$backend_tmp"' EXIT

common=(
  --stats-view diagnostic
  --config "$backend_source/configs/hbm.cfg" --standard hbm4
  --stack-count 2 --requests 0 --max-cycles 10000
  --memory-capacity-bytes 1048576
)

# 稀疏后端覆盖同一进程内的双 Stack 数据闭环。
"$backend_bin" "${common[@]}" --memory-backend sparse \
  --trace "$backend_source/examples/multistack_demos/two_stack_payload.trace" \
  >"$backend_tmp/sparse.out"
grep -Eq '^active_stacks[[:space:]]*: 2$' "$backend_tmp/sparse.out"
grep -Eq '^data_mismatches[[:space:]]*: 0$' "$backend_tmp/sparse.out"

for backend_kind in mmap_sparse chunk_file; do
  backend_dir=$backend_tmp/$backend_kind
  mkdir -p "$backend_dir"
  backend_extra=()
  if [[ "$backend_kind" == chunk_file ]]; then
    backend_extra+=(--memory-chunk-size 65536 --memory-chunk-cache-entries 2)
  fi
  files=(
    --memory-data-file "$backend_dir/data_{stack}.bin"
    --memory-init-file "$backend_dir/init_{stack}.bin"
    --memory-meta-file "$backend_dir/meta_{stack}.bin"
    --memory-presence-file "$backend_dir/presence_{stack}.bin"
  )

  "$backend_bin" "${common[@]}" --memory-backend "$backend_kind" \
    "${files[@]}" "${backend_extra[@]}" \
    --trace "$backend_source/tests/data/multistack_backend_write.trace" \
    >"$backend_dir/write.out"
  test -s "$backend_dir/data_0.bin"
  test -s "$backend_dir/data_1.bin"
  test -s "$backend_dir/meta_0.bin"
  test -s "$backend_dir/meta_1.bin"
  test -s "$backend_dir/init_0.bin"
  test -s "$backend_dir/init_1.bin"
  test -s "$backend_dir/presence_0.bin"
  test -s "$backend_dir/presence_1.bin"

  # 第二个进程只读，验证 Stack 0/1 文件映射和持久化内容都正确。
  "$backend_bin" "${common[@]}" --memory-backend "$backend_kind" \
    "${files[@]}" "${backend_extra[@]}" \
    --trace "$backend_source/tests/data/multistack_backend_read.trace" \
    >"$backend_dir/read.out"
  grep -Eq '^active_stacks[[:space:]]*: 2$' "$backend_dir/read.out"
  # HBM4 的 64 B host request 被拆成两个 32 B DRAM transaction，因此共检查 4 次。
  grep -Eq '^data_checked_reads[[:space:]]*: 4$' "$backend_dir/read.out"
  grep -Eq '^data_mismatches[[:space:]]*: 0$' "$backend_dir/read.out"
done

echo "multistack backend smoke passed"
