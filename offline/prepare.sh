#!/usr/bin/env bash
set -euo pipefail
root=$STORAGE_WORKSPACE
dpkg -i /opt/storagestacked/system-tools/*.deb
ln -sfn /usr/bin/make /usr/bin/gmake
prefix=$root/vortex_StorageStacked/third_party/.cache/tools/toolchain
ln -s "$prefix/bin/python3" /usr/local/bin/python3
# Bazel actions use /usr/bin:/bin rather than the interactive PATH.
ln -s "$prefix/bin/python3" /usr/bin/python3
# The independent AXI runner must use the same portable compiler as the SDKs.
rm -rf "$root/axi_StorageStacked/build"
source "$root/vortex_StorageStacked/third_party/gem5/runtime/environment.sh"
cmake -S "$root/axi_StorageStacked" -B "$root/axi_StorageStacked/build" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER="$AXI_CXX" \
  -DCMAKE_C_COMPILER="$AXI_CC" -DCMAKE_ASM_COMPILER="$AXI_CC" \
  -DPython3_EXECUTABLE="$AXI_PYTHON"
cmake --build "$root/axi_StorageStacked/build" -j 12
# Disable implicit dependency downloads; all repositories are already vendored.
cat >> "$root/coralnpu_StorageStacked/coralnpu/.bazelrc" <<'BAZEL'

# Offline Docker delivery: forbid fetching absent repositories.
common --nofetch
BAZEL
# Bazel requires its cache directories to belong to the executing user.
# Tar archives preserve source UIDs; only directories need ownership changes.
find "$root/coralnpu_StorageStacked/coralnpu/.cache/tools/bazel" -type d -exec chown 0:0 {} +
# Worker processes and their trash directories cannot be relocated across
# Docker's lower and writable layers. Retain dependencies, recreate workers.
find "$root/coralnpu_StorageStacked/coralnpu/.cache/tools/bazel" -mindepth 2 -maxdepth 2 \
  -type d -name bazel-workers -exec rm -rf {} +
bash /opt/storagestacked/fix-native-cache.sh
# Rebuild both platforms and workloads from the bundled source. Build caches
# accelerate compilation, but are never accepted as proof that source is current.
(cd "$root/vortex_StorageStacked" && bash build.sh --jobs 12)
(cd "$root/coralnpu_StorageStacked" && bash build.sh --jobs 12)
mkdir -p /results
