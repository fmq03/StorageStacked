#!/usr/bin/env python3
import argparse,hashlib,json,struct,subprocess,sys
from pathlib import Path
import numpy as np
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/"logic_die/tests"))
from reference import bf16,prepare,query
from provenance import runtime_inputs,verify_inputs

def fixture(path,chunks,lg,k,nq,seed=20260915):
    rng=np.random.default_rng(seed+lg+chunks)
    keys=bf16(rng.normal(size=(chunks,1<<lg,128)));means=prepare(keys)
    blob=bytearray(struct.pack("<5I",0x314d444c,chunks,lg,k,nq))
    for j in range(nq):
        q=bf16(rng.normal(size=128));current=chunks-1
        mask,idx,_=query(means,q,current,k)
        blob+=struct.pack("<3I",current,mask,len(idx))+struct.pack("<"+"I"*len(idx),*idx)+q.tobytes()
    blob+=keys.tobytes();path.write_bytes(blob)
def main():
    p=argparse.ArgumentParser();p.add_argument("--out",default="results/experiments")
    p.add_argument("--quick",action="store_true")
    p.add_argument("--resume",action="store_true",help="reuse completed, independently verified cases")
    a=p.parse_args()
    out=Path(a.out);out.mkdir(parents=True,exist_ok=True)
    inputs=out/"inputs.json"
    if inputs.exists():
        if not a.resume: p.error("output already has inputs.json; choose a new directory or --resume")
        verify_inputs(json.loads(inputs.read_text()))
    else: inputs.write_text(json.dumps(runtime_inputs(),indent=2)+"\n")
    records=[]
    configs=[(4,1,3),(8,2,4)] if a.quick else [(32,lg,12) for lg in (0,1,2,3,4)]
    for c,lg,k in configs:
        f=out/f"c{c}-b{1<<lg}.bin";fixture(f,c,lg,k,4)
        for mode in ["logic_die","vortex_software"]:
            run=out/f"c{c}-b{1<<lg}-{mode}"
            cmd=[str(ROOT/"build/system/systemc/ss_sim"),str(f),str(ROOT/"build/workloads/moba.bin"),str(run)]
            if mode=="vortex_software":cmd.append("software")
            if not (a.resume and (run/"summary.json").exists()):
                with (out/(run.name+".log")).open("w") as log:
                    subprocess.run(cmd,stdout=log,stderr=subprocess.STDOUT,check=True)
            subprocess.run([sys.executable,str(ROOT/"scripts/verify_system.py"),str(run)],check=True,stdout=subprocess.DEVNULL)
            r=json.loads((run/"summary.json").read_text());r["artifact_directory"]=str(run)
            r["fixture_sha256"]=hashlib.sha256(f.read_bytes()).hexdigest();records.append(r)
            print(f"PASS {run.name}: {r['elapsed_fs']/1e9:.3f} us",flush=True)
            (out/"measurements.json").write_text(json.dumps(records,indent=2)+"\n")
if __name__=="__main__":main()
