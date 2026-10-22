#!/usr/bin/env python3
"""Six real AXI transactions with byte, waveform, Flit and online-memory checks."""
import argparse
import csv
import datetime
import json
from pathlib import Path

from run import ROOT, run_case


def run_smoke(output):
    # The shared auditors use assertions; never claim acceptance with them disabled.
    if not __debug__:
        raise RuntimeError('SMOKE requires Python assertions; unset PYTHONOPTIMIZE')
    output = Path(output).resolve()
    if output.exists():
        raise FileExistsError('Use a new SMOKE output directory: ' + str(output))

    def dump(name, value):
        (output / name).write_text(json.dumps(value, indent=2) + '\n')

    try:
        request = json.loads((ROOT / 'examples/smoke.json').read_text())
        result = run_case(request, output)
        replies = json.loads((output / 'responses.json').read_text())
        memory = json.loads((output / 'memsim_check.json').read_text())
        core = json.loads((output / 'memsim_core.json').read_text())
        aou = json.loads((output / 'aou_check_summary.json').read_text())
        with (output / 'axi_events.csv').open() as stream:
            writes = [row for row in csv.DictReader(stream) if row['channel'] == 'W']
        counts = dict(AW=2, W=3, B=2, AR=4, R=7)
        assert result['transactions'] == 6, 'SMOKE requires all six transactions'
        assert result['wave']['handshakes'] == counts, 'Missing AXI handshake coverage'
        assert all(r['response'] == 0 for r in replies), 'SMOKE requires OKAY responses'
        assert all(result['wave']['stalled_edges'].get(ch, 0) > 0 for ch in ('B', 'R')), 'B/R backpressure not exercised'
        assert int(writes[-1]['strb']) == 0x50000000, 'Narrow WSTRB did not reach lanes 28 and 30'
        assert int(writes[-1]['data']) == 0xddccbbaa << (28 * 8), 'Narrow WDATA lane placement differs'
        assert replies[-1]['data'] == ['aa1dcc1f'], 'Masked write did not preserve disabled bytes'
        assert memory['passed'] and memory['bursts'] == 6 and memory['children'] == 10
        assert memory['commands'].get('WR') == 3 and memory['commands'].get('RD') == 7
        assert core['passed'] and core['submitted'] == core['returned'] == 10
        assert core['command_errors'] == core['dfi_errors'] == memory['errors'] == 0
        assert aou['passed'] and aou['memory_completed'] == 6
        assert aou['target_reads'] == 4 and aou['target_writes'] == 2
        assert aou['tx_flits'] > 0 and aou['rx_flits'] > 0
        report = dict(
            passed=True, case='axi-storage-smoke', version=result['version'],
            transactions=6, handshakes=counts,
            stalled_edges=result['wave']['stalled_edges'],
            end_tick_fs=result['end_tick_fs'],
            masked_write=dict(address=0x9000001c, beat_strobe=5,
                              bus_strobe='0x50000000', returned_bytes=replies[-1]['data'][0]),
            readbacks=[dict(id=r['id'], address=r['address'], data=r['data'])
                       for r in replies if r['command'] == 'read'],
            memory=dict(bursts=memory['bursts'], children=memory['children'],
                        read_commands=7, write_commands=3,
                        period_fs=memory['period_fs'], image_bytes=memory['image_bytes'],
                        command_errors=core['command_errors'], dfi_errors=core['dfi_errors']),
            checks=result['checks'] + ['initial zero bytes', 'explicit expected read data',
                                      'two-beat bursts', 'narrow WDATA/WSTRB lanes 28..31',
                                      'masked bytes preserved', 'B/R backpressure observed'])
        dump('smoke_summary.json', report)
        result['smoke_passed'] = True
        result['smoke_report'] = 'smoke_summary.json'
        dump('summary.json', result)
        return report
    except Exception as error:
        # Keep either a run failure or a smoke-only coverage failure visible.
        if output.is_dir():
            failure = dict(passed=False, stage='smoke', error=str(error) or type(error).__name__)
            dump('smoke_summary.json', failure)
            dump('summary.json', failure)
        raise


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, help='New result directory; existing directories are rejected')
    args = parser.parse_args()
    output = (args.output or ROOT / 'results' / ('smoke-' + datetime.datetime.now().strftime('%Y%m%d-%H%M%S-%f'))).resolve()
    run_smoke(output)
    print('PASS: ' + str(output / 'smoke_summary.json'))


if __name__ == '__main__':
    main()
