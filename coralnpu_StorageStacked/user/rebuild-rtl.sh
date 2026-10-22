#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.."
if [[ $# == 0 || $1 == --help ]]; then
    echo '用法：./user/rebuild-rtl.sh --config user/<项目>/config.json'
    echo '读取配置，经 Chisel 生成 RTL，再构建 Verilator C++ 模型和设备仿真库；同配置使用增量构建。'
    exit 0
fi
# shellcheck source-path=SCRIPTDIR source=../coralnpu/runtime/environment.sh
source coralnpu/runtime/environment.sh
exec "$AXI_PYTHON" coralnpu/runtime/rebuild_rtl.py "$@"
