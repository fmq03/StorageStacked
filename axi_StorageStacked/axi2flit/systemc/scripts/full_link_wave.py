#!/usr/bin/env python3
"""使用 Python 标准库将 SystemC VCD 转换为可独立打开的 AXI 波形页面。

原始 VCD 保存完整仿真波形。总线值使用字符串，避免 JavaScript 在处理
64 位地址和 1024 位数据时丢失精度。
"""
import argparse
import json
import re
from pathlib import Path


def read_vcd(path):
    wanted = ["clk", "rst_n", "link_state", "testcase"]
    for prefix, fields in (
        ("aw", "valid ready addr id len size"),
        ("w", "valid ready data strb last"),
        ("b", "valid ready id resp"),
        ("ar", "valid ready addr id len size"),
        ("r", "valid ready data id resp last"),
    ):
        wanted.extend("axi." + prefix + field for field in fields.split())
    definitions, data, widths, scopes = {}, {}, {}, []
    now, scale, header, timescale = 0, None, True, False
    units = {"s": 1e9, "ms": 1e6, "us": 1e3, "ns": 1, "ps": 1e-3, "fs": 1e-6}
    with path.open() as stream:
        for raw in stream:
            line = raw.strip()
            if header:
                if "$timescale" in line:
                    timescale = True
                if timescale:
                    m = re.search(r"(\d+)\s*(ms|us|ns|ps|fs|s)\b", line)
                    if m:
                        scale = int(m[1]) * units[m[2]]
                    if "$end" in line:
                        timescale = False
                if line.startswith("$scope"):
                    scopes.append(line.split()[2])
                elif line.startswith("$upscope"):
                    scopes.pop()
                elif line.startswith("$var"):
                    words = line.split()
                    name = ".".join(scopes[1:] + [words[4]])
                    if name in wanted:
                        definitions[words[3]] = name
                        data[name] = []
                        widths[name] = int(words[2])
                elif line.startswith("$enddefinitions"):
                    header = False
                continue
            if line.startswith("#"):
                now = int(line[1:])
                continue
            if not line or line.startswith("$"):
                continue
            if line[0] in "bB":
                value, code = line[1:].split()
            elif line[0] in "01xXzZ":
                value, code = line[0], line[1:]
            else:
                continue
            if code not in definitions:
                continue
            if scale is None:
                raise ValueError("VCD has no supported timescale")
            name = definitions[code]
            value = value.lower()
            value = value if "x" in value or "z" in value else format(int(value, 2), "x")
            data[name].append([round(now * scale, 6), value])
    if set(wanted) - data.keys():
        raise ValueError("missing signals: " + str(set(wanted) - data.keys()))
    return {"order": wanted, "data": data, "widths": widths, "end": now * scale}


