#!/usr/bin/env python3
import argparse,csv,json,struct,subprocess,tempfile
from pathlib import Path
from verify_system import flits
def main():
    p=argparse.ArgumentParser()
    p.add_argument("--rtl",default="results/acceptance/rtl")
    p.add_argument("--system",default="results/acceptance/system/c4-b2-logic_die")
    p.add_argument("--out",default="results/acceptance/negative_controls.json")
    a=p.parse_args();out=Path(a.out);out.parent.mkdir(parents=True,exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="ss-negative-") as d:
        d=Path(d)
        source=Path(a.rtl)/"random-4-1-3.bin";data=bytearray(source.read_bytes())
        # First query's expected mask, independently of all DUT signals.
        mask=struct.unpack_from("<I",data,24)[0];struct.pack_into("<I",data,24,mask^1)
        bad=d/"bad-oracle.bin";bad.write_bytes(data)
        r=subprocess.run(["build/system/logic_die/gate_unit",str(bad)],capture_output=True,text=True)
        if r.returncode!=1 or "Top-K mask differs" not in r.stderr:raise RuntimeError("oracle negative control escaped")
        source=Path(a.system)/"flits_forward.csv"
        with source.open() as f: rows=list(csv.DictReader(f))
        changed=False
        for row in rows:
            if row["event"]=="RX_FDI":
                b=bytearray.fromhex(row["bytes_hex"]);b[20]^=1;row["bytes_hex"]=b.hex();changed=True;break
        if not changed:raise RuntimeError("no RX payload to corrupt")
        bad=d/"corrupt-flits.csv"
        with bad.open("w") as f:
            w=csv.DictWriter(f,fieldnames=list(rows[0]));w.writeheader();w.writerows(rows)
        caught=False
        try:flits(bad)
        except AssertionError as e:
            if "integrity" not in str(e):raise
            caught=True
        if not caught:raise RuntimeError("flit corruption escaped")
    result={"passed":True,"corrupt_expected_mask":"detected_nonzero_exit","corrupt_rx_flit":"detected"}
    out.write_text(json.dumps(result,indent=2)+"\n");print(json.dumps(result))
if __name__=="__main__":main()
