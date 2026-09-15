"""Hashes for the code actually executed in the online experiment."""
import hashlib
import json
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()

def runtime_inputs():
    directories = ["axi2flit/systemc/include", "axi2flit/systemc/integration",
                   "axi2flit/systemc/src", "logic_die/rtl", "mem_sim/include",
                   "mem_sim/integration", "mem_sim/src", "protocol/include",
                   "systemc", "ucie-model/src", "vortex", "workloads"]
    extensions = {".h", ".hpp", ".cpp", ".sv", ".patch", ".c", ".S", ".ld"}
    paths = sorted({p for d in directories for p in (ROOT / d).rglob("*")
                    if p.is_file() and p.suffix in extensions})
    return {"files": {str(p.relative_to(ROOT)): sha256(p) for p in paths},
            "kernel_sha256": sha256(ROOT / "build/workloads/moba.bin"),
            "lock": json.loads((ROOT / "env/sources.lock.json").read_text())}

def verify_inputs(record):
    for name, expected in record["files"].items():
        if sha256(ROOT / name) != expected:
            raise RuntimeError("Source changed after measurement: " + name)
    if record["lock"] != json.loads((ROOT / "env/sources.lock.json").read_text()):
        raise RuntimeError("Dependency lock changed after measurement")
    if sha256(ROOT / "build/workloads/moba.bin") != record["kernel_sha256"]:
        raise RuntimeError("Kernel binary changed after measurement")
    components = {"vortex-gpu/vortex": "vortex-gpu/vortex",
                  "hardfloat": "vortex-gpu/vortex/third_party/hardfloat",
                  "softfloat": "vortex-gpu/vortex/third_party/softfloat"}
    for key, directory in components.items():
        actual = subprocess.check_output(["git", "-C", str(ROOT / directory), "rev-parse", "HEAD"], text=True).strip()
        if actual != record["lock"][key]:
            raise RuntimeError("Dependency checkout differs from lock: " + directory)
    diff = subprocess.check_output(["git", "-C", str(ROOT / "vortex-gpu/vortex"),
                                    "diff", "--binary", "--no-ext-diff", "--no-color"])
    if diff != (ROOT / "vortex/patches/external_memory.patch").read_bytes():
        raise RuntimeError("VORTEX working changes differ from the saved patch")
