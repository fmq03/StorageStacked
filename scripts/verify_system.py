#!/usr/bin/env python3
"""Independent offline checks over bytes and observed handshakes; no DUT import."""
import argparse,binascii,csv,json
from collections import Counter
from pathlib import Path

def require(ok,msg):
    if not ok:raise AssertionError(msg)
def rows(path):
    with path.open() as f:yield from csv.DictReader(f)

def flits(path):
    tx={};rx={};frames=0
    for r in rows(path):
        data=bytes.fromhex(r["bytes_hex"]);event=r["event"];key=(r["seq"],r["id"])
        if event in ("TX_FRAME","RX_FRAME"):
            require(len(data)==256,"physical frame length")
            require(binascii.crc_hqx(data[:126],0xffff)==int.from_bytes(data[126:128],"big"),"first frame CRC")
            require(binascii.crc_hqx(data[128:254],0xffff)==int.from_bytes(data[254:],"big"),"second frame CRC")
            frames+=1
            if event=="TX_FRAME" and r["replay"]=="0":
                payload=bytes(data[i] for i in (62,63,64,65,128,129,190,191,192,193))
                payload+=b"".join(data[b*64+2:b*64+62] for b in range(4))
                require(key in tx and payload==tx[key][1],"AoU physical mapping changed bytes")
        elif event=="TX_FDI":
            require(key not in tx,"duplicate transmit identity")
            require(len(data)==250,"logical flit length");tx[key]=(int(r["time_fs"]),data)
        elif event=="RX_FDI":
            require(key not in rx,"duplicate delivered flit");rx[key]=(int(r["time_fs"]),data)
    require(tx and tx.keys()==rx.keys(),"incomplete end-to-end flit delivery")
    for k,(t,b) in tx.items():
        require(rx[k][0]>=t and rx[k][1]==b,"flit byte integrity or causality failure")
    return {"logical_flits":len(tx),"checked_physical_events":frames}

def vcd(path,csv_counts):
    scopes=[];names={};values={};changes={};time=0;counts=Counter();header=True
    fields=["clk","awvalid","awready","wvalid","wready","arvalid","arready","bvalid","bready","rvalid","rready"]
    def finish():
        nonlocal values
        clock=names["axi.clk"]
        if values.get(clock)==0 and changes.get(clock)==1:
            for channel in ("aw","w","ar","b","r"):
                if values.get(names["axi."+channel+"valid"])==1 and values.get(names["axi."+channel+"ready"])==1:
                    counts[channel.upper()]+=1
        values.update(changes);changes.clear()
    with path.open() as f:
      for raw in f:
        line=raw.strip()
        if header:
            s=line.split()
            if not s:continue
            if s[0]=="$scope":scopes.append(s[2])
            elif s[0]=="$upscope":scopes.pop()
            elif s[0]=="$var":
                name=".".join(scopes[1:]+[s[4]]);names[name]=s[3]
            elif s[0]=="$enddefinitions":
                header=False
                require(all("axi."+x in names for x in fields),"VCD missing AXI channels")
                require(all("axi."+x in names for x in ("wdata","rdata","wstrb","awaddr","araddr","bid","rid")),"VCD missing payload fields")
            continue
        if not line or line.startswith("$"):continue
        if line[0]=="#":finish();time=int(line[1:])
        elif line[0] in "01xz":
            changes[line[1:]]=int(line[0]) if line[0] in "01" else None
        elif line[0]=="b":
            val,key=line[1:].split();changes[key]=int(val,2) if all(c in "01" for c in val) else None
    finish()
    require(counts==csv_counts,f"VCD/CSV handshake counts differ: {counts} vs {csv_counts}")
    return dict(counts)

