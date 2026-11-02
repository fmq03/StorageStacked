#!/usr/bin/env python3
"""Generate an offline trace browser, a waveform overview, and a GTKWave signal list."""
import csv,json,sys
from pathlib import Path
from html import escape
from audit_wave import vcd_groups
from view_assets import chunks, compact

def read(p):
    with p.open() as f:return list(csv.DictReader(f))

def build(d):
    paths=read(d/'axi_flit_path.csv'); records=read(d/'ucie_flits.csv')
    (d/'axi_view.gtkw').write_text('[dumpfile] "axi_wave.vcd"\n[timestart] 0\n@28\nSystemC.ACLK\nSystemC.ARESETn\n'+''.join(
        '@28\nSystemC.axi256.'+ch+'valid\nSystemC.axi256.'+ch+'ready\n@22\n'+''.join('SystemC.axi256.'+ch+s+'\n' for s in fields)
        for ch,fields in [('aw',['id','addr','len','size']),('w',['data','strb','last']),('b',['id','resp']),('ar',['id','addr','len','size']),('r',['id','data','last','resp'])]))
    # A representative early active window; every edge is from the actual VCD.
    start=max(0,min(int(p['axi_tick_fs']) for p in paths)-12000000); end=start+120000000
    names=['ACLK','ARESETn']+['axi256.'+ch+s for ch in ('aw','w','b','ar','r') for s in ('valid','ready')]
    points={n:[(start,0)] for n in names}
    for t,delta in vcd_groups(d/'axi_wave.vcd'):
        if t>end:break
        for n in names:
            if n in delta:
                if t<=start:points[n]=[(start,delta[n])]
                else:points[n].append((t,delta[n]))
    width=1400; left=165; scale=(width-left-25)/(end-start); height=75+len(names)*35
    svg=[f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {width} {height}"><rect width="100%" height="100%" fill="white"/><style>text{{font:13px monospace}}</style>',
         '<text x="12" y="20">AXI256 sampled waveform · actual VCD · time in ns</text>']
    for t in range(start,end+1,10000000):
        x=left+(t-start)*scale
        svg.append(f'<path d="M{x} 42V{height-10}" stroke="#e4e8ed"/><text x="{x}" y="38">{t/1e6:g}</text>')
    for i,n in enumerate(names):
        y=65+i*35;svg.append(f'<text x="10" y="{y+5}">{escape(n)}</text>')
        pts=points[n]+[(end,points[n][-1][1])]; path=''
        for j,(t,v) in enumerate(pts):
            x=left+(t-start)*scale; yy=y-(12 if v else 0)
            if j==0:path=f'M{x} {yy}'
            else:path+=f'H{x}V{yy}'
        svg.append(f'<path d="{path}" fill="none" stroke="#176e9b" stroke-width="1.6"/>')
    svg.append('</svg>');(d/'axi_overview.svg').write_text('\n'.join(svg))
    path_index=chunks(d, 'trace_paths', paths, fields=('axi_id',))
    records.sort(key=lambda r:(r['direction'],int(r['seq']),int(r['tick_fs']),int(r['record'])))
    flit_index=chunks(d, 'trace_flits', records, fields=('direction','event','status'))
    template=r'''<!doctype html><html lang="zh"><meta charset="utf-8"><title>AXI / UCIe 运行记录</title>
<style>body{font:15px system-ui;max-width:1500px;margin:30px auto;padding:0 18px;color:#193047}h1{font-size:26px}p{line-height:1.7}input,select,button{font:inherit;padding:5px;margin:4px}table{border-collapse:collapse;width:100%;font-size:13px}th,td{padding:7px;border-bottom:1px solid #dbe2e8;text-align:left}th{position:sticky;top:0;background:#eef4f8}tr:hover{background:#edf7fc;cursor:pointer}.scroll{max-height:300px;overflow:auto;border:1px solid #cbd8e0;margin:12px 0}.raw{font:13px monospace;white-space:pre-wrap;word-break:break-all;background:#f4f7f9;padding:16px}.bad{background:#ffd8d3}img{width:100%}a{color:#08648c}</style>
<h1>AXI / UCIe 实际运行记录 · __CASE__</h1>
<p>先输入 AXI ID，再点击一条握手记录，下表会显示承载该消息的 Flit 在两端的发送和接收。最后点击 Flit 查看全部原始字节。
数据按页加载，每页最多100条；Flit按方向和序号排列。请将HTML与旁边的数据目录一起保留。
时间来自统一的 1fs 仿真时间轴。seq 是链路序号，不是 AXI ID；一个 Flit 可承载多条消息，一条消息也可跨 Flit。</p>
<p><a href="axi_wave.vcd">完整 VCD</a> · <a href="axi_view.gtkw">GTKWave 信号列表</a> · <a href="ucie_soc.csv">SoC 端原始日志</a> · <a href="ucie_mem.csv">存储端原始日志</a> · <a href="flit_pairs.csv">收发配对与延迟</a> · <a href="link_check_summary.json">离线核对结果</a></p>
<label>AXI ID <input id="id" type="number" value="1" min="1" max="1023"></label><button id="all">全部 ID</button>
<div class="scroll"><table><thead><tr><th>通道</th><th>ID</th><th>AXI握手(ns)</th><th>地址</th><th>消息 / RP</th><th>方向 / seq范围</th><th>发送(ns)</th><th>交付(ns)</th></tr></thead><tbody id="paths"></tbody></table></div>
<label>方向 <select id="dir"><option>ALL</option><option>FWD</option><option>REV</option></select></label>
<label>seq <input id="seq" placeholder="全部" type="number"></label>
<label>事件 <select id="event"><option>ALL</option><option>TX_FDI</option><option>TX_FRAME</option><option>RX_FRAME</option><option>RX_FDI</option></select></label>
<button id="errors">查看 CRC 错误</button><button id="reset">清除 Flit 筛选</button><span id="count"></span>
<div class="scroll"><table><thead><tr><th>时间(ns)</th><th>端点</th><th>方向</th><th>事件</th><th>seq</th><th>attempt</th><th>重放</th><th>结果</th><th>字节</th></tr></thead><tbody id="flits"></tbody></table></div>
<p>TX_FDI：链路接受250B协议内容；TX_FRAME：256B帧开始序列化；RX_FRAME：接收侧检查物理帧；RX_FDI：通过检查后向对端FIFO交付250B内容。CRC错误与重复帧不会重复交付。</p>
<div id="rawtitle"></div><div class="raw" id="raw">点击 Flit 行查看完整字节。</div>
<p>原始字节按偏移0开始排列；红色表示 RX_FRAME 与对应发送尝试的字节差异。256B 帧含2B帧头、10B分散协议头、240B消息区与两个CRC16；250B日志已经移除物理帧头及CRC。</p>
<img src="axi_overview.svg" alt="从VCD提取的AXI周期波形预览">
<p>预览只显示首段有效活动窗口；完整地址、数据、WSTRB、LAST及全部周期请用GTKWave打开VCD，加载axi_view.gtkw。
应在上升沿用沿前的VALID/READY判定握手。波形时间戳处的稳定值通常是该沿之后更新的值。</p>
<script src="view_store.js"></script><script>
const paths=__PATHS__, records=__RECORDS__;const $=s=>document.getElementById(s);let range=null,errors=false;
const ns=v=>(Number(v)/1e6).toFixed(6);const cell=s=>'<td>'+s+'</td>';
const pathPager=TraceStore.pager(paths,$('paths'),list=>{$('paths').replaceChildren();for(const p of list){let tr=document.createElement('tr');tr.innerHTML=[p.channel,p.axi_id,ns(p.axi_tick_fs),p.address?'0x'+Number(p.address).toString(16):'',p.message+' / '+p.rp,p.direction+' '+p.start_seq+'..'+p.end_seq,ns(p.tx_first_tick_fs),ns(p.rx_last_tick_fs)].map(cell).join('');tr.onclick=()=>{range=[Number(p.start_seq),Number(p.end_seq)];$('dir').value=p.direction;$('seq').value='';$('event').value='ALL';errors=false;flitTable()};$('paths').appendChild(tr)}});
function pathTable(){const id=$('id').value;return pathPager.reset(p=>!id||p.axi_id==id,m=>!id||m.axi_id.includes(id))}
const flitPager=TraceStore.pager(records,$('flits'),list=>{$('flits').replaceChildren();$('count').textContent='按需加载 · 每页100条';for(const r of list){let tr=document.createElement('tr');tr.innerHTML=[r.time_ns,r.endpoint,r.direction,r.event,r.seq,r.attempt,r.replay,r.status,r.bytes].map(cell).join('');tr.onclick=()=>raw(r);$('flits').appendChild(tr)}});
function flitTable(){const dir=$('dir').value,seq=$('seq').value,event=$('event').value,limits=range&&[...range],bad=errors;
return flitPager.reset(r=>(dir=='ALL'||r.direction==dir)&&(!seq||r.seq==seq)&&(event=='ALL'||r.event==event)&&(!limits||(Number(r.seq)>=limits[0]&&Number(r.seq)<=limits[1]))&&(!bad||r.status=='crc_error'),
m=>(dir=='ALL'||m.direction.includes(dir))&&(!seq||(Number(seq)>=m.lo&&Number(seq)<=m.hi))&&(event=='ALL'||m.event.includes(event))&&(!limits||(m.hi>=limits[0]&&m.lo<=limits[1]))&&(!bad||m.status.includes('crc_error')))}
let rawVersion=0;
async function raw(r){const token=++rawVersion;$('rawtitle').textContent=`${r.endpoint} ${r.direction} ${r.event} seq=${r.seq} attempt=${r.attempt} @ ${r.time_ns} ns`;let tx=null;
try {if(r.event=='RX_FRAME')for(const m of records){if(!m.direction.includes(r.direction)||!m.event.includes('TX_FRAME')||Number(r.seq)<m.lo||Number(r.seq)>m.hi)continue;
const data=await TraceStore.load(m.file);if(token!==rawVersion)return;tx=data.find(t=>t.direction==r.direction&&t.seq==r.seq&&t.attempt==r.attempt&&t.event=='TX_FRAME');if(tx)break;}
if(token!==rawVersion)return;let b=r.hex.match(/../g)||[],out='';for(let i=0;i<b.length;i+=16){out+=i.toString(16).padStart(4,'0')+'  ';for(let j=i;j<Math.min(i+16,b.length);j++){let diff=tx&&b[j]!=tx.hex.slice(j*2,j*2+2);out+=(diff?'<span class="bad">':'')+b[j]+(diff?'</span>':'')+' '}out+='\n'}$('raw').innerHTML=out;
}catch(error){if(token===rawVersion)$('raw').textContent=error.message}}
$('id').oninput=pathTable;$('all').onclick=()=>{$('id').value='';pathTable()};for(let n of ['dir','seq','event'])$(n).oninput=()=>{range=null;errors=false;flitTable()};$('errors').onclick=()=>{range=null;errors=true;$('seq').value='';$('dir').value='ALL';$('event').value='ALL';flitTable()};$('reset').onclick=()=>{range=null;errors=false;$('seq').value='';$('dir').value='ALL';$('event').value='ALL';flitTable()};pathTable();flitTable();
</script></html>'''
    text=template.replace('__CASE__',escape(d.name)).replace('__PATHS__',compact(path_index)).replace('__RECORDS__',compact(flit_index))
    (d/'trace_view.html').write_text(text)
    print('Trace viewer:',d/'trace_view.html')
if __name__=='__main__':
    for arg in sys.argv[1:]:build(Path(arg))
