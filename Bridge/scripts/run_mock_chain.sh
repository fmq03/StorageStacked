#!/usr/bin/env bash
# P3b 的前置验证：用 mock gem5 代替真 gem5 驱动整条链路。
#
# 真 gem5 的构建与接入耗时较长，而 socket 协议一旦对上，换真 gem5 不需要改
# 链路侧的任何代码。所以先在这里把"gem5 → Bridge → mem_sim"这条路径跑通，
# 让问题范围只剩 gem5 配置本身。
set -euo pipefail

BRIDGE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${BRIDGE_DIR}"

SOCKET="${SOCKET:-/tmp/bridge_mock_gem5.sock}"
OUT_DIR="${OUT_DIR:-sim}"
RTL_BRIDGE="${RTL_BRIDGE:-0}"

if [[ "${RTL_BRIDGE}" == "1" ]]; then
    CHAIN_TARGET="rtl-gem5-full-chain"
    CHAIN_BIN="build/obj_rtl_gem5_chain/Vstorage_bridge_top"
    CHAIN_LOG="${OUT_DIR}/rtl_gem5_chain.log"
    MOCK_ARGS=(--beat-bytes 32)
else
    CHAIN_TARGET="build/tb_gem5_full_chain"
    CHAIN_BIN="build/tb_gem5_full_chain"
    CHAIN_LOG="${OUT_DIR}/gem5_chain.log"
    MOCK_ARGS=()
fi

mkdir -p "${OUT_DIR}"
rm -f "${SOCKET}"

echo "构建..."
make --no-print-directory "${CHAIN_TARGET}" build/mock_gem5_server >/dev/null

cleanup() {
    [[ -n "${mock_pid:-}" ]] && kill "${mock_pid}" 2>/dev/null || true
    rm -f "${SOCKET}"
}
trap cleanup EXIT

# mock gem5 作为 server 先起，链路侧再连（链路侧本身也有连接重试兜底）。
./build/mock_gem5_server --socket "${SOCKET}" "${MOCK_ARGS[@]}" \
    > "${OUT_DIR}/mock_gem5.log" 2>&1 &
mock_pid=$!

sleep 0.3

set +e
./"${CHAIN_BIN}" --socket "${SOCKET}" > "${CHAIN_LOG}" 2>&1
chain_rc=$?
set -e

if [[ ${chain_rc} -ne 0 ]]; then
    kill "${mock_pid}" 2>/dev/null || true
fi
wait "${mock_pid}" 2>/dev/null && mock_rc=0 || mock_rc=$?
mock_pid=""

echo
echo "===== mock gem5 ====="
sed -n '/已连接/,$p' "${OUT_DIR}/mock_gem5.log"
echo
echo "===== 链路侧 ====="
sed -n '/全链路结果/,$p' "${CHAIN_LOG}"

echo
if [[ ${mock_rc} -eq 0 && ${chain_rc} -eq 0 ]]; then
    echo "P3b（mock gem5，RTL_BRIDGE=${RTL_BRIDGE}）PASSED"
else
    echo "P3b（mock gem5）FAILED：mock rc=${mock_rc} chain rc=${chain_rc}"
    echo "完整日志：${OUT_DIR}/mock_gem5.log  ${CHAIN_LOG}"
    exit 1
fi
