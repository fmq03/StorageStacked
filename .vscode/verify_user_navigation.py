#!/usr/bin/env python3
"""Check every first-party user/integration translation unit and actual LSP definitions."""
import concurrent.futures
import json
import re
import subprocess
import time
from pathlib import Path
from lsp_client import Client, ROOT

PLATFORMS=['vortex_StorageStacked','coralnpu_StorageStacked']
SCOPES=[ROOT/p/d for p in PLATFORMS for d in ('user','integration')]
CPP=sorted(p for scope in SCOPES for p in scope.rglob('*') if p.suffix in {'.cpp','.cc','.h','.hh','.hpp'} and 'result' not in p.parts)
SETTINGS=json.loads((ROOT/'.vscode/settings.json').read_text())
CLANGD=SETTINGS['clangd.path']
RESULT={'scope':[str(p.relative_to(ROOT)) for p in SCOPES], 'compiler':[], 'native_compiler':[], 'cpp_definitions':[], 'probes':[], 'files':{}}

def rel(p):return str(Path(p).relative_to(ROOT))
def compiler_check(source):
    run=subprocess.run([CLANGD,'--check='+str(source),'--log=error'],text=True,capture_output=True)
    errors=[line for line in run.stderr.splitlines() if re.search(r'\bE\[',line) and 'IncludeCleaner' not in line and 'tweak:' not in line]
    return {'source':rel(source),'passed':not errors,'errors':errors,'exit_code':run.returncode}

def probe(client,source,needle,token,expected,language,workspace):
    actual=client.definition(source,needle,token,language)
    if language=='shellscript':
        # Bash LS analyzes didOpen asynchronously; the first request can race
        # initialization. A script containing only source/exec has no own
        # document symbols, so symbol count cannot serve as a ready signal.
        deadline=time.monotonic()+30
        while not actual and time.monotonic()<deadline:
            time.sleep(0.1)
            actual=client.definition(source,needle,token,language)
    expected=str((ROOT/expected).resolve())
    item={'source':source,'symbol':token,'workspace':rel(workspace) if workspace!=ROOT else '.', 'expected':expected,'actual':actual,'passed':expected in actual}
    RESULT['probes'].append(item)
    print(('PASS' if item['passed'] else 'FAIL')+' '+source+': '+token,flush=True)

def tokens(client,source):
    client.open(source,'cpp')
    data=client.request('textDocument/semanticTokens/full',{'textDocument':{'uri':source.as_uri()}})['data']
    legend=client.capabilities['semanticTokensProvider']['legend']
    lines=source.read_text().splitlines();row=column=0
    for offset in range(0,len(data),5):
        dr,dc,length,kind,mods=data[offset:offset+5]
        row+=dr;column=column+dc if dr==0 else dc
        modifiers={name for bit,name in enumerate(legend['tokenModifiers']) if mods&(1<<bit)}
        yield row,column,lines[row][column:column+length],legend['tokenTypes'][kind],modifiers

