#!/usr/bin/env python3
"""Paged offline per-burst timeline from independently checked evidence."""
import csv
from collections import defaultdict
import json
from pathlib import Path
import sys
from view_assets import chunks, compact

p=Path(sys.argv[1])
journeys=json.loads((p/'memsim_journeys.json').read_text())
cfg=json.loads((p/'memsim_config.json').read_text())
with (p/'transactions.csv').open() as f: parents=list(csv.DictReader(f))
parents_by_id=defaultdict(list)
for row in parents: parents_by_id[int(row['id'])].append(row)
commands_by_id=defaultdict(list)
with (p/'memsim_commands.csv').open() as f:
    for row in csv.DictReader(f):
        if row['command'] in ('ACT','RD','WR'):
            commands_by_id[row['request_id']].append(row)
for j in journeys:
    candidates=[r for r in parents_by_id[j['axi_id']] if int(r['accepted_tick'])<=j['axi_tick']<=int(r['axi_done_tick'])]
    assert len(candidates)==1
    parent=candidates[0];j['parent']=parent
    ev=[['gem5 生成请求',int(parent['begin_tick'])],['TLM 接受请求',int(parent['accepted_tick'])],['AXI 地址握手',j['axi_tick']],
        ['请求 Flit 首次 TX_FDI',min(int(r['tx_first_tick_fs']) for r in j['forward'])],
        ['请求/写数据全部 RX_FDI',max(int(r['rx_last_tick_fs']) for r in j['forward'])],['mem_sim 桥接收 burst',j['accept_tick']]]
    for c in j['children']:
        s,r=c['submit'],c['complete'];mid=s['mem_id']
        ev.append([f'子请求 {mid} 提交',int(s['tick'])])
        for d in commands_by_id[mid]:
            tick=int(d['cycle'])*cfg['period_fs']
            if d['request_id']==mid and d['command'] in ('ACT','RD','WR') and int(s['tick'])<=tick<=int(r['tick']):
                ev.append([f"mem_sim {d['command']} / {mid}",tick])
        ev.append([f'子请求 {mid} 原生完成',int(r['completion_cycle'])*cfg['period_fs']])
    ev += [['桥交付完整 burst 响应',j['return_tick']],
           ['响应 Flit 首次 TX_FDI',min(int(r['tx_first_tick_fs']) for r in j['reverse'])],
           ['响应 Flit 全部 RX_FDI',max(int(r['rx_last_tick_fs']) for r in j['reverse'])],
           ['AXI 最后一拍响应',max(int(r['axi_tick_fs']) for r in j['reverse'])],
           ['gem5 请求完成',int(parent['end_resp_tick'])]]
    j['events']=sorted(ev,key=lambda e:e[1])
