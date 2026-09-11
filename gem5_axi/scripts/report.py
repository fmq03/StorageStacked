#!/usr/bin/env python3
"""Regression assertions and a standalone, interactive result viewer."""
import json
import shutil
import sys
import tempfile
from pathlib import Path
from check import check, read_csv


def main(root):
    names = ("directed", "serial_l3", "serial_l9", "period_3ns", "cpu")
    cases = {}
    for name in names:
        d = root / name
        cases[name] = dict(check=json.loads((d / "check_summary.json").read_text()),
                           protocol=json.loads((d / "protocol_summary.json").read_text()),
                           transactions=read_csv(d / "transactions.csv"))
        if (d / "packet_lifecycle.csv").exists():
            cases[name]["packets"] = read_csv(d / "packet_lifecycle.csv")
    directed = cases["directed"]
    assert directed["protocol"]["max_outstanding"] == 4
    assert cases["serial_l3"]["protocol"]["max_outstanding"] == 1
    assert all(v["stall_cycles"] > 0 for v in directed["protocol"]["channels"].values())
    assert directed["check"]["coverage"]["aw_before_w"] > 0
    assert directed["check"]["coverage"]["w_before_aw"] > 0
    assert cases["period_3ns"]["protocol"]["period_ticks"] == 3000000
    tester = json.loads((root / "directed/tester_summary.json").read_text())
    assert tester["response_retries"] == 17
    l3 = {t["substream"]: t for t in cases["serial_l3"]["transactions"]}
    l9 = {t["substream"]: t for t in cases["serial_l9"]["transactions"]}
    changes = []
    for key, a in l3.items():
        b = l9[key]
        delta = ((b["axi_done_tick"] - b["accepted_tick"])
                 - (a["axi_done_tick"] - a["accepted_tick"]))
        expected = a["segments"] * 6 * 2000000
        assert delta == expected, ("latency perturbation", key, delta, expected)
        changes.append(dict(serial=key - 100, segments=a["segments"],
                            measured_delta_ns=delta / 1e6, expected_delta_ns=expected / 1e6))
    assert "CPU AXI PASS" in (root / "cpu/run.log").read_text()
    assert cases["cpu"]["check"]["transactions"] == 452
    # Prove that the external checker actually detects corrupted observations.
    detected = []
    for label, channel, field, mutate in (
            ("wrong_wlast", "W", "last", lambda v: 1 - int(v)),
            ("illegal_wstrb", "W", "strb", lambda v: int(v) | (1 << directed['protocol'].get('axi_data_bits',64)//8)),
            ("corrupt_rdata", "R", "data", lambda v: int(v) ^ 1)):
        with tempfile.TemporaryDirectory(prefix="axi-check-") as tmp:
            d = Path(tmp)
            for filename in ("axi_events.csv", "transactions.csv", "protocol_summary.json", "config.json"):
                shutil.copyfile(root / "directed" / filename, d / filename)
            import csv
            with (d / "axi_events.csv").open() as f:
                reader = csv.DictReader(f)
                fields, rows = reader.fieldnames, list(reader)
            row = next(r for r in rows if r["channel"] == channel)
            row[field] = str(mutate(row[field]))
            with (d / "axi_events.csv").open("w", newline="") as f:
                writer = csv.DictWriter(f, fields)
                writer.writeheader()
                writer.writerows(rows)
            try:
                check(d)
            except AssertionError:
                detected.append(label)
            else:
                raise AssertionError("checker missed " + label)
    summary = dict(passed=True, cases={k: v["check"] for k, v in cases.items()},
                   latency_perturbation=changes, negative_checks_detected=detected)
    (root / "regression_summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    document = """<!doctype html><meta charset="utf-8"><title>gem5 → AXI 验证</title>
<style>
body{font:16px system-ui,sans-serif;background:#f5f7fb;color:#172534;max-width:1200px;margin:36px auto;padding:0 20px}
h1{font-size:27px} select,button{font:inherit;padding:6px;margin:5px} .card{background:white;border:1px solid #dae0ea;border-radius:10px;padding:18px;margin:16px 0}
table{border-collapse:collapse;width:100%;font-size:14px}td,th{padding:7px;text-align:right;border-bottom:1px solid #eee}td:first-child,th:first-child{text-align:left}
.bar{height:19px;display:flex;background:#e6edf4;min-width:3px}.wait{background:#e9b44c}.bus{background:#3185c5}.resp{background:#8c62aa}
.small{font-size:13px;color:#506276}.scroll{max-height:500px;overflow:auto}a{color:#1464a0}
</style>
<h1>gem5 → TLM → AXI 五通道 → SystemC RAM</h1>
<p>同一进程；全局 1 fs/tick。结果来自实际 gem5 运行和独立 CSV 校验。</p>
<div class="card"><b>全部回归通过</b><p id="summary"></p>
<p class="small">范围：普通非一致性读写、有限 outstanding、双向背压、数据/字节使能、突发拆分与错误返回。尚未连接 AXI2Flit / UCIe / mem_sim。</p></div>
<label>场景 <select id="case"></select></label><button id="relative">按事务耗时显示</button>
<div class="card" id="stats"></div><div class="card"><b>事务时间线</b>
<p class="small">黄色：BEGIN_REQ 到准入；蓝色：准入到最后 B/R 握手；紫色：完成到 END_RESP。悬停查看具体时间；初始位置表示全局模拟时刻。</p>
<div class="scroll" id="timeline"></div></div>
<div class="card"><b>延迟扰动验证</b><p>无背压、单 outstanding：RAM 从 3 拍改成 9 拍，每个 AXI burst 增加 12 ns。所有父事务的准入到完成延迟变化都等于其 burst 数 × 12 ns。</p>
<p>独立校验器拒绝了三种人为损坏的记录：错误 WLAST、越界 WSTRB、错误 RDATA。</p>
<a href="regression_summary.json">回归 JSON</a></div>
<script>
const cases=__DATA__;const choice=document.querySelector('#case');let relative=false;
Object.keys(cases).forEach(k=>choice.add(new Option(k,k)));
document.querySelector('#summary').textContent=Object.entries(cases).map(([k,v])=>k+': '+v.check.transactions+' 个父事务').join(' · ');
function draw(){
 const key=choice.value,c=cases[key],ts=c.transactions.slice().sort((a,b)=>a.begin_tick-b.begin_tick);
 const end=Math.max(...ts.map(t=>t.end_resp_tick)),start=Math.min(...ts.map(t=>t.begin_tick));
 const scale=relative?Math.max(...ts.map(t=>t.end_resp_tick-t.begin_tick)):end-start;
 let s='<p>父事务 '+c.check.transactions+'；最大在途 '+c.protocol.max_outstanding+'；AXI 周期 '+c.protocol.period_ticks/1e6+' ns；检查读数据 '+c.check.coverage.checked_read_bytes+' bytes</p>';
 s+='<table><tr><th>通道</th><th>握手次数</th><th>VALID 等待周期</th></tr>';
 for(const [k,v] of Object.entries(c.protocol.channels))s+='<tr><td>'+k+'</td><td>'+v.handshakes+'</td><td>'+v.stall_cycles+'</td></tr>';
 s+='</table><p><a href="'+key+'/axi_wave.vcd">VCD 波形</a> · <a href="'+key+'/axi_events.csv">握手 CSV</a> · <a href="'+key+'/transactions.csv">事务 CSV</a> · <a href="'+key+'/run.log">运行日志</a></p>';
 document.querySelector('#stats').innerHTML=s;
 let h='<table><tr><th>ID / 地址 / 长度</th><th style="width:55%">时间线</th><th>起点 ns</th><th>总耗时 ns</th></tr>';
 for(const t of ts){
  const vals=[t.accepted_tick-t.begin_tick,t.axi_done_tick-t.accepted_tick,t.end_resp_tick-t.axi_done_tick];
  const total=t.end_resp_tick-t.begin_tick;
  const title='BEGIN '+t.begin_tick/1e6+' ns; ACCEPT '+t.accepted_tick/1e6+' ns; AXI DONE '+t.axi_done_tick/1e6+' ns; END_RESP '+t.end_resp_tick/1e6+' ns';
  h+='<tr><td>'+t.id+' '+t.command+' 0x'+t.address.toString(16)+' / '+t.bytes+'B</td><td><div title="'+title+'" class="bar" style="margin-left:'+(relative?0:(t.begin_tick-start)/scale*100)+'%;width:'+total/scale*100+'%">';
  vals.forEach((v,i)=>h+='<span class="'+['wait','bus','resp'][i]+'" style="width:'+v/total*100+'%"></span>');
  h+='</div></td><td>'+t.begin_tick/1e6+'</td><td>'+total/1e6+'</td></tr>';
 }
 document.querySelector('#timeline').innerHTML=h+'</table>';
}
choice.onchange=draw;document.querySelector('#relative').onclick=()=>{relative=!relative;document.querySelector('#relative').textContent=relative?'按全局时刻显示':'按事务耗时显示';draw();};draw();
</script>"""
    (root / "report.html").write_text(document.replace("__DATA__", json.dumps(cases)), encoding="utf-8")
    print("PASS: all scenarios, exact latency perturbations and checker fault injections")
    print(root / "report.html")


if __name__ == "__main__":
    main(Path(sys.argv[1]))
