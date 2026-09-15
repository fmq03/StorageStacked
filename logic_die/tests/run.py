#!/usr/bin/env python3
import argparse,json,struct,subprocess,sys
from pathlib import Path
import numpy as np
from reference import bf16,prepare,query
def main():
    p=argparse.ArgumentParser();p.add_argument("--binary",default="build/system/logic_die/gate_unit")
    p.add_argument("--out",default="results/rtl");a=p.parse_args()
    out=Path(a.out);out.mkdir(parents=True,exist_ok=True);rng=np.random.default_rng(20260915)
    cases=[]
    shapes=[(1,0,1),(4,1,3),(8,3,1),(16,4,8),(32,5,12),(32,0,32),(7,2,5)]
    for mode in ["random","ties","signed","subnormal","rounding"]:
      selected_shapes=shapes+[(4,12,3)] if mode=="random" else shapes if mode in ["ties","signed"] else [(8,1,4)]
      for chunks,lg,k in selected_shapes:
        x=rng.normal(size=(chunks,1<<lg,128)).astype(np.float32)
        if mode=="ties": x.fill(1)
        if mode=="signed": x=np.arange(chunks,dtype=np.float32)[:,None,None]*np.ones_like(x)-chunks/2
        keys=bf16(x)
        if mode=="subnormal": keys=rng.integers(1,128,size=keys.shape,dtype=np.uint16)
        if mode=="rounding":
            for c in range(chunks):
                keys[c,0,:]=0x3f80+c;keys[c,1,:]=0x3f81+c
        means=prepare(keys)
        blob=bytearray(struct.pack("<5I",0x314d444c,chunks,lg,k,3))
        for cur in [0,chunks//2,chunks-1]:
            q=bf16(rng.normal(size=128) if mode!="signed" else -np.ones(128))
            if mode=="subnormal": q=bf16(np.full(128,1e30))
            mask,idx,_=query(means,q,cur,k)
            blob+=struct.pack("<3I",cur,mask,len(idx))+struct.pack("<"+"I"*len(idx),*idx)+q.tobytes()
        blob+=keys.tobytes()
        path=out/f"{mode}-{chunks}-{lg}-{k}.bin";path.write_bytes(blob)
        r=subprocess.run([a.binary,str(path)],capture_output=True,text=True)
        (path.with_suffix(".log")).write_text(r.stdout+r.stderr)
        if r.returncode: raise RuntimeError(f"{path.name}: {r.stderr}")
        result=json.loads(r.stdout);result.update(name=path.stem,chunks=chunks,chunk_log2=lg,top_k=k)
        cases.append(result);print(f"PASS {path.stem}",flush=True)
    (out/"summary.json").write_text(json.dumps({"passed":True,"cases":cases,"seed":20260915},indent=2)+"\n")
if __name__=="__main__":main()
