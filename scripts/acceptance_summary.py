#!/usr/bin/env python3
"""Aggregate evidence at the end of the fail-fast Makefile acceptance chain."""
import argparse,json
from pathlib import Path
from provenance import sha256,verify_inputs

def main():
    p=argparse.ArgumentParser();p.add_argument("directory");a=p.parse_args();out=Path(a.directory)
    def read(name):return json.loads((out/name).read_text())
    rtl=read("rtl/summary.json");online=read("online-api/api_check.json")
    negative=read("negative_controls.json");system=read("system/measurements.json")
    assert rtl["passed"] and len(rtl["cases"])==24 and all(c["passed"] for c in rtl["cases"])
    assert online["passed"] and negative["passed"] and len(system)==4 and all(r["passed"] for r in system)
    verify_inputs(read("system/inputs.json"))
    for r in system:
        name=f"system/c{r['chunks']}-b{1<<r['chunk_log2']}-{r['mode']}"
        assert read(name+"/offline_verification.json")["passed"]
        assert read(name+"/memsim_core.json")["passed"]
    assert "100% tests passed, 0 tests failed out of 8" in (out/"native-memory.log").read_text()
    link=(out/"link.log").read_text()
    assert link.count("ALL FULL-LINK TESTS PASSED")>=9
    assert "PASS: scoreboard negative control detected corrupted expectation" in link
    result={"passed":True,"rtl_cases":24,"rtl_queries":72,"native_memory_tests":8,
            "online_system_cases":4,"full_link_pass_runs":link.count("ALL FULL-LINK TESTS PASSED"),
            "evidence_sha256":{n:sha256(out/n) for n in ("native-memory.log","link.log",
                "rtl/summary.json","online-api/api_check.json","negative_controls.json","system/inputs.json")}}
    (out/"summary.json").write_text(json.dumps(result,indent=2)+"\n");print(json.dumps(result))
if __name__=="__main__":main()