HTML = r'''<!doctype html><html lang="zh-CN"><meta charset="utf-8">
<title>AXI full-link waveform</title>
<style>body{font:14px system-ui;margin:24px;color:#17283a;background:#f7f9fc}
input{width:105px}button,select,input{padding:6px;margin:4px}svg{background:white;border:1px solid #ccd5df}
.controls{position:sticky;top:0;background:#f7f9fc;padding:8px;z-index:1}
text{font:12px monospace;pointer-events:none}p{max-width:1000px}</style>
<h2>SoC AXI 实测波形</h2><p id="source"></p>
<p>数据来自 VCD。选择用例后可缩放、平移；鼠标悬停在总线区间上查看完整十六进制值。
仅在时钟上升沿且 VALID、READY 同时为 1 时发生握手。LEN = beat 数 − 1；RESP：0=OKAY，3=DECERR。
这是波形查看页，通过/失败结论请同时查看仿真日志。</p>
<div class="controls">用例 <select id="case"></select>
起点(ns)<input id="start" type="number" step="2">跨度(ns)<input id="span" type="number" min="2" step="2">
<button id="draw">显示</button><button id="in">放大 ×2</button><button id="out">缩小 ×2</button>
<button id="left">←</button><button id="right">→</button></div>
<svg id="plot" width="1400"></svg>
<script>
const W=__DATA__;
document.getElementById('source').textContent='VCD: '+W.source;
const labels=['启动','零初始化读','单拍写后读','突发长度扫描','掩码/全零选通','窄传输',
'256 beat 长突发','4KB 边界','越界错误响应','多笔并发/多 RP','同 ID 顺序','长背压/credit 恢复','随机突发'];
const ranges=W.data.testcase.filter(x=>parseInt(x[1],16)>0);
const select=document.getElementById('case'), start=document.getElementById('start'), span=document.getElementById('span');
for(let i=0;i<ranges.length;i++){
 const id=parseInt(ranges[i][1],16),o=document.createElement('option');o.value=i;
 o.textContent='TC'+id+' '+(labels[id]||'');select.appendChild(o);
}
function choose(){let i=Number(select.value);start.value=Math.max(0,ranges[i][0]-4);
 span.value=(i+1<ranges.length?ranges[i+1][0]:W.end)-Number(start.value);draw();}
const ns='http://www.w3.org/2000/svg',svg=document.getElementById('plot');
function add(tag,attrs,text){const e=document.createElementNS(ns,tag);for(const [k,v]of Object.entries(attrs))e.setAttribute(k,v);
 if(text!==undefined)e.textContent=text;svg.appendChild(e);return e;}
function draw(){const a=Math.max(0,Number(start.value)), duration=Math.max(2,Number(span.value)),b=a+duration;
 const left=170,right=1370,width=right-left,row=33;svg.replaceChildren();svg.setAttribute('height',W.order.length*row+55);
 const x=t=>left+(t-a)*width/duration;
 for(let i=0;i<=10;i++){const t=a+duration*i/10;add('line',{x1:x(t),x2:x(t),y1:26,y2:W.order.length*row+35,stroke:'#e4eaf0'});
 add('text',{x:x(t),y:17,'text-anchor':'middle'},t.toFixed(1));}
 W.order.forEach((name,index)=>{const y=45+index*row,list=W.data[name],bit=W.widths[name]===1;
 add('text',{x:6,y:y+8},name);let p=0;while(p+1<list.length&&list[p+1][0]<=a)p++;
 let count=0;
 for(let i=p;i<list.length&&list[i][0]<b;i++){
   const t0=Math.max(a,list[i][0]),t1=Math.min(b,i+1<list.length?list[i+1][0]:b),v=list[i][1];
   if(t1<=t0)continue;
   if(bit){const level=v==='1'?y-7:y+9;
     add('line',{x1:x(t0),x2:x(t1),y1:level,y2:level,stroke:'#076b48','stroke-width':1.3});
     if(i>p)add('line',{x1:x(t0),x2:x(t0),y1:y-7,y2:y+9,stroke:'#076b48'});
   }else{const rect=add('rect',{x:x(t0),y:y-9,width:Math.max(.5,x(t1)-x(t0)),height:21,fill:'#edf4fc',stroke:'#6794c3'});
     const title=document.createElementNS(ns,'title');title.textContent=name+' = 0x'+v+' ['+t0+', '+t1+') ns';rect.appendChild(title);
     if(x(t1)-x(t0)>42){let label='0x'+v;const chars=Math.min(24,Math.floor((x(t1)-x(t0)-8)/7));
       if(label.length>chars)label='…'+v.slice(-(chars-1));
       add('text',{x:x(t0)+4,y:y+6},label);}
   }
   if(++count>20000)break;
 }
 });
}
select.onchange=choose;document.getElementById('draw').onclick=draw;
document.getElementById('in').onclick=()=>{span.value=Math.max(2,Number(span.value)/2);draw()};
document.getElementById('out').onclick=()=>{span.value=Number(span.value)*2;draw()};
document.getElementById('left').onclick=()=>{start.value=Math.max(0,Number(start.value)-Number(span.value)/2);draw()};
document.getElementById('right').onclick=()=>{start.value=Number(start.value)+Number(span.value)/2;draw()};
select.value=Math.min(1,ranges.length-1);choose();
</script></html>'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("vcd", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    data = read_vcd(args.vcd)
    data["source"] = args.vcd.name
    output = args.output or args.vcd.with_suffix(".html")
    output.write_text(HTML.replace("__DATA__", json.dumps(data).replace("<", "\\u003c")), encoding="utf-8")
    print(f"Waveform viewer: {output} ({len(data['order'])} signals, {data['end']:.1f} ns)")


if __name__ == "__main__":
    main()
