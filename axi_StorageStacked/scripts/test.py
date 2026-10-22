#!/usr/bin/env python3
"""Acceptance of the independent AXI signal-to-memory project."""
import argparse
import copy
import datetime
import json
from pathlib import Path
import subprocess
import sys
import tempfile
from run import ROOT, normalize, run_case, verify_responses
from smoke import run_smoke

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--output', type=Path)
a = p.parse_args()
output = (a.output or ROOT/'results'/('acceptance-'+datetime.datetime.now().strftime('%Y%m%d-%H%M%S'))).resolve()
output.mkdir(parents=True, exist_ok=False)
summary = output/'summary.json'
summary.write_text('{"passed":false,"stage":"running"}\n')
try:
    def invoke(cmd, name):
        with (output/name).open('w') as log:
            subprocess.run(list(map(str,cmd)), stdout=log, stderr=subprocess.STDOUT, check=True)
    invoke(['ctest','--test-dir',ROOT/'build','--output-on-failure'], 'native-tests.log')
    plan = json.loads(subprocess.check_output(['ctest','--test-dir',str(ROOT/'build'),'--show-only=json-v1'],text=True))
    assert len(plan['tests']) >= 19
    invoke([sys.executable, ROOT/'mem_sim/integration/check_online.py',
            ROOT/'build/mem_sim/libstoragestacked_memsim.so', output/'api'], 'api.log')
    example = json.loads((ROOT/'examples/roundtrip.json').read_text())
    cases = {'smoke': run_smoke(output/'smoke')}
    for name, cfg in [('default',{}), ('slow',{'scale':4}),
                      ('backpressure',{'slots':1,'queue':1,'response_hold':30,'response_stall_cycles':10})]:
        request = copy.deepcopy(example);request['config'].update(cfg)
        cases[name] = run_case(request,output/name)
    first_responses = [json.loads((output/name/'responses.json').read_text())[0]
                       for name in ('default', 'slow')]
    latency = [r['end_tick_fs']-r['begin_tick_fs'] for r in first_responses]
    # Compare the identical first request: later requests can cross different
    # periodic refresh windows, so whole-run time need not be monotonic.
    assert latency[1] > latency[0]
    periods = [json.loads((output/name/'memsim_config.json').read_text())['period_fs']
               for name in ('default','slow')]
    assert periods[1] == 4*periods[0]
    assert cases['backpressure']['wave']['stalled_edges']['B'] > 0
    assert cases['backpressure']['wave']['stalled_edges']['R'] > 0
    request = copy.deepcopy(example)
    request['config']['replay'] = True
    request['transactions'] *= 32
    cases['replay'] = run_case(request,output/'replay')
    # Legal AXI transactions outside the mapped memory window return DECERR.
    request = copy.deepcopy(example)
    request['transactions'] += [dict(command='read', address=0x80000000, size=2, id=7, expected_response=3),
                                dict(command='write',address=0x80000000,size=2,id=7,data=['01020304'],expected_response=3)]
    cases['decode_error'] = run_case(request,output/'decode_error')
    for name, command in [('read_only','read'),('write_only','write')]:
        request = copy.deepcopy(example)
        request['transactions'] = [t for t in request['transactions'] if t['command']==command]
        if command=='read':
            for t in request['transactions']:t.pop('expected',None)
        cases[name] = run_case(request,output/name)
    rejected = []
    for name, update in [('zero_id',{'id':0}),('wide_size',{'size':6}),
                         ('unaligned',{'address':0x90000001}),('cross_4k',{'address':0x90000fe0}),
                         ('wrong_strobe',{'strobe':[2**32]*4}),('bad_data',{'data':['ff']*4})]:
        request = copy.deepcopy(example);request['transactions'][0].update(update)
        try: normalize(request)
        except ValueError: rejected.append(name)
        else: raise AssertionError('Invalid input accepted: '+name)
    # A changed returned byte must fail even if the simulator exited normally.
    with tempfile.TemporaryDirectory() as td:
        target = Path(td)
        replies = json.loads((output/'default/responses.json').read_text())
        reply = next(r for r in replies if r['command']=='read')
        reply['data'][0] = 'ff' + reply['data'][0][2:]
        (target/'responses.json').write_text(json.dumps(replies))
        try: verify_responses(normalize(example),target)
        except AssertionError: rejected.append('corrupted_read_response')
        else: raise AssertionError('Corrupted response accepted')
    result = dict(passed=True,native_tests_passed=len(plan['tests']),
                  api_check=json.loads((output/'api/api_check.json').read_text()),
                  cases={k:{'passed':v['passed'],'transactions':v['transactions'],'end_tick_fs':v['end_tick_fs']} for k,v in cases.items()},
                  rejected_invalid_inputs=rejected,
                  memory_feedback={'passed':True,'first_write_latency_fs':latency,'memory_period_fs':periods})
    assert result['api_check']['passed']
    summary.write_text(json.dumps(result,indent=2)+'\n')
    print('PASS: '+str(summary))
except Exception as error:
    summary.write_text(json.dumps(dict(passed=False,error=str(error)),indent=2)+'\n')
    raise
