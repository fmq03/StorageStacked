#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")"
stamp=$(date +%Y%m%d-%H%M%S)
(cd axi_StorageStacked && ./run.sh test --output "results/acceptance-$stamp")
./vortex_StorageStacked/build.sh --storage ../axi_StorageStacked --test
./coralnpu_StorageStacked/build.sh --storage ../axi_StorageStacked --test
echo '三项目验收通过。各项目结果和汇总报告位置见入口输出。'
