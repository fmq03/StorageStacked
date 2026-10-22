#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."
# HBM3 随机流量示例：请求数较小，避免队列扫描模型运行过久。
./build/hbm_sim --config examples/configs/hbm.cfg --standard hbm3 \
  --pattern random --requests 1500 --read-ratio 70 --seed 7
