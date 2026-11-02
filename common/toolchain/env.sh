#!/usr/bin/env bash
# Source into a build/run process; no shell startup files are modified.
ss_toolchain_here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
ss_toolchain_repo=$(cd "$ss_toolchain_here/../.." && pwd)
export SS_TOOLCHAIN_PREFIX=${SS_TOOLCHAIN_PREFIX:-$ss_toolchain_repo/.cache/toolchain}
if [[ ! -x "$SS_TOOLCHAIN_PREFIX/bin/python" ]]; then
    echo "工具链未安装；运行 $ss_toolchain_here/setup.sh" >&2
    return 1
fi
python3 "$ss_toolchain_here/check.py" "$SS_TOOLCHAIN_PREFIX" "$ss_toolchain_here/conda-linux-64.lock" || return 1
unset LD_PRELOAD LD_AUDIT PYTHONOPTIMIZE
export PYTHONDONTWRITEBYTECODE=1
export CC="$SS_TOOLCHAIN_PREFIX/bin/x86_64-conda-linux-gnu-gcc"
export CXX="$SS_TOOLCHAIN_PREFIX/bin/x86_64-conda-linux-gnu-g++"
export SS_PYTHON="$SS_TOOLCHAIN_PREFIX/bin/python"
export PATH="$SS_TOOLCHAIN_PREFIX/bin:$PATH"
unset ss_toolchain_here ss_toolchain_repo
