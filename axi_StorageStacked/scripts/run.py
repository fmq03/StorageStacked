#!/usr/bin/env python3
"""Drive the public AXI signals and verify returned bytes against the input."""
import argparse
import csv
import datetime
import json
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
DEFAULTS = dict(base=0x90000000, size=0x30000000, planes=2, replay=False,
                standard='hbm4', slots=8, channels=2, scale=1, queue=4,
                response_hold=0, period_fs=2000000, max_ticks=200000000000,
                response_stall_cycles=3)


def integer(value, low, high, label):
    if type(value) is not int or not low <= value <= high:
        raise ValueError(f'{label} must be an integer in [{low}, {high}]')
    return value


def normalize(value):
    if not isinstance(value, dict) or set(value) - {'config', 'transactions'}:
        raise ValueError('Input must contain config and transactions only')
    config = value.get('config', {})
    if not isinstance(config, dict) or set(config) - DEFAULTS.keys():
        raise ValueError('Unknown configuration field')
    c = dict(DEFAULTS, **config)
    ranges = dict(base=(0, 2**64-1), size=(1, 2**64-1), planes=(1, 4),
                  slots=(1, 1023), channels=(1, 64), scale=(1, 1024), queue=(1, 1024),
                  response_hold=(0, 1000000), period_fs=(2, 10**9),
                  max_ticks=(1, 10**15), response_stall_cycles=(0, 100000))
    for key, limits in ranges.items():
        integer(c[key], *limits, key)
    if c['base'] + c['size'] > 2**64-1 or c['period_fs'] % 2:
        raise ValueError('Memory window overflows or clock period is not even')
    if type(c['replay']) is not bool or c['standard'] not in ('hbm3', 'hbm4', 'lpddr5', 'lpddr6'):
        raise ValueError('Invalid replay flag or memory standard')
    transactions = value.get('transactions')
    if not isinstance(transactions, list) or not transactions:
        raise ValueError('transactions must be a nonempty array')
    result = []
    for source in transactions:
        if not isinstance(source, dict) or set(source) - {'command', 'id', 'address', 'size', 'beats', 'data', 'strobe', 'expected', 'expected_response'}:
            raise ValueError('Invalid transaction fields')
        t = dict(source)
        if t.get('command') not in ('read', 'write'):
            raise ValueError('command must be read or write')
        t['id'] = integer(t.get('id', 1), 1, 1023, 'id')
        t['address'] = integer(t.get('address'), 0, 2**64-1, 'address')
        t['size'] = integer(t.get('size', 5), 0, 5, 'size')
        width = 1 << t['size']
        t['beats'] = integer(t.get('beats', len(t.get('data', [])) if t['command'] == 'write' else 1), 1, 256, 'beats')
        if t['address'] % width or t['address'] % 4096 + width * t['beats'] > 4096:
            raise ValueError('AXI INCR requests must be aligned and must not cross 4 KiB')
        t['expected_response'] = integer(t.get('expected_response', 0), 0, 3, 'expected_response')
        def hex_data(key):
            data = t[key]
            if not isinstance(data, list) or len(data) != t['beats']:
                raise ValueError(f'{key} count must equal beats')
            for text in data:
                if not isinstance(text, str) or len(text) != width*2 or any(ch not in '0123456789abcdefABCDEF' for ch in text):
                    raise ValueError(f'{key} must contain exactly {width} bytes per beat, in increasing address order')
            t[key] = [text.lower() for text in data]
        if t['command'] == 'write':
            if 'expected' in t:
                raise ValueError('expected is only valid for reads')
            if 'data' not in t:
                raise ValueError('write requires data')
            hex_data('data')
            t['strobe'] = t.get('strobe', [(1 << width)-1] * t['beats'])
            if not isinstance(t['strobe'], list) or len(t['strobe']) != t['beats']:
                raise ValueError('strobe count must equal beats')
            for mask in t['strobe']:
                integer(mask, 0, (1 << width)-1, 'strobe')
        else:
            if 'data' in t or 'strobe' in t:
                raise ValueError('read does not accept data/strobe inputs')
            if 'expected' in t:
                hex_data('expected')
        result.append(t)
    return dict(config=c, transactions=result)


