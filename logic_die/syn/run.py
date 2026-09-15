#!/usr/bin/env python3
import argparse,hashlib,json,os,subprocess
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
def main():
    p=argparse.ArgumentParser();p.add_argument("--library",default=os.environ.get("SS_TARGET_DB"))
    p.add_argument("--out",default="results/dc");p.add_argument("--clock",default="4.0")
    p.add_argument("--reports-only",action="store_true",help="report an existing, source-matched DDC without recompiling")
    a=p.parse_args()
    if not a.library: p.error("--library or SS_TARGET_DB required")
    lib=Path(a.library).resolve();out=Path(a.out).resolve();out.mkdir(parents=True,exist_ok=True)
    env=os.environ.copy();env.update(SS_TARGET_DB=str(lib),SS_DC_OUT=str(out),SS_CLOCK_NS=a.clock)
    hashes={str(x.relative_to(ROOT)):hashlib.sha256(x.read_bytes()).hexdigest() for x in sorted((ROOT/"logic_die/rtl").glob("*.sv"))}
    manifest={"library_name":lib.name,"library_sha256":hashlib.sha256(lib.read_bytes()).hexdigest(),
              "rtl_sha256":hashes,"clock_ns":float(a.clock),"tool":"Design Compiler X-2025.06-SP4"}
    if a.reports_only:
        if json.loads((out/"inputs.json").read_text())!=manifest:
            raise SystemExit("Saved DDC input manifest differs from current RTL/library/clock")
        if not (out/"logic_die.ddc").is_file():raise SystemExit("Saved DDC missing")
        if (out/"dc.log").exists() and not (out/"compile.log").exists():
            (out/"dc.log").rename(out/"compile.log")
    else:
        (out/"inputs.json").write_text(json.dumps(manifest,indent=2)+"\n")
    script="reports_only.tcl" if a.reports_only else "dc.tcl"
    with (out/"dc.log").open("w") as log:
      r=subprocess.run([os.environ.get("DC_SHELL","/eda/synopsys2025/syn/X-2025.06-SP4/bin/dc_shell"),"-f",str(ROOT/"logic_die/syn"/script)],cwd=out,env=env,stdout=log,stderr=subprocess.STDOUT)
    log=(out/"dc.log").read_text()
    if r.returncode or "SS_DC_COMPLETE" not in log or "Error:" in log:raise SystemExit("DC failed; inspect "+str(out/"dc.log"))
    data=json.loads((out/"summary.json").read_text())
    if data["unmapped_cells"]:raise SystemExit("Unmapped cells remain")
    print(json.dumps(data,indent=2))
if __name__=="__main__":main()
