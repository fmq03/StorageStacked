#!/usr/bin/env python3
"""Offline byte-level UCIe audit and AXI/Flit correlation, independent of C++ decoders."""
import argparse
import binascii
import collections
import csv
import json
from pathlib import Path
from audit_wave import vcd_groups, CHANNELS


def read_csv(path):
    with path.open() as f: return list(csv.DictReader(f))


def write_csv(path, rows, fields=None):
    with path.open('w') as f:
        w=csv.DictWriter(f,fieldnames=fields or list(rows[0]))
        w.writeheader(); w.writerows(rows)


def bits(b,start,n):
    return (int.from_bytes(b,'big') >> (len(b)*8-start-n)) & ((1<<n)-1)


def gather(b):
    return bytes(b[i] for i in (62,63,64,65,128,129,190,191,192,193)) + b''.join(b[i:i+60] for i in (2,66,130,194))


def crc_ok(b):
    return all(binascii.crc_hqx(b[i:i+126],0xffff)==int.from_bytes(b[i+126:i+128],'big') for i in (0,128))


class Decoder:
    def __init__(self): self.carry=None
    def feed(self, row):
        b=bytes.fromhex(row['hex']); assert len(b)==250
        assert not (b[0]&12 or b[2]&15 or b[6]&15 or b[8]&15)
        start=b[0]>>4 | b[1]<<4 | (b[2]>>4)<<12 | b[3]<<16 | (b[6]>>4)<<24 | b[7]<<28 | (b[8]>>4)<<36 | b[9]<<40
        payload=b[10:]; pos=0; out=[]
        while pos<48:
            if self.carry is None:
                if not (start>>pos)&1: pos+=1; continue
                first=payload[pos*5]; typ=first>>4; dl=first&3
                if typ==0: length={0:1,4:2}[(first>>1)&7]
                elif typ in (1,2): length=3
                elif typ==5: length=1
                else:
                    assert dl<3
                    length={3:(8,15,30),4:(8,14,27),6:(7,14,27)}[typ][dl]
                self.carry=dict(raw=b'', length=length*5, start_seq=int(row['seq']),start_tick=int(row['tick_fs']))
            c=self.carry; take=min(c['length']-len(c['raw']),(48-pos)*5)
            assert not ((start>>pos)&((1<<(take//5))-1)&~1), 'message-start inside another message'
            c['raw']+=payload[pos*5:pos*5+take]; pos+=take//5
            if len(c['raw'])==c['length']:
                raw=c['raw']; typ=raw[0]>>4
                m=dict(type={0:'Credit',1:'WriteReq',2:'ReadReq',3:'WriteData',4:'ReadData',5:'WriteResp',6:'WriteDataFull'}[typ],
                       rp=(raw[0]>>2)&3, axi_id='',address='',length='',size='',data_hex='',strb_hex='',resp='',last='',
                       start_seq=c['start_seq'],end_seq=int(row['seq']),start_tick=c['start_tick'],end_tick=int(row['tick_fs']),raw_hex=raw.hex())
                if typ in (1,2):
                    m.update(axi_id=bits(raw,24,10),size=bits(raw,34,3),length=bits(raw,40,8),address=bits(raw,56,64))
                elif typ in (4,5):
                    m.update(axi_id=bits(raw,24,10),resp=bits(raw,34,2))
                    if typ==4: m.update(last=bits(raw,36,1),data_hex=raw[5:5+(32<<(raw[0]&3))].hex())
                elif typ in (3,6):
                    width=32<<(raw[0]&3)
                    m.update(data_hex=raw[3:3+width].hex(),strb_hex=raw[3+width:3+width+width//8].hex() if typ==3 else format((1<<width)-1,'x'))
                out.append(m); self.carry=None
        return out


def audit_wave(d):
    # Audit all five 256-bit channels, including control fields and stalls.
    state={}; held={}; actual=[]; cycles=[]
    payloads={ch:tuple('axi256.'+n for n in names) for ch,names in CHANNELS.items()}
    for ch in ('AW','AR'):
        payloads[ch]+=tuple('axi256.'+ch.lower()+n for n in ('lock','cache','prot','qos','user'))
    for ch in ('W','B','R'): payloads[ch]+=('axi256.'+ch.lower()+'user',)
    for tick,changes in vcd_groups(d/'axi_wave.vcd'):
        before=state.copy(); state.update(changes)
        edge=state.get('ACLK')==1 and before.get('ACLK')!=1
        if not before.get('ARESETn'): continue
        for ch,names in payloads.items():
            pre='axi256.'+ch.lower(); v=before[pre+'valid']; ready=before[pre+'ready']
            if v and (not edge or not ready):
                assert state[pre+'valid']==1 and all(state[n]==before[n] for n in names), (tick,ch,'unstable wide channel')
            if edge and v and ready:
                r=dict(tick_fs=tick,channel=ch,axi_id='',address='',length='',size='',data_hex='',strb_hex='',last='',resp='')
                if ch in ('AW','AR'):
                    r.update(axi_id=before[pre+'id'],address=before[pre+'addr'],length=before[pre+'len'],size=before[pre+'size'])
                elif ch=='W': r.update(data_hex=format(before[pre+'data'],'064x'),strb_hex=format(before[pre+'strb'],'x'),last=before[pre+'last'])
                elif ch=='B': r.update(axi_id=before[pre+'id'],resp=before[pre+'resp'])
                else:r.update(axi_id=before[pre+'id'],data_hex=format(before[pre+'data'],'064x'),last=before[pre+'last'],resp=before[pre+'resp'])
                actual.append(r)
            if edge and (v or ready) and tick<=200000000:
                cycles.append(dict(tick_fs=tick,channel=ch,valid=v,ready=ready,handshake=int(v and ready),payload=' '.join(n.split('.')[-1]+'='+hex(before[n]) for n in names)))
    write_csv(d/'axi256_handshakes.csv',actual)
    write_csv(d/'axi_first_200ns_cycles.csv',cycles,fields=['tick_fs','channel','valid','ready','handshake','payload'])
    # Exact comparison with the separately sampled SystemC monitor.
    expected=[r for r in read_csv(d/'aou_events.csv') if r['channel'] in CHANNELS]
    key=lambda r:(int(r.get('tick_fs',r.get('tick'))),r['channel'])
    for a,b in zip(sorted(actual,key=key),sorted(expected,key=key)):
        assert int(b['tick'])==a['tick_fs'] and b['channel']==a['channel']
        for dst,src in [('axi_id','id'),('address','address'),('length','len'),('size','size'),('last','last'),('resp','resp')]:
            if a[dst]!='': assert a[dst]==int(b[src]),(a,b)
        for field in ('data_hex','strb_hex'):
            if a[field]!='': assert int(a[field],16)==int(b[field],16)
    assert len(actual)==len(expected)
    return actual


def inspect(d):
    rows=read_csv(d/'ucie_flits.csv'); bykey=collections.defaultdict(dict); expected_seq=collections.Counter(); rx_seen=set()
    decoders={}; messages=[]; counts=collections.Counter(); physical=collections.Counter()
    for i,r in enumerate(rows,1):
        assert int(r['record'])==i
        if i>1: assert int(r['tick_fs'])>=int(rows[i-2]['tick_fs'])
        direction,event=r['direction'],r['event']; key=(direction,int(r['seq'])); item=bykey[key]
        raw=bytes.fromhex(r['hex']); assert len(raw)==int(r['bytes'])
        counts[event]+=1
        assert r['endpoint']==('SOC' if ((direction=='FWD') == event.startswith('TX')) else 'MEM')
        if event=='TX_FDI':
            assert 'tx' not in item and len(raw)==250; item['tx']=r; item['attempts']=[]; item['received']=[]
        elif event=='TX_FRAME':
            assert len(raw)==256 and crc_ok(raw)
            assert gather(raw)==bytes.fromhex(item['tx']['hex'])
            assert int(r['attempt'])==len(item['attempts'])+1 and int(r['replay'])==bool(item['attempts'])
            assert raw[0]==key[1]%256 and (raw[1]&1)==int(r['replay'])
            item['attempts'].append(r)
        elif event=='RX_FRAME':
            assert len(raw)==256 and int(r['attempt'])==len(item['received'])+1
            sent=item['attempts'][int(r['attempt'])-1]
            # 128 UI serialization + 4+8+4 UI pipeline, zero configured skew.
            assert int(r['tick_fs'])-int(sent['tick_fs'])>=144*41667
            good=crc_ok(raw); diff=(raw[0]-expected_seq[direction])%256
            status='crc_error' if not good else 'in_order' if diff==0 else 'duplicate' if diff>127 else 'seq_error'
            assert status==r['status']; physical[status]+=1
            if good: assert raw==bytes.fromhex(sent['hex']), 'undetected corruption'
            item['received'].append(r)
        elif event=='RX_FDI':
            assert key not in rx_seen and raw==bytes.fromhex(item['tx']['hex'])
            assert item['received'][-1]['status']=='in_order'
            assert key[1]==expected_seq[direction]; expected_seq[direction]+=1
            assert int(r['tick_fs'])>=int(item['received'][-1]['tick_fs'])
            rx_seen.add(key); item['rx']=r
        if event in ('TX_FDI','RX_FDI'):
            decoder=decoders.setdefault((direction,event),Decoder())
            for msg in decoder.feed(r):
                msg.update(direction=direction,event=event); messages.append(msg)
    for endpoint in ('SOC','MEM'):
        assert read_csv(d/('ucie_'+endpoint.lower()+'.csv'))==[r for r in rows if r['endpoint']==endpoint]
    paired=[]
    for (direction,seq),x in bykey.items():
        tx=x['tx']; rx=x.get('rx'); last=x['received'][-1] if x['received'] else None
        paired.append(dict(direction=direction,seq=seq,flit_id=tx['flit_id'],tx_fdi_tick=tx['tick_fs'],
            first_tx_frame_tick=x['attempts'][0]['tick_fs'],rx_fdi_tick=rx['tick_fs'] if rx else '',
            latency_fs=int(rx['tick_fs'])-int(tx['tick_fs']) if rx else '',attempts=len(x['attempts']),
            status='delivered' if rx else 'in_flight_at_exit'))
    for pair in paired:
        if pair['status']=='in_flight_at_exit':
            relevant=[m for m in messages if m['event']=='TX_FDI' and m['direction']==pair['direction']
                      and m['start_seq']<=pair['seq']<=m['end_seq']]
            assert all(m['type']=='Credit' for m in relevant), 'business flit still in flight'
            pair['content']='credit_or_empty'
        else: pair['content']='see_aou_messages'
    assert all(decoder.carry is None for decoder in decoders.values()), 'unfinished decoded message'
    write_csv(d/'flit_pairs.csv',paired)
    write_csv(d/'aou_messages.csv',messages)
    axi=audit_wave(d)
    correlate(d,axi,messages,bykey)
    stats=json.loads((d/'aou_summary.json').read_text())
    assert physical['crc_error']==stats['crc_errors']
    assert sum(len(x['attempts'])-1 for (direction,seq),x in bykey.items() if direction=='FWD')==stats['forward_replays']
    assert sum(len(x['attempts'])-1 for (direction,seq),x in bykey.items() if direction=='REV')==stats['reverse_replays']
    result=dict(passed=True,records=len(rows),events=dict(counts),rx_status=dict(physical),
                fdi_delivered=len(rx_seen),fdi_in_flight_at_exit=len(bykey)-len(rx_seen),axi_handshakes=len(axi),
                business_messages=sum(m['type']!='Credit' and m['event']=='TX_FDI' for m in messages))
    (d/'link_check_summary.json').write_text(json.dumps(result,indent=2)+'\n')
    print(d.name,'UCIe/AXI trace PASS',result)


def correlate(d,axi,messages,flits):
    tx=[m for m in messages if m['event']=='TX_FDI' and m['type']!='Credit']
    rx=[m for m in messages if m['event']=='RX_FDI' and m['type']!='Credit']
    # Compare independent reconstructions at both ends, including cross-flit messages.
    canon=lambda m:(m['direction'],m['start_seq'],m['end_seq'],m['raw_hex'])
    assert sorted(map(canon,tx))==sorted(map(canon,rx)), 'business messages not delivered exactly once'
    requests={ch:collections.deque(a for a in axi if a['channel']==ch) for ch in ('AW','AR')}
    # Per-ID AXI order is preserved; cross-ID / cross-RP transmission may reorder.
    request_lists=collections.defaultdict(collections.deque)
    writes_by_rp=collections.defaultdict(list); responses=collections.defaultdict(collections.deque)
    for a in axi:
        if a['channel'] in ('AW','AR'): request_lists[(a['channel'],a['axi_id'])].append(a)
        if a['channel'] in ('B','R'): responses[(a['channel'],a['axi_id'])].append(a)
    wide_w=collections.deque(a for a in axi if a['channel']=='W')
    axi_writes={}
    for aw in requests['AW']:
        beats=[]
        for beat in range(aw['length']+1):
            a=dict(wide_w.popleft())
            a['axi_id']=aw['axi_id']; a['address']=aw['address']+(beat<<aw['size'])
            beats.append(a)
        axi_writes[(aw['axi_id'],aw['tick_fs'])]=beats
    assert not wide_w
    pending_w=collections.defaultdict(collections.deque)
    for m in tx:
        if m['type']=='WriteReq':
            a=request_lists[('AW',m['axi_id'])].popleft()
            writes_by_rp[m['rp']].extend(axi_writes[(a['axi_id'],a['tick_fs'])])
            m['_axi']=a
        elif m['type']=='ReadReq': m['_axi']=request_lists[('AR',m['axi_id'])].popleft()
    for rp,beats in writes_by_rp.items(): pending_w[rp].extend(beats)
    path=[]
    for m in tx:
        typ=m['type']
        if typ in ('WriteReq','ReadReq'):
            a=m['_axi']
            assert (a['address'],a['length'],a['size'])==(m['address'],m['length'],m['size'])
        elif typ in ('WriteData','WriteDataFull'):
            a=pending_w[m['rp']].popleft()
            assert int(a['data_hex'],16)==int(m['data_hex'],16) and int(a['strb_hex'],16)==int(m['strb_hex'],16)
        else:
            a=responses[('B' if typ=='WriteResp' else 'R',m['axi_id'])].popleft()
            assert a['resp']==m['resp']
            if typ=='ReadData': assert a['last']==m['last'] and int(a['data_hex'],16)==int(m['data_hex'],16)
        rxfirst=flits[(m['direction'],m['start_seq'])]['rx']; rxlast=flits[(m['direction'],m['end_seq'])]['rx']
        if m['direction']=='FWD': assert a['tick_fs']<=m['start_tick']
        else: assert int(rxlast['tick_fs'])<=a['tick_fs']
        path.append(dict(channel=a['channel'],axi_id=a['axi_id'],axi_tick_fs=a['tick_fs'],address=a['address'],
            message=typ,rp=m['rp'],direction=m['direction'],start_seq=m['start_seq'],end_seq=m['end_seq'],
            tx_first_tick_fs=m['start_tick'],tx_last_tick_fs=m['end_tick'],
            rx_first_tick_fs=rxfirst['tick_fs'],rx_last_tick_fs=rxlast['tick_fs']))
    assert all(not q for q in request_lists.values()) and all(not q for q in responses.values()) and all(not q for q in pending_w.values())
    write_csv(d/'axi_flit_path.csv',path)

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('directories',nargs='+',type=Path)
    for d in p.parse_args().directories: inspect(d)
