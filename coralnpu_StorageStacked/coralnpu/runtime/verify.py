#!/usr/bin/env python3
"""Independent transaction, source, byte, compute and waveform acceptance."""
import csv
import collections
import json
from pathlib import Path
import re
import sys

def read(p):
    return json.loads(p.read_text())

def verify(root):
    c = read(root / 'resolved.json')
    dev = c['device']
    a = c['architecture']
    b = c['benchmark']
    assert read(root / 'ucie_config.json') == a['ucie'], 'UCIe configuration was not applied'
    completion = read(root / 'completion.json')
    assert completion['passed'] and completion['code'] == 0
    memory_config = read(root / 'memsim_config.json')
    assert memory_config['standard'] == a['memory']['standard']
    assert memory_config['channels'] == a['memory']['channels']
    assert memory_config['timing_scale'] == a['memory']['scale']
    assert memory_config['queue_depth'] == a['memory']['queue']
    actual_system = read(root / 'config.json')
    assert actual_system['runtime'] == 'standalone-systemc'
    assert actual_system['device_period_fs'] == 10**9 // a['device_clock_mhz']
    assert 'cpu' not in actual_system
    native=read(root/'npu_model.json')
    n=a['npu']
    for key in ('fetch_l0_enabled','itcm_kib','dtcm_kib'):
        assert native[key]==n[key],'Loaded RTL differs: '+key
    assert native['rvv_enabled'] and native['float_enabled']
    small=(n['itcm_kib'],n['dtcm_kib'])==(8,32)
    assert native['dtcm_base']==(0x10000 if small else 0x100000)
    assert native['csr_base']==(0x30000 if small else 0x200000)

    actual_axi = actual_system['axi']
    assert actual_axi['period'] == a['axi']['period_ns'] * 1000000
    assert actual_axi['planes'] == a['axi']['planes']
    assert actual_axi['outstanding'] == a['axi']['outstanding']
    burst_beats=a['axi'].get('burst_beats',1)
    assert actual_axi.get('burst_beats',1) == burst_beats
    assert actual_axi['replay'] == a['axi']['replay']
    assert actual_axi['memsim_slots'] == a['memory']['slots']
    checks = {k: read(root / (k + '.json')) for k in ('check_summary', 'aou_check_summary', 'memsim_check', 'memsim_core')}
    assert all((v['passed'] for v in checks.values()))
    wave = read(root / 'wave_audit/summary.json')
    assert len(wave) == 1 and all((v['passed'] for v in wave.values()))
    def rows(name):
        with (root / name).open() as stream:
            return list(csv.DictReader(stream))
    tx = rows('transactions.csv')
    source = rows('npu_requests.csv')
    requests = {int(r['sequence']): r for r in source if r['event'] == 'request'}
    responses = {int(r['sequence']): r for r in source if r['event'] == 'response'}
    assert len(source) == 2 * len(requests) == 2 * len(responses) == 2 * len(tx)
    assert requests.keys() == responses.keys() == {int(t['substream']) for t in tx}
    events = rows('axi_events.csv')
    aw = [r for r in events if r['channel'] == 'AW']
    ws = [r for r in events if r['channel'] == 'W']
    write_beats=collections.deque(ws)
    writes_by_key={}
    for address in aw:
        count=int(address['len'])+1
        assert len(write_beats)>=count, 'Missing W beat'
        writes_by_key[address['id'],int(address['tick'])]=[write_beats.popleft() for _ in range(count)]
    assert not write_beats, 'Orphan W beat'
    channels = {ch: {} for ch in ('AW', 'AR', 'B', 'R')}
    for e in events:
        if e['channel'] in channels:
            channels[e['channel']].setdefault(e['id'], []).append(e)
    latency = []
    for t in tx:
        seq = int(t['substream']); req, rsp = requests[seq], responses[seq]
        assert all(req[k] == rsp[k] for k in ('command', 'address', 'native_id', 'wire_id', 'mask'))
        assert req['wire_id'] == t['id'] and req['command'] == t['command'] and req['address'] == t['address']
        assert int(req['native_id']) == int(t['stream'])
        begin, end = int(req['tick']), int(rsp['tick'])
        assert begin == int(t['begin_tick']) and end == int(t['end_resp_tick']) and end > begin
        assert end <= completion['tick_fs'] and int(rsp['response']) == 0 and int(t['status']) == 1
        lane = int(req['address']) % 32
        ch = 'AW' if req['command'] == 'W' else 'AR'
        address = [e for e in channels[ch][t['id']] if begin < int(e['tick']) <= int(t['axi_done_tick'])]
        assert len(address) == 1 and address[0]['address'] == req['address']
        address=address[0]
        count=int(address['len'])+1; width=1<<int(address['size'])
        assert count==burst_beats and count*width==16, 'AXI burst geometry differs from native request'
        ch = 'B' if req['command'] == 'W' else 'R'
        reply = [e for e in channels[ch][t['id']] if begin < int(e['tick']) <= int(t['axi_done_tick'])]
        assert len(reply) == (1 if ch=='B' else count) and int(reply[-1]['tick'])==int(t['axi_done_tick'])
        assert all(int(e['resp'])==0 for e in reply)
        if req['command'] == 'W':
            beats=writes_by_key[t['id'],int(address['tick'])]
            payload=bytes.fromhex(req['data'])
            for index,w in enumerate(beats):
                offset=index*width;lane=(int(req['address'])+offset)%32
                assert int(w['last'])==int(index==count-1), 'Incorrect WLAST'
                assert begin<int(w['tick'])<int(reply[0]['tick']), 'W beat outside native request lifetime'
                mask=(int(req['mask'])>>offset)&((1<<width)-1)
                assert int(w['strb'])==mask<<lane, 'Write byte enables differ'
                assert int(w['data']).to_bytes(32,'little')[lane:lane+width]==payload[offset:offset+width], 'Write beat bytes differ'
            assert rsp['data'] == req['data']
        else:
            payload=bytes.fromhex(rsp['data'])
            for index,r in enumerate(reply):
                offset=index*width;lane=(int(req['address'])+offset)%32
                assert int(r['last'])==int(index==count-1), 'Incorrect RLAST'
                assert int(r['data']).to_bytes(32,'little')[lane:lane+width]==payload[offset:offset+width], 'Read beat bytes differ'
        latency.append(end - begin)
    protocol = read(root / 'protocol_summary.json')
    assert latency and protocol['max_outstanding'] <= a['axi']['outstanding']
    if a['axi']['outstanding'] == 1:
        assert protocol['max_outstanding'] == 1 and protocol['capacity_denials'] > 0
    keys = {(r['command'], r['native_id']) for r in requests.values()}
    for key in keys:
        expected_order = [r['sequence'] for r in source if r['event'] == 'request' and (r['command'], r['native_id']) == key]
        actual_order = [r['sequence'] for r in source if r['event'] == 'response' and (r['command'], r['native_id']) == key]
        assert expected_order == actual_order
    sources = {dev: {'transactions': len(tx), 'bytes': 16 * len(tx),
                     'roundtrip_mean_ns': sum(latency) / len(latency) / 1000000.0}}
    log = (root / 'run.log').read_text()
    devices = {}
    assert completion['mailbox'] == 1611464704
    m = re.search('kernel finished after (\\d+) cycles', log)
    assert m
    devices['cycles'] = int(m[1])
    assert devices['cycles'] == completion['cycles']
    if b['name'] == 'tiny_llm':
        from verify_llm import verify_llm
        llm = verify_llm(root, c)
        devices.update(prompt_tokens=len(llm['prompt_tokens']), generated_tokens=len(llm['generated_token_ids']), mailbox=completion['mailbox'])
    elif b['name'] == 'smoke':
        from verify_smoke import verify_smoke
        smoke = verify_smoke(root, c)
        devices.update(input=smoke['input'], output=smoke['output'], mailbox=completion['mailbox'])
    else:
        assert b['name'] == 'custom'
        for e in b['expect']:
            address=int(e['address'],0); seen=[]
            for row in source:
                if row['event']!='response' or row['command']!='R': continue
                base=int(row['address']);lane=address-base
                if 0<=lane<=12:
                    seen.append(int.from_bytes(bytes.fromhex(row['data'])[lane:lane+4],'little'))
            assert seen and seen[-1]==e['value'], f'Expected output mismatch at {address:#x}'
        devices.update(checked_output_words=len(b['expect']),mailbox=completion['mailbox'])
    burst_summary={'beats_per_request':burst_beats,'bytes_per_beat':16//burst_beats,
                   'write_bursts':len(aw),'write_beats':len(ws),
                   'read_bursts':sum(e['channel']=='AR' for e in events),
                   'read_beats':sum(e['channel']=='R' for e in events)}
    result = {'passed': True, 'device': dev, 'benchmark': b, 'axi_data_bits': read(root / 'protocol_summary.json')['axi_data_bits'], 'completion': completion, 'sources': sources, 'devices': devices, 'checks': checks, 'wave': wave, 'axi_burst':burst_summary, 'npu_model':native, 'scope': 'CoralNPU RTL callbacks -> bounded AXI256 INCR burst adapter -> AXI2Flit -> UCIe -> online mem_sim -> returned data'}
    if b['name'] == 'tiny_llm':
        result['llm'] = llm
    if b['name'] == 'smoke':
        result['smoke'] = smoke
    assert result['axi_data_bits'] == 256
    (root / 'summary.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps({'passed': True, 'device': dev, 'sources': sources, 'devices': devices}, indent=2))
if __name__ == '__main__':
    from result_layout import grouped, working_result
    directory = Path(sys.argv[1])
    if grouped(directory):
        with working_result(directory):
            verify(directory)
    else:
        verify(directory)
