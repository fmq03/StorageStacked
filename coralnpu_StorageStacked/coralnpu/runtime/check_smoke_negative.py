#!/usr/bin/env python3
"""Require smoke verification to reject wrong computation and missing/reordered work."""
import copy
import csv
import json
from pathlib import Path
import sys
import tempfile
from configure import validate
import copy
from verify_smoke import check_observations
from result_layout import artifact

source, output = map(Path, sys.argv[1:])
config = json.loads(artifact(source,'resolved.json').read_text())['benchmark']
with artifact(source,'npu_requests.csv').open() as stream:
    original = list(csv.DictReader(stream))
assert check_observations(config, original)['passed']
checks = {}
for fault in ('wrong_input', 'wrong_output', 'wrong_readback', 'missing_transaction', 'extra_transaction', 'reordered_transactions'):
    rows = copy.deepcopy(original)
    if fault.startswith('wrong_'):
        index = {'wrong_input':0, 'wrong_output':4, 'wrong_readback':6}[fault]
        for row in rows[index:index+2]:
            if row['data']:
                data = bytearray.fromhex(row['data']); data[0] ^= 1; row['data'] = data.hex()
    elif fault == 'missing_transaction': rows = rows[:-2]
    elif fault == 'extra_transaction': rows += rows[-2:]
    else: rows = rows[2:4] + rows[:2] + rows[4:]
    try: check_observations(config, rows)
    except AssertionError as error: checks[fault] = {'rejected': True, 'reason': str(error)}
    else: raise AssertionError('Corrupted smoke evidence accepted: '+fault)
with tempfile.TemporaryDirectory(prefix='smoke-config-') as temporary:
    original_config=json.loads(artifact(source,'input.json').read_text())
    for label, replacement in [('negative',-1), ('overflow',2**32), ('boolean',True)]:
        candidate=copy.deepcopy(original_config);candidate['program']['input']=replacement
        try: validate(candidate)
        except ValueError as error: checks['config_'+label] = {'rejected':True, 'reason':str(error)}
        else: raise AssertionError('Bad smoke configuration accepted: '+label)
    for value in (0, 0xffffffff):
        candidate=copy.deepcopy(original_config);candidate['program']['input']=value;validate(candidate)
    checks['config_uint32_boundaries'] = {'passed':True}
result = {'passed':True, 'checks':checks}
output.write_text(json.dumps(result,indent=2)+'\n')
print(output.read_text())
