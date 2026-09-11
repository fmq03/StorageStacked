#!/usr/bin/env bash
set -euo pipefail
if [[ ${AXI_PROFILE:-unified} == unified ]]; then
    exec bash "$(dirname -- "${BASH_SOURCE[0]}")/../../env/build.sh" "$@"
fi
source "$(dirname -- "${BASH_SOURCE[0]}")/env.sh"
cd "$AXI_GEM5_HOME"
test "$(cat .source-revision)" = 2721ed751edac7d4cf3df574c6e0293343a14ba2
"$AXI_PYTHON" "$AXI_PROJECT_DIR/scripts/patch_gem5.py" "$AXI_GEM5_HOME"
"$AXI_ENV_ROOT/venv/bin/scons" build/AXI/gem5.opt \
    "EXTRAS=$AXI_PROJECT_DIR" "CXX=${AXI_CXX:-g++-10}" "-j${AXI_JOBS:-6}"
