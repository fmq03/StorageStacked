#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd "$root"
bash .vscode/setup_verification.sh
for platform in vortex coralnpu; do
    if [[ $platform == vortex ]]; then runtime="$root/vortex_StorageStacked/third_party/gem5/runtime"; else runtime="$root/coralnpu_StorageStacked/coralnpu/runtime"; fi
    (
        source "$runtime/environment.sh"
        "$AXI_PYTHON" "$root/.vscode/collect_user_commands.py" "$platform"
        if [[ $platform == coralnpu ]]; then
            "$AXI_PYTHON" "$root/.vscode/refresh_bazel_commands.py"
        fi
    )
done
/usr/bin/python3 .vscode/refresh_compile_commands.py
/usr/bin/python3 .vscode/refresh_gem5_python_navigation.py
/usr/bin/python3 .vscode/refresh_scala_tags.py
