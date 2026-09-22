#!/usr/bin/env python3
"""Join actual AXI/Flit traffic, online completions, DRAM/DFI and final memory bytes.

Run inspect_link.py first: its path CSV is independently decoded from raw Flits.
The exercised suites require one physical RD/WR per native child (no forwarding).
"""
import argparse
from collections import defaultdict, deque, Counter
import csv
import json
from pathlib import Path


def rows(path):
    with path.open() as f:
        return list(csv.DictReader(f))


def check(directory):
    cfg = json.loads((directory/'memsim_config.json').read_text())
    core = json.loads((directory/'memsim_core.json').read_text())
    bridge = json.loads((directory/'memsim_bridge_summary.json').read_text())
    period, granule, size = cfg['period_fs'], cfg['transaction_bytes'], cfg['window_bytes']
    config = json.loads((directory/'config.json').read_text())['systemc_kernel']['system']['axi']
    base = int(config['base'])
    assert cfg['phy'] == 'behavioral' and core['passed'] and bridge['passed']
    assert core['command_errors'] == core['dfi_errors'] == 0
    assert bridge['period_fs'] == period
    paths = {(r['channel'], r['axi_id'], r['axi_tick_fs']): r
             for r in rows(directory/'axi_flit_path.csv')}
    bus = rows(directory/'aou_events.csv')
    by_ch = {ch: [r for r in bus if r['channel'] == ch] for ch in ('AW','W','B','AR','R')}
    replies = {ch: defaultdict(deque) for ch in ('B','R')}
    for ch in replies:
        for r in by_ch[ch]:
            replies[ch][r['id']].append(r)
    requests = defaultdict(deque)
    request_list = []
    ws = iter(by_ch['W'])
    for ch in ('AW','AR'):
        for a in by_ch[ch]:
            width, beats = 1 << int(a['size']), int(a['len'])+1
            data, mask, returned = bytearray(), [], bytearray()
            forward = [paths[ch, a['id'], a['tick']]]
            reverse = []
            for beat in range(beats):
                lane = (int(a['address']) + beat*width) % 32
                if ch == 'AW':
                    w = next(ws)
                    data.extend(int(w['data_hex'],16).to_bytes(32,'little')[lane:lane+width])
                    mask.extend(str((int(w['strb_hex'],16) >> (lane+j)) & 1) for j in range(width))
                    forward.append(paths['W',a['id'],w['tick']])
                else:
                    r = replies['R'][a['id']].popleft()
                    returned.extend(int(r['data_hex'],16).to_bytes(32,'little')[lane:lane+width])
                    reverse.append(paths['R',a['id'],r['tick']])
            if ch == 'AW':
                r = replies['B'][a['id']].popleft()
                reverse.append(paths['B',a['id'],r['tick']])
            key = ('W' if ch == 'AW' else 'R', a['id'], a['address'], str(width*beats))
            request = dict(axi=a, command=key[0], address=int(a['address']),
                           bytes=width*beats, width=width, data=data.hex(),
                           mask=''.join(mask), returned=returned.hex(),
                           status=int(r['resp']), forward=forward, reverse=reverse)
            requests[key].append(request)
            request_list.append(request)
    assert next(ws,None) is None and all(not q for c in replies.values() for q in c.values())
    events = rows(directory/'memsim_bridge.csv')
    groups, children, completions = defaultdict(list), {}, {}
    for r in events:
        assert int(r['tick']) % period == 0
        groups[r['burst']].append(r)
        if r['event'] in ('submit','complete'):
            table = children if r['event'] == 'submit' else completions
            assert r['mem_id'] not in table, 'duplicate child/completion'
            table[r['mem_id']] = r
    assert children.keys() == completions.keys(), 'lost/orphan completion'
    assert len(children) == core['submitted'] == core['returned'] == bridge['children']
    assert len(groups) == bridge['bursts']
    assert sum(r['event']=='submit_stall' for r in events) == core['submit_stalls'] == bridge['submit_stalls']
    commands = rows(directory/'memsim_commands.csv')
    physical = defaultdict(list)
    for c in commands:
        if c['command'] in ('RD','WR'):
            physical[c['request_id']].append(c)
    assert physical.keys() == children.keys(), 'unaccounted physical transaction'
    dfi = rows(directory/'memsim_dfi_signals.csv')
    signals = defaultdict(list)
    for r in dfi:
        if r['kind'] in ('WRITE_DATA','READ_DATA'):
            signals[r['request_id']].append(r)
    assert signals.keys() == children.keys()
    for mid, s in children.items():
        c = completions[mid]
        assert all(s[k] == c[k] for k in ('burst','address','bytes','offset','command'))
        assert len(physical[mid]) == 1, 'this suite expects no forwarded/coalesced children'
        cmd = physical[mid][0]
        assert cmd['command'] == ('WR' if s['command']=='W' else 'RD')
        assert int(cmd['address'],0) == int(s['address'])-base
        assert int(cmd['cycle']) == int(c['issued_cycle'])
        assert int(s['tick']) <= int(c['issued_cycle'])*period <= int(c['completion_cycle'])*period <= int(c['tick'])
        assert int(c['status']) in (0,1), 'unexpected native failure in successful traffic'
        chunks = sorted(signals[mid],key=lambda r:(int(r['cycle']),int(r['phase'])))
        payload, masks = bytearray(), bytearray()
        for d in chunks:
            assert int(d['address'],0) == int(s['address'])-base+len(payload)
            assert int(d['issued_cycle']) == int(cmd['cycle'])
            assert d['kind'] == ('WRITE_DATA' if s['command']=='W' else 'READ_DATA')
            payload.extend(bytes.fromhex(d['dfi_wrdata'] if s['command']=='W' else d['dfi_rddata']))
            if s['command']=='W': masks.extend(bytes.fromhex(d['dfi_wrdata_mask']))
        assert payload.hex() == (s['data'] if s['command']=='W' else c['data']), 'DFI/host bytes differ'
        if s['command']=='W':
            assert list(masks) == [0 if b=='1' else 255 for b in s['mask']], 'DFI mask differs'
    if config.get('bridge_impl', 'cpp') == 'rtl':
        image = defaultdict(int)
        journeys = []
        for req in request_list:
            req['consumed'] = 0
            req['groups'] = []
        for serial, group in groups.items():
            accepts = [r for r in group if r['event']=='accept']
            returns = [r for r in group if r['event']=='return']
            assert len(accepts) == len(returns) == 1
            a, returned = accepts[0], returns[0]
            address, count = int(a['address']), int(a['bytes'])
            candidates = [req for req in request_list
                if req['command'] == a['command'] and req['consumed'] < req['bytes']
                and req['address'] + req['consumed'] == address
                and count <= req['bytes'] - req['consumed']]
            if a['command'] == 'W':
                candidates = [req for req in candidates
                    if a['data'] == req['data'][req['consumed']*2:(req['consumed']+count)*2]
                    and a['mask'] == req['mask'][req['consumed']:req['consumed']+count]]
            assert candidates, ('RTL child has no matching AXI parent', a)
            req = candidates[0]
            offset = req['consumed']
            relevant = [req['forward'][0]]
            if a['command'] == 'W':
                first_beat = offset // req['width']
                last_beat = (offset + count - 1) // req['width']
                relevant += req['forward'][1+first_beat:2+last_beat]
            assert int(a['tick']) >= max(int(p['rx_last_tick_fs']) for p in relevant)
            submitted = [s for s in group if s['event']=='submit']
            child_offset = 0
            child_read = bytearray()
            for s in submitted:
                assert int(s['offset']) == child_offset
                assert int(s['address']) == address + child_offset
                n, local = int(s['bytes']), int(s['address'])-base
                assert 0 < n <= granule and local//granule == (local+n-1)//granule
                c = completions[s['mem_id']]
                assert int(c['tick']) <= int(returned['tick'])
                if a['command'] == 'W':
                    assert s['data'] == a['data'][child_offset*2:(child_offset+n)*2]
                    assert s['mask'] == a['mask'][child_offset:child_offset+n]
                else:
                    child_read.extend(bytes.fromhex(c['data']))
                child_offset += n
            if int(a['status']):
                assert not submitted and int(returned['status']) == int(a['status'])
            else:
                assert child_offset == count and int(returned['status']) == 0
                if a['command'] == 'R':
                    assert child_read.hex() == returned['data']
            req['groups'].append((serial, a, returned, submitted, child_read, offset))
            req['consumed'] += count

        errors = 0
        for req in request_list:
            assert req['consumed'] == req['bytes'], 'AXI burst not fully covered by RTL children'
            assert req['groups'], 'AXI burst not delivered to memory'
            for g in req['groups']:
                last_return = int(g[2]['tick'])
                if req['command'] == 'R':
                    first_beat = g[5] // req['width']
                    last_beat = (g[5] + int(g[1]['bytes']) - 1) // req['width']
                    responses = req['reverse'][first_beat:last_beat+1]
                else:
                    responses = req['reverse']
                first_response = min(int(p['tx_first_tick_fs']) for p in responses)
                assert last_return <= first_response, \
                    ('RTL response Flit precedes child completion', req['command'],
                     req['address'], req['bytes'], last_return, first_response)
            statuses = {int(g[2]['status']) for g in req['groups']}
            assert statuses == {req['status']}
            errors += int(req['status'] != 0)
            if req['command'] == 'R':
                data = b''.join(bytes(g[4]) for g in req['groups'])
                assert data.hex() == req['returned'], 'RTL child read assembly differs from AXI'
            elif not req['status']:
                local = req['address'] - base
                for j,b in enumerate(bytes.fromhex(req['data'])):
                    if req['mask'][j] == '1': image[local+j] = b
            group_ids = [int(g[0]) for g in req['groups']]
            journeys.append(dict(burst=group_ids[0], backend_bursts=group_ids,
                axi_id=int(req['axi']['id']), command=req['command'], address=req['address'],
                bytes=req['bytes'], status=req['status'], axi_tick=int(req['axi']['tick']),
                forward=req['forward'], reverse=req['reverse'],
                accept_tick=min(int(g[1]['tick']) for g in req['groups']),
                return_tick=max(int(g[2]['tick']) for g in req['groups']),
                data=req['data'], mask=req['mask'],
                children=[dict(submit=s, complete=completions[s['mem_id']],
                    command=physical[s['mem_id']][0])
                    for g in req['groups'] for s in g[3]]))

        actual, covered = {}, set()
        for r in rows(directory/'memsim_image.csv'):
            addr, data = int(r['address'],0), bytes.fromhex(r['data'])
            assert 0 <= addr and addr+len(data) <= size
            assert bytes.fromhex(r['init']) == b'\xff'*len(data)
            assert not covered.intersection(range(addr,addr+len(data)))
            covered.update(range(addr,addr+len(data)))
            actual.update((addr+j,b) for j,b in enumerate(data))
        assert all(actual.get(a,0)==b for a,b in image.items())
        assert all(image.get(a,0)==b for a,b in actual.items())
        if size <= 1024*1024: assert len(covered)==size
        result = dict(passed=True,bursts=len(request_list),backend_bursts=len(groups),
                      children=len(children),errors=errors,image_bytes=len(covered),
                      window_bytes=size,period_fs=period,
                      commands=dict(Counter(c['command'] for c in commands)),
                      dfi_data_events=sum(map(len,signals.values())),
                      submit_stalls=bridge['submit_stalls'],
                      response_stalls=bridge['response_stalls'])
        (directory/'memsim_check.json').write_text(json.dumps(result,indent=2)+'\n')
        (directory/'memsim_journeys.json').write_text(json.dumps(journeys,indent=2)+'\n')
        return result
    image = defaultdict(int)
    journeys, errors = [], 0
    for serial, group in groups.items():
        accepts = [r for r in group if r['event']=='accept']
        returns = [r for r in group if r['event']=='return']
        assert len(accepts) == len(returns) == 1
        a, r = accepts[0], returns[0]
        req = requests[a['command'],a['axi_id'],a['address'],a['bytes']].popleft()
        assert a['data'] == req['data'] and a['mask'] == req['mask'], 'AXI/bridge write differs'
        assert int(a['tick']) >= max(int(p['rx_last_tick_fs']) for p in req['forward']), 'accepted before UCIe delivery'
        assert int(r['tick']) <= min(int(p['tx_first_tick_fs']) for p in req['reverse']), 'response Flit precedes memory'
        assert int(r['status']) == req['status']
        assert r['data'] == req['returned'], 'AXI/bridge read differs'
        submitted = [s for s in group if s['event']=='submit']
        offset = 0
        read_data = bytearray()
        for s in submitted:
            assert int(s['offset']) == offset and int(s['address']) == int(a['address'])+offset
            n, addr = int(s['bytes']), int(s['address'])-base
            assert 0 < n <= granule and addr//granule == (addr+n-1)//granule
            assert int(a['tick']) <= int(s['tick'])
            c = completions[s['mem_id']]
            assert int(c['tick']) <= int(r['tick'])
            if a['command']=='W':
                assert s['data'] == a['data'][offset*2:(offset+n)*2]
                assert s['mask'] == a['mask'][offset:offset+n]
            else: read_data.extend(bytes.fromhex(c['data']))
            offset += n
        if int(a['status']):
            errors += 1
            assert not submitted and int(a['status']) == int(r['status'])
        else:
            assert offset == int(a['bytes'])
            if a['command']=='R': assert read_data.hex() == r['data']
            else:
                addr = int(a['address'])-base
                for j,b in enumerate(bytes.fromhex(req['data'])):
                    if req['mask'][j]=='1': image[addr+j]=b
        for s in group:
            if s['event']=='submit_stall':
                accepted = children[s['mem_id']]
                assert all(s[k]==accepted[k] for k in ('burst','address','bytes','offset'))
                assert int(s['tick']) < int(accepted['tick'])
        journeys.append(dict(burst=int(serial), axi_id=int(a['axi_id']),command=a['command'],
            address=int(a['address']), bytes=int(a['bytes']), status=int(r['status']),
            axi_tick=int(req['axi']['tick']), forward=req['forward'], reverse=req['reverse'],
            accept_tick=int(a['tick']), return_tick=int(r['tick']), data=req['data'] or r['data'], mask=a['mask'],
            children=[dict(submit=s, complete=completions[s['mem_id']], command=physical[s['mem_id']][0]) for s in submitted]))
    assert errors == bridge['errors']
    assert all(not q for q in requests.values()), 'AXI burst not delivered to memory'
    actual, covered = {}, set()
    for r in rows(directory/'memsim_image.csv'):
        addr, data = int(r['address'],0), bytes.fromhex(r['data'])
        assert 0 <= addr and addr+len(data) <= size
        assert bytes.fromhex(r['init']) == b'\xff'*len(data)
        assert not covered.intersection(range(addr,addr+len(data)))
        covered.update(range(addr,addr+len(data))); actual.update((addr+j,b) for j,b in enumerate(data))
    assert all(actual.get(a,0)==b for a,b in image.items()) and all(image.get(a,0)==b for a,b in actual.items()), 'native memory image differs from AXI byte/mask reference'
    if size <= 1024*1024: assert len(covered)==size
    result = dict(passed=True,bursts=len(groups),children=len(children),errors=errors,
                  image_bytes=len(covered),window_bytes=size,period_fs=period,commands=dict(Counter(c['command'] for c in commands)),
                  dfi_data_events=sum(map(len,signals.values())),submit_stalls=bridge['submit_stalls'],
                  response_stalls=bridge['response_stalls'])
    (directory/'memsim_check.json').write_text(json.dumps(result,indent=2)+'\n')
    (directory/'memsim_journeys.json').write_text(json.dumps(journeys,indent=2)+'\n')
    return result


if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('directory',type=Path)
    print(json.dumps(check(p.parse_args().directory),indent=2))
