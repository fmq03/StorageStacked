#!/usr/bin/env bash
# P3b：真 gem5 → Bridge → mem_sim 的完整链路。
#
# gem5 的 CommMonitor 是 AF_UNIX **server**（在 m5.instantiate() 里 bind+listen），
# 链路侧是 client 并带连接重试，所以这里先起链路侧、再起 gem5 也可以。
#
# 起步用 --axi-direct-shadow：RTL/SystemC 侧仍然真实握手并真实往返，但 gem5 的
# 指令与数据响应暂由 SimpleMemory 提供——因为 mem_sim 里还没有 gem5 ELF 的
# 初始镜像，若让链路侧成为唯一响应源，CPU 会执行到未初始化的垃圾。
set -euo pipefail

BRIDGE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
GEM5_DIR="${GEM5_DIR:-${BRIDGE_DIR}/../gem5}"
cd "${BRIDGE_DIR}"

GEM5_BIN="${GEM5_DIR}/build/X86/gem5.opt"
SOCKET="${SOCKET:-/tmp/bridge_gem5.sock}"
OUT_DIR="${OUT_DIR:-sim/gem5_chain}"
AXI_WIDTH="${AXI_WIDTH:-512}"   # 必须 <= Bridge 的 AXI_DATA_WIDTH_CFG

# 运行规模可通过环境变量收缩，便于先做 smoke
CORES="${CORES:-2}"
OPERATORS="${OPERATORS:-gemm}"
MAX_INSTS="${MAX_INSTS:-200000}"
SHADOW="${SHADOW:-1}"
RTL_BRIDGE="${RTL_BRIDGE:-0}"

if [[ "${RTL_BRIDGE}" == "1" ]]; then
    [[ "${AXI_WIDTH}" == "256" ]] || {
        echo "RTL Bridge 当前要求 AXI_WIDTH=256" >&2
        exit 2
    }
    CHAIN_TARGET="rtl-gem5-full-chain"
    CHAIN_BIN="build/obj_rtl_gem5_chain/Vstorage_bridge_top"
else
    CHAIN_TARGET="build/tb_gem5_full_chain"
    CHAIN_BIN="build/tb_gem5_full_chain"
fi

[[ -x "${GEM5_BIN}" ]] || {
    echo "找不到 ${GEM5_BIN}，请先在 gem5 目录执行：scons build/X86/gem5.opt -j\$(nproc)" >&2
    exit 2
}
[[ -f "${GEM5_DIR}/projects/memory/build/benchmark/llm_operators" ]] || \
    "${GEM5_DIR}/projects/memory/scripts/build_benchmark.sh"

mkdir -p "${OUT_DIR}"
rm -f "${SOCKET}"

echo "构建链路侧..."
make --no-print-directory "${CHAIN_TARGET}" >/dev/null

cleanup() {
    [[ -n "${chain_pid:-}" ]] && kill "${chain_pid}" 2>/dev/null || true
    rm -f "${SOCKET}"
}
trap cleanup EXIT

echo "启动链路侧（等待 gem5 监听）..."
./"${CHAIN_BIN}" --socket "${SOCKET}" > "${OUT_DIR}/chain.log" 2>&1 &
chain_pid=$!

sleep 0.5

GEM5_ARGS=(
    -d "${OUT_DIR}/gem5"
    "${GEM5_DIR}/projects/memory/gem5/soc_axi.py"
    --axi-data-width "${AXI_WIDTH}"
    --axi-direct-socket "${SOCKET}"
    --cores "${CORES}"
    --operators "${OPERATORS}"
    --max-insts "${MAX_INSTS}"
)
[[ "${SHADOW}" == "1" ]] && GEM5_ARGS+=(--axi-direct-shadow)

echo "运行 gem5..."
set +e
"${GEM5_BIN}" "${GEM5_ARGS[@]}" > "${OUT_DIR}/gem5.log" 2>&1
gem5_rc=$?
set -e
echo "gem5 退出码 ${gem5_rc}"

# gem5 退出会关闭 socket，链路侧据此排空并自行停下。
wait "${chain_pid}"; chain_rc=$?
chain_pid=""

echo
echo "===== gem5 ====="
tail -5 "${OUT_DIR}/gem5.log"
echo
echo "===== 链路侧 ====="
sed -n '/全链路结果/,$p' "${OUT_DIR}/chain.log"

echo
if [[ ${gem5_rc} -eq 0 && ${chain_rc} -eq 0 ]]; then
    echo "P3b（真 gem5）PASSED"
else
    echo "P3b（真 gem5）FAILED：gem5 rc=${gem5_rc} chain rc=${chain_rc}"
    echo "日志：${OUT_DIR}/gem5.log  ${OUT_DIR}/chain.log"
    exit 1
fi
