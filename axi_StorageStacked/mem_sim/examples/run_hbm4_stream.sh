#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."
# HBM4 默认示例：顺序流量 + 80% 读请求，用于观察 row-hit 和带宽统计。
./build/hbm_sim --config examples/configs/hbm.cfg --standard hbm4 \
  --pattern stream --requests 3000 --read-ratio 80
