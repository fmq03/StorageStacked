#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd "$root"
if [[ ! -f build/vortex/sw/VX_config.h ]]; then bash env/bootstrap.sh; fi
cmake -S . -B build/system -DCMAKE_BUILD_TYPE=Release
cmake --build build/system -j"${SS_JOBS:-8}"
mkdir -p build/workloads
riscv64-unknown-elf-gcc -march=rv32imf -mabi=ilp32f -O2 -ffp-contract=off -fno-fast-math \
  -msmall-data-limit=0 -nostdlib -ffreestanding -fno-builtin -T workloads/link.ld \
  workloads/start.S workloads/moba.c -o build/workloads/moba.elf
riscv64-unknown-elf-objcopy -O binary build/workloads/moba.elf build/workloads/moba.bin