def cpp():
    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
        for result in pool.map(compiler_check,CPP):
            RESULT['compiler'].append(result)
            print(('PARSE PASS ' if result['passed'] else 'PARSE FAIL ')+result['source'],flush=True)
    native=ROOT/'coralnpu_StorageStacked/coralnpu/native/coralnpu_native.cc'
    result=compiler_check(native);RESULT['native_compiler'].append(result)
    print(('PARSE PASS ' if result['passed'] else 'PARSE FAIL ')+result['source'],flush=True)
    client=Client([CLANGD,'--background-index=false','--log=error','-j=4'])
    try:
        all_tokens={p:list(tokens(client,p)) for p in CPP}
        names={platform:set() for platform in PLATFORMS}
        for platform in PLATFORMS:
            short=platform.removesuffix('_StorageStacked')
            for header in (ROOT/'.vscode/projects'/short).glob('*/sources/project_config.h'):
                names[platform].update(re.findall(r'^\s*#define\s+(\w+)',header.read_text(),re.M))
        for source,items in all_tokens.items():
            platform=source.relative_to(ROOT).parts[0]
            names[platform].update(t for _,_,t,k,m in items if {'declaration','definition'}&m)
        for source,items in all_tokens.items():
            platform=source.relative_to(ROOT).parts[0];count=references=0
            seen=set()
            for row,col,name,kind,mods in items:
                if name not in names[platform] or kind in {'comment','string','number','operator','keyword'}:continue
                if (row,col) in seen:continue
                seen.add((row,col))
                actual=client.definition_at(source,row,col)
                # All project-defined references must resolve to existing files;
                # identical names in the other platform must never steal them.
                wrong=[p for p in actual if any(str(ROOT/other/d)+'/' in p for other in PLATFORMS if other!=platform for d in ('user','integration'))]
                snapshots=[p for p in actual if '/.vscode/projects/' in p and not p.endswith('/project_config.h')]
                config_context=True
                if 'user' in source.relative_to(ROOT).parts:
                    project=source.relative_to(ROOT/platform/'user').parts[0]
                    expected_config=ROOT/'.vscode/projects'/platform.removesuffix('_StorageStacked')/project/'sources/project_config.h'
                    config_context=all(Path(p)==expected_config for p in actual if p.endswith('/project_config.h'))
                    wrong += [p for p in actual if str(ROOT/platform/'user')+'/' in p and ROOT/platform/'user'/project not in Path(p).parents]
                passed=bool(actual) and all(Path(p).is_file() for p in actual) and not wrong and not snapshots and config_context
                is_reference=not bool({'declaration','definition'}&mods)
                RESULT['cpp_definitions'].append({'source':rel(source),'line':row+1,'symbol':name,'kind':kind,'reference':is_reference,'actual':actual,'passed':passed})
                count+=1;references+=int(is_reference)
            RESULT['files'][rel(source)]={'definitions':count,'references':references}
            print('LSP '+rel(source)+f': {count} definitions, {references} references',flush=True)
        checks=[
            ('vortex_StorageStacked/user/smoke/src/host.cpp','SMOKE_INPUT','SMOKE_INPUT','.vscode/projects/vortex/smoke/sources/project_config.h'),
            ('vortex_StorageStacked/user/smoke/src/kernel.cpp','#include \"project_config.h\"','project_config.h','.vscode/projects/vortex/smoke/sources/project_config.h'),
            ('vortex_StorageStacked/user/llm/src/kernel.cpp','linear(s, n1','linear','vortex_StorageStacked/user/llm/src/kernel.cpp'),
        ]
        # Intrinsics are generated by clang's pragma and have no source definition.
        # Their semantic correctness is checked by the full frontend above.
        for source,needle,token,expected in checks:probe(client,source,needle,token,expected,'cpp',ROOT)
        probe(client,rel(native),'CoreMiniAxiWrapper wrapper','CoreMiniAxiWrapper',
              'coralnpu_StorageStacked/coralnpu/hw_sim/core_mini_axi_wrapper.h','cpp',ROOT)
        for source in CPP:
            for include in re.findall(r'^\s*#include\s+"([^"]+)"',source.read_text(),re.M):
                expected=source.parent/include
                if expected.is_file():
                    probe(client,rel(source),'#include "'+include+'"',include,rel(expected),'cpp',ROOT)
    finally:client.close()
    for platform in PLATFORMS:
        workspace=ROOT/platform
        local_clangd=json.loads((workspace/'.vscode/settings.json').read_text())['clangd.path']
        client=Client([local_clangd,'--background-index=false','--log=error','-j=2'],workspace=workspace)
        try:
            if platform.startswith('vortex'):
                probe(client,platform+'/user/smoke/src/host.cpp','SMOKE_INPUT','SMOKE_INPUT','.vscode/projects/vortex/smoke/sources/project_config.h','cpp',workspace)
                probe(client,platform+'/integration/storage.cc','std::make_unique<AouBackend>','AouBackend','axi_StorageStacked/storage_axi/aou_backend.hh','cpp',workspace)
            else:
                probe(client,platform+'/integration/main.cc','storage_axi::ConfigParams p','ConfigParams',platform+'/integration/config.hh','cpp',workspace)
                probe(client,platform+'/user/llm/src/tinyllm.cpp','#include "tinyllm.h"','tinyllm.h',platform+'/user/llm/src/tinyllm.h','cpp',workspace)
        finally:client.close()

