#!/bin/bash --norc
# Copyright 2025 Google LLC
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

PROG=$(basename "$0")
DRIVER_DIR=$(dirname "$0")

# Find the bzlmod canonical repo directory name.
_resolve_toolchain() {
    local root="$1"
    local match
    match=$(ls -d "${root}/external/"*toolchain_coralnpu_v2 2>/dev/null | head -1)
    [[ -n "${match}" ]] && basename "${match}"
}

ARGS=()
POSTARGS=()
case "${PROG}" in
    gcc)
        ;;
esac

# Strip -lpthread / -pthread injected by abseil and a handful of tflite_micro
# helpers when they're transitively pulled into a coralnpu_v2 (RISC-V bare-
# metal, newlib) link. The cross newlib has no pthread; the threading code
# from those libraries is dead under our use because nothing in litert-micro
# / tfmicro actually starts a thread on this target.
FILTERED=()
for arg in "$@"; do
    case "$arg" in
        -lpthread|-pthread) ;;
        *) FILTERED+=("$arg") ;;
    esac
done
set -- "${FILTERED[@]}"

if [[ -n "${EXT_BUILD_ROOT}" ]]; then
    ROOT="${EXT_BUILD_ROOT}"
else
    ROOT="."
fi
TOOLCHAIN=$(_resolve_toolchain "${ROOT}")
if [[ -z "${TOOLCHAIN}" ]]; then
    echo "$0: cannot find toolchain_coralnpu_v2 under ${ROOT}/external" 1>&2
    exit 1
fi

if [[ -f "${ROOT}/external/${TOOLCHAIN}/bin/riscv64-unknown-elf-gcc" ]]; then
    PREFIX="riscv64-unknown-elf"
else
    PREFIX="riscv32-unknown-elf"
fi

exec "${ROOT}/external/${TOOLCHAIN}/bin/${PREFIX}-${PROG}" \
    "${ARGS[@]}" \
    "$@" \
    "${POSTARGS[@]}"
