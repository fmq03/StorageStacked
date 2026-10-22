#!/usr/bin/env python3
"""Require real source, calculation, AXI/VCD, Flit and online-memory acceptance."""
import json
import subprocess
import sys
from pathlib import Path
from paths import ROOT,RUNTIME,STORAGE_ROOT,relative
from report import write_report,write_status
from portable import sanitize
from result_layout import working_result
out=Path(sys.argv[1])
def run(script,args):
    with (out/'verification.log').open('a') as stream:
        subprocess.run([sys.executable,relative(script),*map(str,args)],cwd=ROOT,stdout=stream,stderr=subprocess.STDOUT,check=True)
def validate():
    write_status(out,'validating saved raw evidence')
    c=json.loads((out/'resolved.json').read_text())
    for name in ('check','check_aou','inspect_link','check_memsim'):
        args=[out]
        if name=='check_aou' and c['architecture']['axi']['replay']:args+=['--replay']
        run((RUNTIME/'checks' if name=='check' else STORAGE_ROOT/'scripts')/(name+'.py'),args)
    run(STORAGE_ROOT/'scripts/audit_wave.py',[out,'--output',out/'wave_audit'])
    run(RUNTIME/'verify.py',[out])
    for name in ('trace_view','memsim_view'):run(STORAGE_ROOT/'scripts'/(name+'.py'),[out])
    write_report(out)

with working_result(out):
    try:
        validate()
    except Exception as error:
        write_status(out,'validate',error)
        raise
    finally:sanitize(out)