parts=chunks(p, 'memsim', journeys, size=50)
index=[[j['burst'],j['axi_id'],j['command'],j['address'],j['bytes']] for j in journeys]
data=compact(dict(index=index,chunks=parts,config=cfg))
html='''<!doctype html><html lang="zh"><meta charset="utf-8"><title>处理器 → UCIe → mem_sim 实测请求</title>
<style>body{font:15px system-ui;margin:28px;background:#f4f7fb;color:#182e48}main{max-width:1250px;margin:auto}select{font:inherit;padding:8px;max-width:100%}section{background:white;border-radius:10px;padding:18px;margin:18px 0;overflow:auto}table{border-collapse:collapse;width:100%}th,td{text-align:left;padding:7px;border-bottom:1px solid #dde5ef}code,pre{font-family:monospace;overflow-wrap:anywhere;white-space:pre-wrap}svg{width:100%;min-width:760px}a{color:#176ac3}.small{color:#52657a}</style>
<main><h1>一次请求怎样走完 处理器 → UCIe → mem_sim</h1>
<p>选择一个实际 AXI burst。时间来自本次日志，统一换算为 ns；拆分的 burst 可共享一个 gem5 父请求。</p>
<p>详情按需加载；复制页面时请同时保留旁边的数据目录。</p>
<label>筛选 burst、AXI ID 或地址 <input id="search" placeholder="例如 0x90000000"></label>
<button id="prev">上一页</button><button id="next">下一页</button><span id="page"></span>
<p><select id="pick"></select></p><p id="info"></p>
<section><h2>请求与响应时间线</h2><div id="timeline"></div><p class="small">mem_sim 完成点采用原生控制器的 HostResponse 定义；DFI 是由实发命令及其数据构建的行为级信号轨迹，并非外部 DDR RTL 引脚采样。</p></section>
<section><h2>DRAM 子请求</h2><div id="children"></div></section>
<section><h2>字节与跨链路标识</h2><p id="bytes"></p><div id="links"></div></section>
<section><h2>原始证据</h2><p><a href="trace_view.html">AXI/UCIe 逐拍查看器</a> · <a href="axi_wave.vcd">AXI 五通道 VCD</a> · <a href="ucie_soc.csv">SoC 端完整 Flit</a> · <a href="ucie_mem.csv">内存端完整 Flit</a> · <a href="memsim_bridge.csv">桥接事件</a> · <a href="memsim_commands.csv">DRAM 命令</a> · <a href="memsim_dfi_signals.csv">DFI 数据</a> · <a href="memsim_image.csv">最终内存镜像</a> · <a href="memsim_check.json">独立校验结果</a></p></section>
</main><script src="view_store.js"></script><script>const D=DATA;
const el=id=>document.getElementById(id),ns=t=>(Number(t)/1e6).toFixed(3),hex=n=>'0x'+Number(n).toString(16);
const table=(heads,rs)=>'<table><tr>'+heads.map(s=>'<th>'+s+'</th>').join('')+'</tr>'+rs.map(r=>'<tr>'+r.map(s=>'<td>'+s+'</td>').join('')+'</tr>').join('')+'</table>';
const labels=D.index.map(j=>`burst ${j[0]} · ${j[2]} · ${hex(j[3])} · ${j[4]} B · AXI ID ${j[1]}`);
let matches=D.index.map((_,i)=>i),page=0,version=0;
function choices(){el('pick').replaceChildren();for(const i of matches.slice(page*50,page*50+50)){let o=document.createElement('option');o.value=i;o.textContent=labels[i];el('pick').append(o)}el('page').textContent=`${matches.length} 笔 · 第 ${page+1}/${Math.max(1,Math.ceil(matches.length/50))} 页`;el('prev').disabled=page===0;el('next').disabled=(page+1)*50>=matches.length;draw()}
el('search').oninput=()=>{const q=el('search').value.trim().toLowerCase();matches=labels.map((s,i)=>s.toLowerCase().includes(q)?i:-1).filter(i=>i>=0);page=0;choices()};
el('prev').onclick=()=>{if(page){page--;choices()}};el('next').onclick=()=>{if((page+1)*50<matches.length){page++;choices()}};
async function draw(){const token=++version;if(!el('pick').value){el('info').textContent='没有匹配请求';for(const id of ['timeline','children','links','bytes'])el(id).textContent='';return}el('info').textContent='加载中…';
try {const index=Number(el('pick').value),block=await TraceStore.load(D.chunks[Math.floor(index/50)].file);if(token!==version)return;
let j=block[index%50],ev=j.events,first=ev[0][1],last=ev[ev.length-1][1],span=Math.max(1,last-first),h=ev.length*29+40;
el('info').textContent=`HBM4 / ${D.config.channels} channels / mem_sim 步长 ${ns(D.config.period_fs)} ns / 父请求总耗时 ${ns(j.parent.end_resp_tick-j.parent.begin_tick)} ns / AXI 状态 ${j.status}`;
let svg=`<svg viewBox="0 0 1100 ${h}" role="img" aria-label="请求各阶段实测时间线">`;
ev.forEach((e,i)=>{let y=25+i*29,x=330+(e[1]-first)/span*570;svg+=`<text x="8" y="${y+5}" font-size="13">${e[0]}</text><line x1="330" x2="900" y1="${y}" y2="${y}" stroke="#e4ebf4"/><circle cx="${x}" cy="${y}" r="5" fill="#1777b9"/><text x="930" y="${y+5}" font-size="13">${ns(e[1])} ns</text>`});el('timeline').innerHTML=svg+'</svg>';
el('children').innerHTML=table(['mem ID','局部地址','字节数','提交 ns','RD/WR cycle','完成 cycle','完成 ns'],j.children.map(c=>[c.submit.mem_id,hex(c.command.address),c.submit.bytes,ns(c.submit.tick),c.command.cycle,c.complete.completion_cycle,ns(c.complete.tick)]));
el('bytes').textContent=`数据（地址递增）：${j.data || '无'}；写 byte enable：${j.mask || '不适用'}`;
el('links').innerHTML=table(['方向','消息','RP','Flit 序号','发送 ns','接收 ns'],[...j.forward,...j.reverse].map(p=>[p.direction,p.message,p.rp,p.start_seq+'–'+p.end_seq,ns(p.tx_first_tick_fs),ns(p.rx_last_tick_fs)]));
}catch(error){if(token===version)el('info').textContent=error.message}}
el('pick').onchange=draw;choices();</script></html>'''
(p/'memsim_view.html').write_text(html.replace('DATA',data),encoding='utf-8')
print('Generated',p/'memsim_view.html')
