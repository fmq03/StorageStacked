#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
cd "$root"
(
./axi_StorageStacked/run.sh setup
source common/toolchain/env.sh
./axi_StorageStacked/run.sh build
build=${STORAGE_BUILD_DIR:-$root/axi_StorageStacked/build}
export SYSTEMC_HOME
SYSTEMC_HOME=$(./axi_StorageStacked/run.sh systemc-prefix)
export SYSTEMC_CXX_STANDARD=${SYSTEMC_CXX_STANDARD:-${STORAGE_CXX_STANDARD:-17}}
stamp=$(date +%Y%m%d-%H%M%S)
component_results=$root/results/components-$stamp
status=0
make -C components/axi2flit CXX="$CXX" SYSTEMC_HOME="$SYSTEMC_HOME" SYSTEMC_CXX_STANDARD="$SYSTEMC_CXX_STANDARD" SUMMARY_DIR="$component_results/axi2flit" preflight || status=1
"$SS_PYTHON" common/verif/summary.py check "$component_results/axi2flit/summary.json"
[[ $status == 0 ]]
make -C components/ucie-model CXX="$CXX" SYSTEMC_HOME="$SYSTEMC_HOME" SYSTEMC_CXX_STANDARD="$SYSTEMC_CXX_STANDARD" SUMMARY_DIR="$component_results/ucie" validate || status=1
"$SS_PYTHON" common/verif/summary.py check "$component_results/ucie/summary.json"
[[ $status == 0 ]]
"$SS_PYTHON" common/verif/summary.py ctest --build "$build/mem_sim" --directory "$component_results/memsim"
"$SS_PYTHON" common/verif/summary.py check "$component_results/memsim/summary.json"
if [[ -x /usr/bin/clang++-18 ]]; then
    make -C components/mem_sim SANITIZER_BUILD_DIR="$root/.cache/memsim-sanitize" sanitize-test
else
    printf '跳过 mem_sim sanitizer：未安装 clang++-18。\n'
fi
./axi_StorageStacked/run.sh link-test preflight
./axi_StorageStacked/run.sh test --output "$root/results/acceptance-$stamp"
)
./vortex_StorageStacked/build.sh --storage ../axi_StorageStacked --jobs "${BUILD_JOBS:-12}" --test
./coralnpu_StorageStacked/build.sh --storage ../axi_StorageStacked --jobs "${BUILD_JOBS:-12}" --test
( source common/toolchain/env.sh; "$SS_PYTHON" tools/check_links.py )
printf '原生验收通过。各项目真实数据与协议检查的 summary.json 路径见入口输出。\n'