def python_checks():
    probes=[
        ('vortex_StorageStacked/integration/system.py','from soc import create','create','vortex_StorageStacked/third_party/gem5/runtime/soc.py'),
        ('vortex_StorageStacked/integration/system.py','system.axi=StorageBridge(','StorageBridge','vortex_StorageStacked/integration/StorageBridge.py'),
        ('vortex_StorageStacked/integration/StorageBridge.py','class StorageBridge(SystemC_ScModule)','SystemC_ScModule','vortex_StorageStacked/third_party/gem5/src/systemc/core/SystemC.py'),
        ('vortex_StorageStacked/integration/StorageBridge.py','TlmTargetSocket(64','TlmTargetSocket','vortex_StorageStacked/third_party/gem5/src/systemc/Tlm.py'),
        ('vortex_StorageStacked/integration/SConscript','os.path.join(storage,','storage','vortex_StorageStacked/integration/SConscript'),
    ]
    for workspace in (ROOT,ROOT/PLATFORMS[0]):
        settings=json.loads((workspace/'.vscode/settings.json').read_text())
        analysis={key.removeprefix('python.analysis.'):value for key,value in settings.items() if key.startswith('python.analysis.')}
        analysis['extraPaths']=[str((workspace/p.replace('${workspaceFolder}/','')).resolve()) for p in settings['python.analysis.extraPaths']]
        analysis['stubPath']=str((workspace/settings['python.analysis.stubPath']).resolve())
        client=Client([str(ROOT/'.vscode/deps/node_modules/.bin/pyright-langserver'),'--stdio'],analysis,'/usr/bin/python3',workspace=workspace)
        try:
            for source,needle,token,expected in probes:probe(client,source,needle,token,expected,'python',workspace)
        finally:client.close()
    npu_probes=[
        ('session.py','from result_layout import organize, write_index','organize','result_layout.py'),
        ('session.py','from rtl_build import native_library','native_library','rtl_build.py'),
        ('rtl_build.py','from configure import rtl_config, memory_layout','rtl_config','configure.py'),
        ('platform_build.py','from rtl_build import build_native, variant_name','build_native','rtl_build.py'),
        ('organize_results.py','from result_layout import organize, write_index','write_index','result_layout.py'),
        ('validate.py','from result_layout import working_result','working_result','result_layout.py'),
        ('test_rtl.py','from session import finish','finish','session.py'),
    ]
    for workspace in (ROOT,ROOT/'coralnpu_StorageStacked'):
        settings=json.loads((workspace/'.vscode/settings.json').read_text())
        analysis={key.removeprefix('python.analysis.'):value for key,value in settings.items() if key.startswith('python.analysis.')}
        analysis['extraPaths']=[str((workspace/p.replace('${workspaceFolder}/','')).resolve()) for p in settings['python.analysis.extraPaths']]
        analysis['stubPath']=str((workspace/settings['python.analysis.stubPath']).resolve())
        client=Client([str(ROOT/'.vscode/deps/node_modules/.bin/pyright-langserver'),'--stdio'],analysis,'/usr/bin/python3',workspace=workspace)
        try:
            for source,needle,token,expected in npu_probes:
                prefix='coralnpu_StorageStacked/coralnpu/runtime/'
                probe(client,prefix+source,needle,token,prefix+expected,'python',workspace)
        finally:client.close()

def shell_checks():
    extensions=Path.home()/'.vscode-server/extensions'
    server=next(extensions.glob('mads-hartmann.bash-ide-vscode-*/node_modules/bash-language-server/out/cli.js'))
    for workspace in (ROOT,*(ROOT/p for p in PLATFORMS)):
        settings=json.loads((workspace/'.vscode/settings.json').read_text())
        config={key.removeprefix('bashIde.'):value for key,value in settings.items() if key.startswith('bashIde.')}
        client=Client(['node',str(server),'start'],workspace=workspace,configuration={'bashIde':config})
        try:
            for platform in PLATFORMS:
                if workspace!=ROOT and workspace.name!=platform:continue
                source=platform+'/user/run.sh'
                for needle,token in [('|| { usage;','usage'),('|| die','die'),('trap finish EXIT','finish'),('stage=simulate','stage')]:probe(client,source,needle,token,source,'shellscript',workspace)
                runtime=platform+('/third_party/gem5/runtime/environment.sh' if platform.startswith('vortex') else '/coralnpu/runtime/environment.sh')
                probe(client,source,'exec "$AXI_PYTHON"','AXI_PYTHON',runtime,'shellscript',workspace)
                if platform.startswith('coralnpu'):
                    probe(client,platform+'/user/rebuild-rtl.sh','exec "$AXI_PYTHON"','AXI_PYTHON',runtime,'shellscript',workspace)
        finally:client.close()

def main():
    try:
        cpp();python_checks();shell_checks()
    except Exception as error:
        RESULT['error']=str(error)
        raise
    finally:
        results=RESULT['compiler']+RESULT['native_compiler']+RESULT['cpp_definitions']+RESULT['probes']
        RESULT['summary']={'cpp_files':len(CPP),'native_cpp_files':len(RESULT['native_compiler']),'compiler_errors':sum(not x['passed'] for x in RESULT['compiler']+RESULT['native_compiler']), 'definition_requests':len(RESULT['cpp_definitions'])+len(RESULT['probes']), 'reference_requests':sum(x['reference'] for x in RESULT['cpp_definitions']), 'failures':sum(not x['passed'] for x in results), 'passed':all(x['passed'] for x in results) and 'error' not in RESULT}
        (ROOT/'.vscode/user_navigation_verification.json').write_text(json.dumps(RESULT,indent=2)+'\n')
    print(json.dumps(RESULT['summary'],ensure_ascii=False))
    if not RESULT['summary']['passed']:raise SystemExit(1)
if __name__=='__main__':main()
