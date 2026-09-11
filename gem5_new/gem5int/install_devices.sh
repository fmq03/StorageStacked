#!/usr/bin/env bash
set -euo pipefail
self=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
: "${GEM5_HOME:?请设置 GEM5_HOME}"
patch_file="$self/patches/dma_byte_enable.patch"
if ! patch -R -p1 -s -f --dry-run -d "$GEM5_HOME" -i "$patch_file" >/dev/null 2>&1; then
    patch -p1 -s -f --dry-run -d "$GEM5_HOME" -i "$patch_file"
    patch -p1 -s -d "$GEM5_HOME" -i "$patch_file"
fi
for component in dev/coralnpu dev/vortex mem/unified_timing; do
    mkdir -p "$GEM5_HOME/src/$component"
    cp "$self/src/$component/"* "$GEM5_HOME/src/$component/"
done
