#!/bin/sh
# Copyright 2026 Google LLC
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

set -x
set -e

# Ensure HOME is writable when running via docker --user
export HOME=/tmp

# Use git:// protocol for Sourceware repositories to prevent smart HTTPS 403 blocks
git config --global url."git://sourceware.org/git/".insteadOf "https://sourceware.org/git/"

# If output directory is empty/missing, clean gnu toolchain build stamps to force re-install
if [ -d "riscv-gnu-toolchain/stamps" ] && ( [ ! -d "rv_multilib_out" ] || [ -z "$(ls -A rv_multilib_out 2>/dev/null)" ] ); then
  echo "Output directory is empty/missing. Cleaning build stamps to force re-install..."
  rm -rf riscv-gnu-toolchain/stamps
fi

mkdir -p rv_multilib_out
TOOLCHAIN_OUT_DIR="$(pwd)"/rv_multilib_out

# Build gcc (shallow clone and fetch only required bare-metal submodules)
if [ ! -d "riscv-gnu-toolchain" ]; then
  git clone --depth=1 --branch 2026.06.06 https://github.com/riscv-collab/riscv-gnu-toolchain.git
fi
cd riscv-gnu-toolchain/ || exit
git submodule update --init --recursive --depth=1 gcc binutils newlib gdb
# Update gcc
cd gcc || exit
sed -i 's|https://gcc.gnu.org/pub/gcc/infrastructure/|ftp://gcc.gnu.org/pub/gcc/infrastructure/|g' contrib/download_prerequisites
contrib/download_prerequisites
cd .. || exit

./configure \
  --srcdir="$(pwd)" \
  --prefix="${TOOLCHAIN_OUT_DIR}" \
  --with-multilib-generator="rv32imf_zicsr_zifencei_zbb-ilp32--;rv64imf_zicsr_zifencei_zbb-lp64--" \
  --with-gcc-src="$(pwd)"/gcc \
  --with-cmodel=medany

make -C "$(pwd)" -j 32  newlib
cd .. || exit

# Build LLVM + compiler-rt
if [ ! -d "llvm-project" ]; then
  git clone --depth=1 https://github.com/llvm/llvm-project -b llvmorg-20.1.0
fi
cd llvm-project/ || exit

# Build LLVM
mkdir -p build
cmake -B build \
    -DCMAKE_INSTALL_PREFIX="${TOOLCHAIN_OUT_DIR}" \
    -DCMAKE_C_COMPILER=clang \
    -DCMAKE_CXX_COMPILER=clang++ \
    -DCMAKE_BUILD_TYPE=Release \
    -DLLVM_TARGETS_TO_BUILD="RISCV" \
    -DLLVM_ENABLE_PROJECTS="clang"  \
    -DLLVM_DEFAULT_TARGET_TRIPLE="riscv64-unknown-elf" \
    -DLLVM_INSTALL_TOOLCHAIN_ONLY=On \
    -DDEFAULT_SYSROOT="${TOOLCHAIN_OUT_DIR}"/riscv64-unknown-elf \
    -G Ninja \
    "$(pwd)"/llvm

cmake --build  build --target install

cmake -B "$(pwd)"/build/compiler-rt \
    -DCMAKE_INSTALL_PREFIX="${TOOLCHAIN_OUT_DIR}" \
    -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY \
    -DCMAKE_AR="${TOOLCHAIN_OUT_DIR}"/bin/llvm-ar \
    -DCMAKE_NM="${TOOLCHAIN_OUT_DIR}"/bin/llvm-nm \
    -DCMAKE_RANLIB="${TOOLCHAIN_OUT_DIR}"/bin/llvm-ranlib \
    -DCMAKE_C_FLAGS="-march=rv64imf_zicsr_zifencei_zbb" \
    -DCMAKE_ASM_FLAGS="-march=rv64imf_zicsr_zifencei_zbb" \
    -DCMAKE_C_COMPILER="${TOOLCHAIN_OUT_DIR}"/bin/clang \
    -DCMAKE_C_COMPILER_TARGET=riscv64-unknown-elf \
    -DCMAKE_ASM_COMPILER_TARGET=riscv64-unknown-elf \
    -DCOMPILER_RT_OS_DIR="clang/20/lib" \
    -DCMAKE_EXE_LINKER_FLAGS="-fuse-ld=lld" \
    -DCOMPILER_RT_BUILD_BUILTINS=ON \
    -DCOMPILER_RT_BUILD_SANITIZERS=OFF \
    -DCOMPILER_RT_BUILD_XRAY=OFF \
    -DCOMPILER_RT_BUILD_LIBFUZZER=OFF \
    -DCOMPILER_RT_BUILD_MEMPROF=OFF \
    -DCOMPILER_RT_BUILD_PROFILE=OFF \
    -DCOMPILER_RT_BUILD_ORC=OFF \
    -DCOMPILER_RT_BUILD_GWP_ASAN=OFF \
    -DCOMPILER_RT_BUILD_CTX_PROFILE=OFF \
    -DCOMPILER_RT_BAREMETAL_BUILD=ON \
    -DCOMPILER_RT_DEFAULT_TARGET_ONLY=ON \
    -DLLVM_CONFIG_PATH="$(pwd)"/build/bin/llvm-config \
    -DCMAKE_C_FLAGS="-march=rv64imf_zicsr_zifencei_zbb -mno-relax" \
    -DCMAKE_ASM_FLAGS="-march=rv64imf_zicsr_zifencei_zbb -mno-relax" \
    -G "Ninja" "$(pwd)"/compiler-rt

cmake --build "$(pwd)"/build/compiler-rt --target install

cd .. || exit

# New lib build fails if riscv64-unknown-elf-gcc is not available in PATH
export PATH="${TOOLCHAIN_OUT_DIR}"/bin:"$PATH"

if [ ! -f "${TOOLCHAIN_OUT_DIR}"/bin/riscv64-unknown-elf-gcc ]; then
  echo "ERROR: ${TOOLCHAIN_OUT_DIR}/bin/riscv64-unknown-elf-gcc does not exits"
  exit 1
fi

if [ ! -d "libgloss-htif" ]; then
  git clone --depth=1 https://github.com/ucb-bar/libgloss-htif
fi
cd libgloss-htif || exit
git checkout 39234a16247ab1fa234821b251f1f1870c3de343
# Enable zicsr extension in crt0.S to support CSR instructions under modern GCC multilibs
sed -i 's|\.section \.text\.init|\.section \.text\.init\n    \.option arch, +zicsr|g' misc/crt0.S
CC=${TOOLCHAIN_OUT_DIR}/bin/riscv64-unknown-elf-gcc \
AR=${TOOLCHAIN_OUT_DIR}/bin/riscv64-unknown-elf-ar \
SIZE=${TOOLCHAIN_OUT_DIR}/bin/riscv64-unknown-elf-size \
./configure --prefix=${TOOLCHAIN_OUT_DIR}/riscv64-unknown-elf --host=riscv64-unknown-elf
make && make install
cd .. || exit