"""Check the four observed smoke transactions and describe their real path."""
import csv
import json


def check_observations(benchmark, source):
    value = benchmark['input']
    expected = (value + 1) & 0xffffffff
    operations = [('W', 0x90000000, value), ('R', 0x90000000, value),
                  ('W', 0x90010000, expected), ('R', 0x90010000, expected)]
    assert len(source) == 8, 'SMOKE requires exactly four requests and four responses'
    observed = []
    for i, (command, address, word) in enumerate(operations):
        request, response = source[2*i:2*i+2]
        assert request['event'] == 'request' and response['event'] == 'response', 'SMOKE request/response order differs'
        assert request['sequence'] == response['sequence'], 'SMOKE response sequence differs'
        assert int(request['tick']) < int(response['tick']), 'SMOKE completed before issue'
        if i:
            assert int(source[2*i-1]['tick']) < int(request['tick']), 'SMOKE operations must be sequential'
        for row in (request, response):
            assert row['command'] == command and int(row['address']) == address, 'SMOKE operation/address differs'
            assert int(row['response']) == 0, 'SMOKE memory error'
        payload = bytes.fromhex(response['data'])
        assert len(payload) == 16 and payload[:4] == word.to_bytes(4, 'little') and payload[4:] == bytes(12), 'SMOKE observed bytes differ from independent input + 1'
        if command == 'W':
            assert int(request['mask']) == int(response['mask']) == 15, 'SMOKE write mask differs'
            assert request['data'] == response['data'], 'SMOKE write response bytes differ'
        observed.append(int.from_bytes(payload[:4], 'little'))
    assert len({r['sequence'] for r in source if r['event'] == 'request'}) == 4, 'Duplicate SMOKE request'
    return {'passed': True, 'input': observed[1], 'output': observed[3], 'expected_output': expected,
            'operation': '(input + 1) modulo 2^32', 'external_transactions': 4,
            'reads': 2, 'writes': 2, 'native_transfer_bytes': 64, 'useful_word_bytes': 16}


def verify_smoke(root, config):
    with (root / 'npu_requests.csv').open() as stream:
        source = list(csv.DictReader(stream))
    result = check_observations(config['benchmark'], source)
    journeys = json.loads((root / 'memsim_journeys.json').read_text())
    assert len(journeys) == 4
    memory = json.loads((root / 'memsim_config.json').read_text())
    timeline = []
    for i in range(4):
        request, response = source[2*i:2*i+2]
        matching = [j for j in journeys if j['axi_id'] == int(request['wire_id'])]
        assert len(matching) == 1
        journey = matching[0]
        assert journey['command'] == request['command'] and journey['address'] == int(request['address'])
        assert len(journey['children']) == 1
        child = journey['children'][0]
        ticks = {'npu_request': int(request['tick']), 'axi_address': journey['axi_tick'],
                 'forward_flit_received': max(int(p['rx_last_tick_fs']) for p in journey['forward']),
                 'memory_accept': journey['accept_tick'],
                 'dram_command': int(child['command']['cycle']) * memory['period_fs'],
                 'memory_return': journey['return_tick'],
                 'reverse_flit_received': max(int(p['rx_last_tick_fs']) for p in journey['reverse']),
                 'axi_response': max(int(p['axi_tick_fs']) for p in journey['reverse']),
                 'npu_response': int(response['tick'])}
        assert list(ticks.values()) == sorted(ticks.values()), 'SMOKE path timestamps are not causal'
        timeline.append({'step': i+1, 'command': request['command'], 'address': hex(int(request['address'])),
                         'value': int.from_bytes(bytes.fromhex(response['data'])[:4], 'little'),
                         'physical_command': child['command']['command'],
                         'time_ns': {k: v/1e6 for k,v in ticks.items()},
                         'roundtrip_ns': (int(response['tick'])-int(request['tick']))/1e6})
    result['timeline'] = timeline
    result['timing_scope'] = 'Absolute simulation time from raw source, AXI, decoded Flits and online memory evidence'
    (root / 'smoke_summary.json').write_text(json.dumps(result, indent=2)+'\n')
    return result
