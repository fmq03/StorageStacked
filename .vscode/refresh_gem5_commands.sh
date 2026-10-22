#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
source "$root/vortex_StorageStacked/third_party/gem5/runtime/environment.sh"
exec 9>"$root/vortex_StorageStacked/third_party/.cache/build.lock"
flock -s 9
cd "$GEM5_HOME"
for variant in AXI AXI_ARM; do
    [[ -f build/$variant/gem5.build/config ]] || continue
    "$GEM5_SCONS" "build/$variant/compile_commands.json" \
        "CXX=$AXI_CXX" "CC=$AXI_CC" "PYTHON_CONFIG=$PYTHON_CONFIG" \
        "EXTRAS=$root/vortex_StorageStacked/integration:$STORAGE_STACK_ROOT/storage_axi" \
        -j 4 > "$root/.vscode/gem5-$variant-index.log" 2>&1
done
