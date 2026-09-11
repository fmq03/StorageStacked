"""Versioned simulator results; missing/invalid metrics are never silently zero.

Text input remains supported for historical artifacts. Simulation tools request
JSON explicitly, independently of the user's terminal view.
"""
from __future__ import annotations

import json
import math
from pathlib import Path
import subprocess
import tempfile


def parse_stats(text: str) -> dict[str, str]:
    fields: dict[str, str] = {}
    for line in text.splitlines():
        if line.lstrip().startswith("#") or ":" not in line:
            continue
        key, value = (item.strip() for item in line.split(":", 1))
        if key in fields and fields[key] != value:
            raise ValueError(f"conflicting metric {key!r}")
        fields[key] = value
    return fields


def read_result(path: Path, *, require_completed: bool = False) -> dict[str, str]:
    text = path.read_text(encoding="utf-8")
    if not text.lstrip().startswith("{"):
        if "模型 / MODEL" in text or "# ===== MODEL =====" in text:
            raise ValueError("compact report is for humans; supply --stats-json result.json "
                             "or a --stats-view diagnostic text file")
        metrics = parse_stats(text)
    else:
        def reject_constant(value: str):
            raise ValueError(f"non-finite JSON value: {value}")

        report = json.loads(text, parse_constant=reject_constant)
        if report.get("schema_version") not in (1, 2):
            raise ValueError(f"unsupported result schema in {path}")
        if report.get("run_status") not in {"completed", "truncated", "failed"}:
            raise ValueError(f"invalid run_status in {path}")
        raw = report.get("metrics")
        if not isinstance(raw, dict):
            raise ValueError(f"missing metrics object in {path}")
        if report["schema_version"] == 2:
            # Diagnostics are opt-in and never required by the public result.
            # This adapter is the sole compatibility mapping for old tools.
            raw = dict(report.get("diagnostics", {}))
            for section in ("model", "parameters", "metrics", "validation"):
                fields = report.get(section)
                if not isinstance(fields, dict):
                    raise ValueError(f"missing {section} object in {path}")
                raw.update(fields)
            raw.pop("run_status", None)
            if raw.get("avg_read_latency_ns") is not None:
                raw["avg_read_latency"] = (raw["avg_read_latency_ns"] * 1000 /
                                           raw["tick_duration_ps"])
            elif raw.get("completed_reads") == 0:
                # Legacy numerical zero only; canonical JSON uses null.
                raw["avg_read_latency"] = 0
            for stack in report.get("stacks", []):
                prefix = f"stack_{stack['stack']}_"
                for old, key in (("reads", "completed_reads"), ("writes", "completed_writes"),
                                 ("bw_GBps", "achieved_bw_GBps")):
                    raw[prefix + old] = stack[key]
                if stack.get("avg_read_latency_ns") is not None:
                    raw[prefix + "avg_read_latency"] = (
                        stack["avg_read_latency_ns"] * 1000 / raw["tick_duration_ps"])
            # Public null means unavailable, never zero. Keep absent when reading.
            raw = {k: v for k, v in raw.items() if v is not None}
        metrics = {}
        for key, value in raw.items():
            if not isinstance(value, (str, int, float, bool)):
                raise ValueError(f"invalid metric type: {key}")
            if isinstance(value, float) and not math.isfinite(value):
                raise ValueError(f"non-finite metric: {key}")
            metrics[key] = str(value).lower() if isinstance(value, bool) else str(value)
        status = report["run_status"]
        if "run_status" in metrics and metrics["run_status"] != status:
            raise ValueError(f"contradictory run status in {path}")
        metrics["run_status"] = status
        if require_completed and status != "completed":
            raise ValueError(f"simulation {status}: {report.get('error', '')}")
    if require_completed:
        require_valid_run(metrics)
    return metrics


def number(metrics: dict[str, str], key: str) -> float:
    if key not in metrics:
        raise ValueError(f"required metric is missing: {key}")
    try:
        result = float(metrics[key])
    except (TypeError, ValueError) as error:
        raise ValueError(f"invalid numeric metric {key}: {metrics[key]!r}") from error
    if not math.isfinite(result):
        raise ValueError(f"non-finite metric {key}: {metrics[key]!r}")
    return result


def count(metrics: dict[str, str], key: str) -> int:
    value = metrics.get(key, "")
    # Counters must be integers, not rounded floats or boolean strings.
    if not value.isdecimal():
        raise ValueError(f"missing/invalid non-negative integer metric {key}: {value!r}")
    return int(value)


def require_valid_run(metrics: dict[str, str]) -> None:
    if metrics.get("run_status", "completed") != "completed":
        raise ValueError("simulation did not complete")
    if metrics.get("hit_cycle_limit") != "false":
        raise ValueError("missing completion evidence or cycle limit reached")
    for key in ("remaining_requests", "remaining_pending", "data_mismatches"):
        if count(metrics, key) != 0:
            raise ValueError(f"invalid experiment: {key}={metrics[key]}")
    if count(metrics, "completed_reads") + count(metrics, "completed_writes") != count(metrics, "dram_transactions"):
        raise ValueError("completed transaction count differs from submitted DRAM transactions")
    count(metrics, "host_requests")
    count(metrics, "system_cycles")


def run_simulator(command: list[str], *, cwd: Path,
                  timeout: float | None = None,
                  diagnostic: bool = False) -> tuple[dict[str, str], str]:
    with tempfile.TemporaryDirectory(prefix="hbm_result_") as temp:
        result = Path(temp) / "result.json"
        completed = subprocess.run(
            [*command, "--stats-view", "diagnostic" if diagnostic else "summary",
             "--stats-json", str(result)],
            cwd=cwd, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            timeout=timeout, check=False)
        if completed.returncode:
            raise RuntimeError(f"simulation exited {completed.returncode}: {' '.join(command)}\n"
                               f"{completed.stdout}{completed.stderr}")
        return read_result(result, require_completed=True), completed.stdout
