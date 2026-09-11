#!/usr/bin/env python3
"""Generate a deterministic, synthetic decoder-LLM memory trace.

This is a memory-system benchmark, not a functional language model.  It models
three traffic roles that the project can already represent:

* host: load NPU weights, initialize the Vortex KV cache, and exchange tokens;
* CoralNPU: stream quantized projection/FFN weights and write activations;
* Vortex: scan an FP16 KV cache and append the new K/V state.

The output is ordinary HETTrace, so validate/stats and the
``convert --preset memsim`` path need no benchmark-specific code.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))

from hettrace import addrmap  # noqa: E402
from hettrace.reader import OP_READ, OP_WRITE  # noqa: E402
from hettrace.synth import SynthWriter  # noqa: E402


def positive_int(raw: str) -> int:
    value = int(raw, 0)
    if value <= 0:
        raise argparse.ArgumentTypeError("必须是正整数: %r" % raw)
    return value


def align_up(value: int, alignment: int) -> int:
    return ((value + alignment - 1) // alignment) * alignment


class TrafficWriter:
    def __init__(self, output: Path, source: str, request_bytes: int):
        self.writer = SynthWriter(
            str(output), source, axi_data_bytes=request_bytes,
            level="interconnect", synth_flag=True,
        )
        self.source = source
        self.requests = 0
        self.reads = 0
        self.writes = 0
        self.bytes = 0

    def emit(self, tick: int, addr: int, size: int, op: int, ctx: int = 0) -> None:
        self.writer.emit(tick, addr, size, op, ctx=ctx)
        self.requests += 1
        self.bytes += size
        if op == OP_WRITE:
            self.writes += 1
        else:
            self.reads += 1

    def emit_range(
        self,
        tick: int,
        addr: int,
        size: int,
        op: int,
        request_bytes: int,
        arrival_step: int,
        ctx: int = 0,
    ) -> int:
        for offset in range(0, size, request_bytes):
            chunk = min(request_bytes, size - offset)
            self.emit(tick, addr + offset, chunk, op, ctx)
            tick += arrival_step
        return tick

    def close(self) -> dict:
        self.writer.close()
        return {
            "source": self.source,
            "requests": self.requests,
            "reads": self.reads,
            "writes": self.writes,
            "bytes": self.bytes,
        }


def check_region(name: str, start: int, size: int) -> None:
    base, capacity, _kind, _accessors = addrmap.REGIONS[name]
    if start < base or start + size > base + capacity:
        raise ValueError(
            "%s 布局 [0x%x, 0x%x) 超出区域 [0x%x, 0x%x)"
            % (name, start, start + size, base, base + capacity)
        )


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--output",
        type=Path,
        default=ROOT / "build/llm_memory/traces",
        help="HETTrace 输出目录",
    )
    parser.add_argument("--hidden-size", type=positive_int, default=128)
    parser.add_argument("--layers", type=positive_int, default=4)
    parser.add_argument("--context-tokens", type=positive_int, default=128)
    parser.add_argument("--decode-tokens", type=positive_int, default=8)
    parser.add_argument("--ffn-multiplier", type=positive_int, default=4)
    parser.add_argument("--weight-bytes", type=positive_int, default=1)
    parser.add_argument("--kv-element-bytes", type=positive_int, default=2)
    parser.add_argument("--request-bytes", type=positive_int, default=64)
    parser.add_argument(
        "--arrival-step",
        type=positive_int,
        default=500,
        help="同一源相邻请求的固定到达间隔，单位 HETTrace tick（默认 500 ps）",
    )
    parser.add_argument(
        "--layer-gap",
        type=positive_int,
        default=20000,
        help="相邻 layer 发射窗口之间的空隙，单位 tick",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if args.request_bytes > 64 or args.request_bytes & (args.request_bytes - 1):
        raise SystemExit(
            "--request-bytes 必须是 <=64 的 2 次幂（HETTrace v2 WSTRB 为 64 位）"
        )

    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    manifest_path = output / "benchmark.json"
    existing_traces = sorted(output.glob("*.hettrace"))
    if existing_traces:
        try:
            with manifest_path.open(encoding="utf-8") as handle:
                previous_manifest = json.load(handle)
        except (OSError, ValueError):
            raise SystemExit(
                "输出目录已有 trace 且没有有效 benchmark.json，拒绝覆盖: %s"
                % output
            )
        if previous_manifest.get("benchmark") != "synthetic-decoder-llm-memory":
            raise SystemExit("输出目录属于其他 benchmark，拒绝覆盖: %s" % output)
    expected_names = {"host.hettrace", "vortex.hettrace", "coralnpu.hettrace"}
    unexpected = sorted(
        path.name
        for path in output.glob("*.hettrace")
        if path.name not in expected_names
    )
    if unexpected:
        raise SystemExit(
            "输出目录含非本 benchmark 的 trace，拒绝混用: %s"
            % ", ".join(unexpected)
        )

    hidden = args.hidden_size
    intermediate = hidden * args.ffn_multiplier
    # Q/K/V + attention output: 4*H^2; SwiGLU gate/up/down: 3*H*I.
    layer_weight_bytes = (
        4 * hidden * hidden + 3 * hidden * intermediate
    ) * args.weight_bytes
    layer_weight_stride = align_up(layer_weight_bytes, 8192)
    kv_token_bytes = 2 * hidden * args.kv_element_bytes
    kv_layer_bytes = (args.context_tokens + args.decode_tokens) * kv_token_bytes
    kv_layer_stride = align_up(kv_layer_bytes, 8192)
    activation_bytes = align_up(hidden * args.kv_element_bytes, args.request_bytes)
    activation_token_stride = max(
        0x10000, align_up(args.layers * activation_bytes, 0x1000)
    )

    weight_base = addrmap.REGIONS["npu_work"][0]
    kv_base = addrmap.REGIONS["vortex_bar"][0] + 0x00200000
    activation_base = addrmap.REGIONS["shared_buffer"][0]
    check_region("npu_work", weight_base, args.layers * layer_weight_stride)
    check_region("vortex_bar", kv_base, args.layers * kv_layer_stride)
    check_region(
        "shared_buffer",
        activation_base,
        args.decode_tokens * activation_token_stride,
    )

    weight_requests = (layer_weight_bytes + args.request_bytes - 1) // args.request_bytes
    max_kv_bytes = (args.context_tokens + args.decode_tokens - 1) * kv_token_bytes
    max_kv_requests = (max_kv_bytes + args.request_bytes - 1) // args.request_bytes
    weight_window = (weight_requests + 1) * args.arrival_step
    activation_requests = (
        activation_bytes + args.request_bytes - 1
    ) // args.request_bytes
    npu_window = (
        weight_requests + activation_requests + 1
    ) * args.arrival_step
    kv_window = (
        max_kv_requests
        + (kv_token_bytes + args.request_bytes - 1) // args.request_bytes
        + 1
    ) * args.arrival_step
    vortex_offset = weight_window // 2
    layer_span = max(npu_window, vortex_offset + kv_window)
    layer_span += args.layer_gap
    token_span = args.layers * layer_span + args.layer_gap

    host = TrafficWriter(output, "host", args.request_bytes)
    npu = TrafficWriter(output, "coralnpu", args.request_bytes)
    vortex = TrafficWriter(output, "vortex", args.request_bytes)

    # Model load and prompt KV initialization happen before measured decode.
    host_tick = 1000
    for layer in range(args.layers):
        host_tick = host.emit_range(
            host_tick,
            weight_base + layer * layer_weight_stride,
            layer_weight_bytes,
            OP_WRITE,
            args.request_bytes,
            args.arrival_step,
            ctx=layer,
        )
    for layer in range(args.layers):
        host_tick = host.emit_range(
            host_tick,
            kv_base + layer * kv_layer_stride,
            args.context_tokens * kv_token_bytes,
            OP_WRITE,
            args.request_bytes,
            args.arrival_step,
            ctx=layer,
        )
    inference_start = align_up(host_tick + 10 * args.layer_gap, 1000)

    for token in range(args.decode_tokens):
        token_base = inference_start + token * token_span
        # CPU supplies one token embedding/control line and later consumes output.
        host.emit(
            token_base - args.arrival_step,
            activation_base + token * activation_token_stride,
            min(args.request_bytes, activation_bytes),
            OP_WRITE,
            ctx=token,
        )

        for layer in range(args.layers):
            ctx = (token << 16) | layer
            layer_base = token_base + layer * layer_span

            npu_tick = npu.emit_range(
                layer_base,
                weight_base + layer * layer_weight_stride,
                layer_weight_bytes,
                OP_READ,
                args.request_bytes,
                args.arrival_step,
                ctx=ctx,
            )
            npu.emit_range(
                npu_tick,
                activation_base
                + token * activation_token_stride
                + layer * activation_bytes,
                activation_bytes,
                OP_WRITE,
                args.request_bytes,
                args.arrival_step,
                ctx=ctx,
            )

            visible_tokens = args.context_tokens + token
            vortex_tick = layer_base + vortex_offset
            vortex_tick = vortex.emit_range(
                vortex_tick,
                kv_base + layer * kv_layer_stride,
                visible_tokens * kv_token_bytes,
                OP_READ,
                args.request_bytes,
                args.arrival_step,
                ctx=ctx,
            )
            vortex.emit_range(
                vortex_tick,
                kv_base
                + layer * kv_layer_stride
                + visible_tokens * kv_token_bytes,
                kv_token_bytes,
                OP_WRITE,
                args.request_bytes,
                args.arrival_step,
                ctx=ctx,
            )

        host.emit(
            token_base + token_span - args.arrival_step,
            activation_base
            + token * activation_token_stride
            + (args.layers - 1) * activation_bytes,
            min(args.request_bytes, activation_bytes),
            OP_READ,
            ctx=token,
        )

    source_stats = {
        item["source"]: item
        for item in (host.close(), npu.close(), vortex.close())
    }
    manifest = {
        "schema_version": 1,
        "benchmark": "synthetic-decoder-llm-memory",
        "functional_model": False,
        "projection_payload_semantics": "zero-surrogate-for-size-only",
        "warning": (
            "合成、open-loop 的 LLM 访存压力流；不执行 transformer 数值运算，"
            "mem_sim 的零值 payload 只承载请求大小；不能报告模型精度、"
            "tokens/s、IPC、原始数据正确性或端到端加速比。"
        ),
        "model": {
            "hidden_size": hidden,
            "intermediate_size": intermediate,
            "layers": args.layers,
            "context_tokens": args.context_tokens,
            "decode_tokens": args.decode_tokens,
            "weight_bytes": args.weight_bytes,
            "kv_element_bytes": args.kv_element_bytes,
        },
        "traffic": {
            "request_bytes": args.request_bytes,
            "arrival_step_ticks": args.arrival_step,
            "layer_gap_ticks": args.layer_gap,
            "layer_weight_bytes": layer_weight_bytes,
            "kv_bytes_per_token_per_layer": kv_token_bytes,
            "activation_bytes_per_layer": activation_bytes,
            "activation_token_stride": activation_token_stride,
            "inference_start_tick": inference_start,
            "layer_span_ticks": layer_span,
            "token_span_ticks": token_span,
        },
        "roles": {
            "host": "加载 NPU 权重、初始化 Vortex KV cache、提交/读取 token",
            "coralnpu": "流式读取 QKV/O/FFN 权重并写 activation",
            "vortex": "扫描已有 K/V 并追加当前 token 的 K/V",
        },
        "regions": {
            "weights": "npu_work",
            "kv_cache": "vortex_bar",
            "activations": "shared_buffer",
        },
        "source_stats": source_stats,
    }
    with manifest_path.open("w", encoding="utf-8") as handle:
        json.dump(manifest, handle, ensure_ascii=False, indent=2, sort_keys=True)
        handle.write("\n")

    total_requests = sum(values["requests"] for values in source_stats.values())
    total_bytes = sum(values["bytes"] for values in source_stats.values())
    print("LLM memory trace generated: %s" % output)
    print("requests=%d bytes=%d" % (total_requests, total_bytes))
    for name, values in sorted(source_stats.items()):
        print(
            "  %-9s req=%-8d R=%-8d W=%-8d bytes=%d"
            % (
                name,
                values["requests"],
                values["reads"],
                values["writes"],
                values["bytes"],
            )
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
