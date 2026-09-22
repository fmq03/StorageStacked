#!/usr/bin/env bash
# 从上游仓库收集 Bridge 所需的依赖到 vendor/ 下，并给 ucie-model 的副本打上
# AOU 接入补丁。上游仓库全程只读——本项目独立演进，不回写别人的仓库。
#
# 重新运行会先清空 vendor/，因此 vendor/ 下的内容永远可以由本脚本重新生成。
set -euo pipefail

BRIDGE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORKSPACE="$(cd "${BRIDGE_DIR}/.." && pwd)"

AXI2FLIT_DIR="${AXI2FLIT_DIR:-${WORKSPACE}/axi2flit}"
UCIE_DIR="${UCIE_DIR:-${WORKSPACE}/ucie-model}"

SRC_SYSTEMC="${AXI2FLIT_DIR}/systemc"
SRC_UCIE="${UCIE_DIR}/src"
AOU_PATCH="${SRC_SYSTEMC}/integration/ucie-model-aou.patch"
VENDOR="${BRIDGE_DIR}/vendor"

for probe in "${SRC_SYSTEMC}/include/axi2flit.h" "${SRC_UCIE}/ucie_link.h" "${AOU_PATCH}"; do
    [[ -f "${probe}" ]] || { echo "缺少上游文件：${probe}" >&2; exit 1; }
done

echo "上游来源："
printf '  axi2flit   %s @ %s\n' "${AXI2FLIT_DIR}" \
    "$(git -C "${AXI2FLIT_DIR}" rev-parse --short HEAD 2>/dev/null || echo '(非 git)')"
printf '  ucie-model %s @ %s\n' "${UCIE_DIR}" \
    "$(git -C "${UCIE_DIR}" rev-parse --short HEAD 2>/dev/null || echo '(非 git)')"

rm -rf "${VENDOR}"
mkdir -p "${VENDOR}/axi2flit/include" \
         "${VENDOR}/axi2flit/integration" \
         "${VENDOR}/axi2flit/src" \
         "${VENDOR}/ucie-model/src"

# 桥的编解码与类型定义（纯头文件）
cp "${SRC_SYSTEMC}/include/"*.h "${VENDOR}/axi2flit/include/"

# 链路适配层与存储端点（纯头文件）。simple_burst_memory.h 也一并带上——它是
# Bridge 要对齐的语义参照，也是回归对照用的原后端。
cp "${SRC_SYSTEMC}/integration/aou_target.h" \
   "${SRC_SYSTEMC}/integration/simple_burst_memory.h" \
   "${SRC_SYSTEMC}/integration/ucie_aou_adapter.h" \
   "${VENDOR}/axi2flit/integration/"

# Axi2Flit 需要真正编译的源文件
cp "${SRC_SYSTEMC}/src/"*.cpp "${VENDOR}/axi2flit/src/"

# ucie-model 只需四个头文件（纯头文件实现，无 .cpp 需要链接）
cp "${SRC_UCIE}/ucie_common.h" \
   "${SRC_UCIE}/ucie_fdi.h" \
   "${SRC_UCIE}/ucie_link.h" \
   "${SRC_UCIE}/ucie_phy.h" \
   "${VENDOR}/ucie-model/src/"

# AOU 补丁还改了 Makefile 与 ucie_systemc_main.cpp，这两个我们并不 vendor，
# 因此只取补丁里与链路行为相关的两个头文件 hunk。aou_format6.h 由
# vendor/axi2flit/include 提供，通过 -I 传递。
#
# 注意：必须 cd 进目标目录后按相对路径 apply。给 git apply 传 --directory
# 会与 --include 组合后静默失效（rc=0 但一个 hunk 都不打），排查过一次。
( cd "${VENDOR}/ucie-model" && \
  git apply --include='src/ucie_common.h' --include='src/ucie_link.h' "${AOU_PATCH}" )

# 补丁生效自检：AOU 枚举与 gather 调用必须出现
grep -q 'AouFormat6' "${VENDOR}/ucie-model/src/ucie_common.h" \
    || { echo "补丁未生效：ucie_common.h 中没有 AouFormat6" >&2; exit 1; }
grep -q 'aou_format6::gather' "${VENDOR}/ucie-model/src/ucie_link.h" \
    || { echo "补丁未生效：ucie_link.h 中没有 aou_format6::gather" >&2; exit 1; }

echo
echo "vendor 完成："
echo "  ${VENDOR}/axi2flit/include      $(ls -1 "${VENDOR}/axi2flit/include" | wc -l) 个头文件"
echo "  ${VENDOR}/axi2flit/integration  $(ls -1 "${VENDOR}/axi2flit/integration" | wc -l) 个头文件"
echo "  ${VENDOR}/axi2flit/src          $(ls -1 "${VENDOR}/axi2flit/src" | wc -l) 个源文件"
echo "  ${VENDOR}/ucie-model/src        $(ls -1 "${VENDOR}/ucie-model/src" | wc -l) 个头文件（已打 AOU 补丁）"