def verify_responses(request, output):
    responses = json.loads((output / 'responses.json').read_text())
    assert len(responses) == len(request['transactions']), 'Missing responses'
    memory = {}
    previous = 0
    for t, r in zip(request['transactions'], responses):
        assert all(t[k] == r[k] for k in ('command', 'id', 'address'))
        assert r['response'] == t['expected_response'], 'Unexpected AXI response'
        assert previous <= r['begin_tick_fs'] < r['end_tick_fs']
        previous = r['end_tick_fs']
        width = 1 << t['size']
        if t['command'] == 'write':
            assert r['data'] == []
            if r['response'] == 0:
                for beat, (text, mask) in enumerate(zip(t['data'], t['strobe'])):
                    for lane, byte in enumerate(bytes.fromhex(text)):
                        if mask >> lane & 1:
                            memory[t['address'] + beat*width + lane] = byte
        else:
            assert len(r['data']) == t['beats']
            if r['response'] == 0:
                expected = [bytes(memory.get(t['address'] + beat*width + lane, 0) for lane in range(width)).hex()
                            for beat in range(t['beats'])]
                assert r['data'] == expected, 'Returned memory bytes differ from independent byte model'
            if 'expected' in t:
                assert r['data'] == t['expected'], 'Returned bytes differ from explicit expected input'
    with (output / 'axi_events.csv').open() as f:
        events = list(csv.DictReader(f))
    for channel, command in [('AW', 'write'), ('B', 'write'), ('AR', 'read')]:
        assert sum(e['channel'] == channel for e in events) == sum(t['command'] == command for t in request['transactions'])
    for channel, command in [('W', 'write'), ('R', 'read')]:
        assert sum(e['channel'] == channel for e in events) == sum(t['beats'] for t in request['transactions'] if t['command'] == command)
    return responses


def run_case(request, output):
    output.mkdir(parents=True, exist_ok=False)
    def dump(name, value):
        (output / name).write_text(json.dumps(value, indent=2) + '\n')
    dump('summary.json', dict(passed=False, stage='input validation'))
    try:
        request = normalize(request)
        dump('input.json', request)
        dump('config.json', dict(axi=request['config'], runtime='standalone-axi'))
        def invoke(command):
            with (output / 'run.log').open('a') as stream:
                subprocess.run(list(map(str, command)), stdout=stream, stderr=subprocess.STDOUT, check=True)
        invoke([ROOT / 'build/axi_storage', output / 'input.json', output])
        responses = verify_responses(request, output)
        # Decode the actual wire data independently, all the way to physical memory commands.
        for name in ('check_aou', 'inspect_link', 'check_memsim'):
            flags = ['--replay'] if name == 'check_aou' and request['config']['replay'] else []
            invoke([sys.executable, ROOT / 'scripts' / (name+'.py'), output, *flags])
        from audit_wave import audit
        wave, _, _ = audit(output, output / 'wave_audit')
        core = json.loads((output / 'memsim_core.json').read_text())
        assert core['passed'] and core['command_errors'] == core['dfi_errors'] == 0
        result = dict(passed=True, version=(ROOT / 'VERSION').read_text().strip(),
                      transactions=len(responses), end_tick_fs=responses[-1]['end_tick_fs'],
                      checks=['returned bytes', 'response IDs/status', 'AXI five channels',
                              'VCD/CSV agreement', 'AXI/Flit bytes', 'online memory/DRAM/DFI'], wave=wave)
        dump('summary.json', result)
        return result
    except Exception as error:
        dump('summary.json', dict(passed=False, error=str(error)))
        raise


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--input', type=Path, default=ROOT/'examples/roundtrip.json')
    p.add_argument('--output', type=Path)
    p.add_argument('--scale', type=int)
    p.add_argument('--replay', action='store_true')
    a = p.parse_args()
    request = json.loads(a.input.read_text())
    config = request.setdefault('config', {})
    if a.scale is not None:
        config['scale'] = a.scale
    if a.replay:
        config['replay'] = True
    output = (a.output or ROOT/'results'/datetime.datetime.now().strftime('%Y%m%d-%H%M%S-%f')).resolve()
    run_case(request, output)
    print('PASS: ' + str(output/'summary.json'))


if __name__ == '__main__':
    main()
