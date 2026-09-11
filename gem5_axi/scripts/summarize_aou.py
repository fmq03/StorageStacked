#!/usr/bin/env python3
import csv
import json
import re
import sys
from pathlib import Path

root=Path(sys.argv[1])
cases={}
lines=['# AXI2Flit 联调实测结果', '',
       '同一进程：gem5 → TLM → AXI64 → AXI256 → AXI2Flit → UCIe → AouTarget → SimpleBurstMemory。', '',
       '| 场景 | gem5 请求 | 内存 burst | AXI 握手 | 退出时间(ns) | CRC 错误 | 正/反向重放 |',
       '|---|---:|---:|---:|---:|---:|---:|']
for name in ('directed','rp2','replay','period_3ns','cpu','id_wrap'):
    d=root/name
    s=json.loads((d/'aou_check_summary.json').read_text())
    c=json.loads((d/'check_summary.json').read_text())
    assert s['passed'] and c['passed']
    tick=int(re.findall(r'EXIT: .* tick (\d+)',(d/'run.log').read_text())[-1])
    with (d/'transactions.csv').open() as f: tx=list(csv.DictReader(f))
    ids=[int(t['id']) for t in tx]
    if name=='id_wrap':
        assert len(ids)==1356 and max(ids)==1023 and len(set(ids))==1023
        assert (d/'run.log').read_text().count('CPU AXI PASS')==3
    cases[name]=dict(requests=c['transactions'],bursts=s['memory_completed'],
                    handshakes=c['axi_handshakes'],exit_tick=tick,
                    crc_errors=s['crc_errors'],forward_replays=s['forward_replays'],
                    reverse_replays=s['reverse_replays'],max_wire_id=max(ids),unique_wire_ids=len(set(ids)))
    lines.append(f"| [{name}]({name}/run.log) | {len(tx)} | {s['memory_completed']} | {c['axi_handshakes']} | {tick/1e6:.12g} | {s['crc_errors']} | {s['forward_replays']}/{s['reverse_replays']} |")
lines += ['', '[交互查看 AXI 与 Flit](directed/trace_view.html) · [查看 CRC 重放](replay/trace_view.html)', '', '所有场景均通过字节参考内存检查、TLM 时间关联、两侧 AXI 逐拍核对和 VCD 审计。',
          'CRC 测试启用确定性随机种子与 2% 物理帧错误注入，重放后数据检查仍通过。',
          'ID 复用测试完成 1356 笔请求，实际使用 1..1023，并在释放后重复使用。', '',
          '检查器故障注入：[结果](negative_checks.json)。',
          '波形示例：[directed/axi_wave.vcd](directed/axi_wave.vcd)。',
          'CPU：[日志](cpu/run.log)、[握手](cpu/axi_events.csv)、[请求延迟](cpu/transactions.csv)。',
          '这些数字包含测试内存和本轮适配策略，不能作为 mem_sim 或目标硬件性能数据。']
(root/'README.md').write_text('\n'.join(lines)+'\n')
(root/'summary.json').write_text(json.dumps(cases,indent=2)+'\n')
print('AoU summary:',root/'README.md')