def verify(root):
    summary=json.loads((root/"summary.json").read_text())
    core=json.loads((root/"memsim_core.json").read_text())
    cfg=json.loads((root/"memsim_config.json").read_text())
    require(summary.get("passed") is True and core.get("passed") is True,"run or memory model did not pass")
    require(core["command_errors"]==core["dfi_errors"]==0,"DRAM/DFI violation")
    require(core["submitted"]==core["returned"]>0,"native response leakage")
    require(summary["vortex_requests"]>0 and summary["response_stalls"]>0,"no actual VORTEX traffic/backpressure")
    link={d:flits(root/f"flits_{d}.csv") for d in ("forward","reverse")}
    events=[]
    for r in rows(root/"axi.csv"):events.append((int(r["time_fs"]),0,"axi",r))
    native=list(rows(root/"memory.csv"))
    for r in native:events.append((int(r["time_fs"]),1,"native",r))
    events.sort(key=lambda x:x[:2])
    memory={};writes={};reads={};pending={};counts=Counter();checked_reads=0;dma_reads=0
    for time,_,kind,r in events:
        address=int(r["address"]);ident=int(r["id"])
        if kind=="axi":
            ch=r["channel"];counts[ch]+=1
            require(0<=ident<1024,"AXI ID overflow")
            if ch=="AW":
                require(ident not in writes,"AW ID reuse before B");writes[ident]=[]
            elif ch=="W":
                require(ident in writes,"W without AW")
                data=bytes.fromhex(r["data_hex"]);mask=r["strobe_hex"]
                require(len(data)==len(mask)==32,"AXI W payload width")
                writes[ident].append((address+32*int(r["beat"]),data,mask))
            elif ch=="B":
                require(ident in writes,"B without AW")
                for a,data,mask in writes.pop(ident):
                    for i,b in enumerate(data):
                        if mask[i]=="1":memory[a+i]=b
            elif ch=="AR":
                require(ident not in reads,"AR ID reuse before last R");reads[ident]=0
            elif ch=="R":
                require(ident in reads,"R without AR")
                data=bytes.fromhex(r["data_hex"]);beat=int(r["beat"])
                require(beat==reads[ident],"R beat order")
                if address>=0x80000000:
                    expected=bytes(memory.get(address+beat*32+i,0) for i in range(32))
                    require(data==expected,"AXI read data differs from handshake-based memory oracle");checked_reads+=1
                reads[ident]+=1
                # VORTEX and loader requests use 64 B cache lines.
                if reads[ident]==2:del reads[ident]
        else:
            if r["event"]=="submit":
                require(ident not in pending,"duplicate native ID");pending[ident]=(time,address,r["origin"])
            else:
                require(ident in pending,"native completion without submit")
                t,a,origin=pending.pop(ident)
                require(a==address and origin==r["origin"] and time>=t,"native response identity/causality")
                require(int(r["status"])<=1,"native memory error")
                require(time>=int(r["completion_cycle"])*cfg["period_fs"],"response delivered before native completion")
                if r["write"]=="0":
                    data=bytes.fromhex(r["data_hex"])
                    require(data==bytes(memory.get(address+i,0) for i in range(32)),"native read/DMA data differs from independent AXI oracle")
                    dma_reads+=origin=="logic_die"
    require(not writes and not reads and not pending,"pending transaction at end")
    require(counts["AW"]==counts["B"] and counts["W"]==2*counts["B"] and counts["R"]==2*counts["AR"],"AXI transaction balance")
    if summary["mode"]=="logic_die":
        expected=summary["chunks"]*(1<<summary["chunk_log2"])*256+summary["queries"]*256
        require(summary["local_dma_bytes"]==expected and dma_reads*32==expected,"resident K/Q DMA accounting")
        require(all(q["dma_beats"]==8 for q in summary["query_results"]),"query illegally reloaded K")
    else:require(dma_reads==0,"software baseline used accelerator DMA")
    waves=vcd(root/"axi_logic_die.vcd",counts)
    return {"passed":True,"link":link,"axi_handshakes":waves,"axi_data_beats_checked":checked_reads,"dma_data_beats_checked":dma_reads}

def main():
    p=argparse.ArgumentParser();p.add_argument("run");a=p.parse_args();root=Path(a.run)
    result=verify(root);(root/"offline_verification.json").write_text(json.dumps(result,indent=2)+"\n");print(json.dumps(result))
if __name__=="__main__":main()
