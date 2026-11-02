#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."
# LPDDR6 trace 示例；完整 cfg 是由权威 LPDDR 主配置复制得到的自包含示例。
./build/hbm_sim --config examples/configs/lpddr.cfg --standard lpddr6 \
  --trace examples/sample.trace --requests 0
