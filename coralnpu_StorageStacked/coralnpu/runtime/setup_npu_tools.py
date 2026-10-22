#!/usr/bin/env python3
import json,os,subprocess
from pathlib import Path
root=Path(__file__).resolve().parent;deps=Path(os.environ["SS_DEPS_ROOT"])
lock=json.loads((root/"defaults/xpu-artifacts.lock.json").read_text())
for name,rel in [("bazel-8.6.0-linux-x86_64","xpu-tools/bin/bazel"),("lz4-1.10.0.tar.gz","xpu-downloads/lz4-1.10.0.tar.gz")]:
    if name.startswith("lz4") and (deps/"xpu-native/lib/liblz4.so").exists():continue
    path=deps/rel;path.parent.mkdir(parents=True,exist_ok=True)
    if not path.exists():
        if os.environ.get("SS_OFFLINE")=="1":raise RuntimeError("Missing cached dependency: "+str(path))
        subprocess.run(["curl","-fL","--retry","3",lock[name]["url"],"-o",str(path)],check=True)
    if path.stat().st_size!=lock[name]["bytes"]:raise RuntimeError("Unexpected file size: "+str(path))
(deps/"xpu-tools/bin/bazel").chmod(0o755)
