#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd "$root"
cmake -S axi_StorageStacked -B axi_StorageStacked/build -DCMAKE_EXPORT_COMPILE_COMMANDS=ON > .vscode/axi-index.log 2>&1
(
    source "$root/vortex_StorageStacked/third_party/gem5/runtime/environment.sh"
    export NAV_CONFIGS=$("$AXI_PYTHON" -c 'import sys;from configure import load_config,simx_flags;print(simx_flags(load_config(sys.argv[1])))' "$root/vortex_StorageStacked/user/smoke/config.json")
    /usr/bin/python3 .vscode/refresh_make_commands.py
)
(
    source "$root/coralnpu_StorageStacked/coralnpu/runtime/environment.sh"
    "$AXI_PYTHON" .vscode/refresh_bazel_commands.py
)
/usr/bin/python3 .vscode/refresh_compile_commands.py --gem5
bash .vscode/refresh_user_navigation.sh
/usr/bin/python3 .vscode/refresh_gem5_python_navigation.py
/usr/bin/python3 .vscode/refresh_scala_tags.py
