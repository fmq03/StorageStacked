#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd "$root"
git submodule update --init vortex-gpu/vortex
git -C vortex-gpu/vortex submodule update --init third_party/softfloat third_party/hardfloat
patchfile="$root/vortex/patches/external_memory.patch"
if ! git -C vortex-gpu/vortex apply --reverse --check "$patchfile" 2>/dev/null; then
    git -C vortex-gpu/vortex apply --check "$patchfile"
    git -C vortex-gpu/vortex apply "$patchfile"
fi
mkdir -p build/vortex
cd build/vortex
../../vortex-gpu/vortex/configure --xlen=32 --tooldir=/usr
make -C ../../vortex-gpu/vortex/third_party softfloat -j"${SS_JOBS:-8}"
