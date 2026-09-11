#!/usr/bin/env python3
"""Independently compare both AXI boundaries and the actual UCIe endpoint counts."""
import argparse
import collections
import csv
import json
from pathlib import Path
from audit_wave import vcd_groups


def check(directory, replay=False):
    def rows(name):
        with (directory / name).open() as f:
            return list(csv.DictReader(f))
    narrow = rows('axi_events.csv')
    data_bits = json.loads((directory/'protocol_summary.json').read_text()).get('axi_data_bits',64)
    assert data_bits in (64,256)
    wide = rows('aou_events.csv')
    bus = [r for r in wide if r['channel'] in ('AW','W','B','AR','R')]
    assert len(narrow) == len(bus)
    key = lambda row: (int(row['tick']), ('AW','W','B','AR','R').index(row['channel']))
    narrow.sort(key=key); bus.sort(key=key)
    writes, reads = collections.deque(), {}
    wide_channels = collections.Counter()
    for lo, hi in zip(narrow, bus):
        ch = lo['channel']; wide_channels[ch] += 1
        assert (lo['tick'], ch) == (hi['tick'], hi['channel'])
        if ch in ('AW','AR'):
            for k in ('id','address','len','size'):
                assert lo[k] == hi[k], (ch,k,lo,hi)
            assert 0 < int(hi['id']) <= 1023
            c = [int(hi['address']), 1 << int(hi['size']), int(hi['len'])+1]
            if ch == 'AW': writes.append(c)
            else:
                assert hi['id'] not in reads
                reads[hi['id']] = c
        elif ch == 'W':
            c = writes[0]; shift = (c[0] % 32 & ~7)*8 if data_bits==64 else 0
            assert int(hi['data_hex'],16) == int(lo['data']) << shift, 'master/AXI2Flit WDATA mismatch'
            assert int(hi['strb_hex'],16) == int(lo['strb']) << (shift//8), 'wrong WSTRB expansion'
            assert lo['last'] == hi['last'] == str(int(c[2]==1))
            c[0] += c[1]; c[2] -= 1
            if not c[2]: writes.popleft()
        elif ch == 'R':
            assert lo['id'] == hi['id'] and lo['resp'] == hi['resp']
            c = reads[hi['id']]; shift = (c[0] % 32 & ~7)*8 if data_bits==64 else 0
            assert int(lo['data']) == (int(hi['data_hex'],16) >> shift) & ((1<<data_bits)-1), 'master/AXI2Flit RDATA mismatch'
            assert lo['last'] == hi['last'] == str(int(c[2]==1))
            c[0] += c[1]; c[2] -= 1
            if not c[2]: del reads[hi['id']]
        else:
            assert lo['id'] == hi['id'] and lo['resp'] == hi['resp']
    assert not writes and not reads
    s = json.loads((directory/'aou_summary.json').read_text())
    assert s['memory_completed'] == wide_channels['AW']+wide_channels['AR']
    assert s['target_reads'] == wide_channels['AR']
    assert s['target_writes'] == wide_channels['AW']
    assert s['target_read_beats'] == wide_channels['R']
    assert s['target_write_beats'] == wide_channels['W']
    assert s['memory_errors'] == sum(r['resp']=='3' for r in bus if r['channel']=='B') + sum(r['resp']=='3' and r['last']=='1' for r in bus if r['channel']=='R')
    assert s['tx_flits'] and s['rx_flits'] and s['order_violations']==0
    if replay:
        assert s['crc_errors'] > 0 and s['forward_replays']+s['reverse_replays'] > 0
    else:
        assert s['crc_errors'] == s['forward_replays'] == s['reverse_replays'] == 0
    # Independently recover wide W/R data from the VCD's pre-edge snapshot.
    values, recovered, flits = {}, [], collections.Counter()
    held = {}
    for tick, changes in vcd_groups(directory/'axi_wave.vcd'):
        before = values.copy(); values.update(changes)
        edge = values.get('ACLK')==1 and before.get('ACLK')!=1
        if not edge or not before.get('ARESETn'): continue
        for ch,v,ready,data in [('W','aou.wv','aou.wr','aou.wide_wdata'),('R','aou.rv','aou.rr','aou.wide_rdata')]:
            payload = (before.get(data), before.get('aou.wide_strb')) if ch=='W' else (before.get(data),)
            if ch in held:
                assert before[v] and payload==held[ch], 'wide payload changed under backpressure'
            if before[v] and not before[ready]: held[ch]=payload
            else: held.pop(ch,None)
            if before[v] and before[ready]: recovered.append((tick,ch,payload))
        for ch,v,ready in [('TX','aou.tx.valid','aou.txr'),('RX','aou.rx.valid','aou.rxr')]:
            if before[v] and before[ready]: flits[ch]+=1
    expected = []
    for r in bus:
        if r['channel'] in ('W','R'):
            payload=(int(r['data_hex'],16),)
            if r['channel']=='W': payload+=(int(r['strb_hex'],16),)
            expected.append((int(r['tick']),r['channel'],payload))
    expected.sort(key=lambda row: (row[0], row[1]))
    # W and R may handshake on the same edge. Their relative enumeration order
    # is not a protocol ordering constraint; compare both in the same order.
    recovered.sort(key=lambda row: (row[0], row[1]))
    assert recovered==expected, 'wide VCD/CSV mismatch'
    assert flits['TX']==s['tx_flits'] and flits['RX']==s['rx_flits']
    result=dict(passed=True, master_data_bits=data_bits, wide_handshakes=len(bus), wide_vcd_data_beats=len(recovered), **s)
    (directory/'aou_check_summary.json').write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps(result,indent=2))

if __name__=='__main__':
    p=argparse.ArgumentParser(); p.add_argument('directory',type=Path); p.add_argument('--replay',action='store_true')
    a=p.parse_args(); check(a.directory,a.replay)
