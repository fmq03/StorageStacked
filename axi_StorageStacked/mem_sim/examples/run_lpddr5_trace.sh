#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."
# LPDDR5 兼容示例。默认推荐使用 LPDDR6；该脚本用于对照 baseline。
./build/hbm_sim --config examples/configs/lpddr.cfg --standard lpddr5 \
  --trace examples/sample.trace --requests 0
