#!/usr/bin/env python3
"""Verify definition targets using the actual clangd and Pyright language servers."""
import json
from pathlib import Path
from lsp_client import Client, ROOT

CPP=[
('axi_StorageStacked/storage_axi/aou_backend.cc','', 'StorageConfig','axi_StorageStacked/storage_axi/storage_config.hh'),
('vortex_StorageStacked/integration/storage.cc','std::make_unique<AouBackend>', 'AouBackend','axi_StorageStacked/storage_axi/aou_backend.hh'),
('vortex_StorageStacked/third_party/vortex/sim/simx/gem5/vortex_gpgpu.cpp','std::make_unique<Processor>()','Processor','vortex_StorageStacked/third_party/vortex/sim/simx/processor.h'),
('vortex_StorageStacked/third_party/vortex/sim/simx/gem5/vortex_gpgpu_dev.hh','class VortexGPGPU : public DmaDevice','DmaDevice','vortex_StorageStacked/third_party/gem5/src/dev/dma_device.hh'),
('coralnpu_StorageStacked/integration/config.hh','', 'StorageConfig','axi_StorageStacked/storage_axi/storage_config.hh'),
('coralnpu_StorageStacked/coralnpu/native/coralnpu_native.cc','CoreMiniAxiWrapper wrapper','CoreMiniAxiWrapper','coralnpu_StorageStacked/coralnpu/hw_sim/core_mini_axi_wrapper.h'),
]
PYTHON=[
('vortex_StorageStacked/third_party/gem5/runtime/soc.py','class L1(Cache)','Cache','vortex_StorageStacked/third_party/gem5/src/mem/cache/Cache.py'),
('vortex_StorageStacked/third_party/gem5/runtime/soc.py','system=System()','System','vortex_StorageStacked/third_party/gem5/src/sim/System.py'),
('vortex_StorageStacked/third_party/gem5/runtime/soc.py','TimingSimpleCPU(cpu_id=i)','TimingSimpleCPU','vortex_StorageStacked/third_party/gem5/src/arch/x86/X86CPU.py'),
('vortex_StorageStacked/third_party/gem5/runtime/soc.py','host_range = AddrRange','AddrRange','vortex_StorageStacked/third_party/gem5/src/python/m5/params/param_types.py'),
('vortex_StorageStacked/third_party/gem5/runtime/soc.py','system.vortex = VortexGPGPU(','VortexGPGPU','vortex_StorageStacked/third_party/vortex/sim/simx/gem5/VortexGPGPU.py'),
('vortex_StorageStacked/third_party/gem5/runtime/test.py','from paths import ROOT','ROOT','vortex_StorageStacked/third_party/gem5/runtime/paths.py'),
('coralnpu_StorageStacked/coralnpu/runtime/test.py','from paths import ROOT','ROOT','coralnpu_StorageStacked/coralnpu/runtime/paths.py'),
]

def main():
    settings=json.loads((ROOT/'.vscode/settings.json').read_text())
    analysis={'extraPaths':[p.replace('${workspaceFolder}',str(ROOT)) for p in settings['python.analysis.extraPaths']], 'stubPath':str(ROOT/'.vscode/gem5-python-stubs'), 'diagnosticMode':'openFilesOnly','typeCheckingMode':'off','exclude':settings['python.analysis.exclude']}
    results=[]
    for command,probes,language,config in [([settings['clangd.path'],'--background-index=false','--log=error','-j=2'],CPP,'cpp',None),([str(ROOT/'.vscode/deps/node_modules/.bin/pyright-langserver'),'--stdio'],PYTHON,'python',analysis)]:
        client=Client(command,config,'/usr/bin/python3')
        try:
            for relative,needle,token,expected in probes:
                try:
                    # Empty needles select the first line containing the symbol.
                    actual=client.definition(relative,needle or token,token,language)
                    passed=str((ROOT/expected).resolve()) in actual
                    item={'source':relative,'symbol':token,'expected':expected,'actual':actual,'passed':passed}
                except Exception as error:item={'source':relative,'symbol':token,'passed':False,'error':str(error)}
                results.append(item);print(('PASS' if item['passed'] else 'FAIL')+' '+relative+': '+token,flush=True)
        finally:client.close()
    (ROOT/'.vscode/navigation_verification.json').write_text(json.dumps(results,indent=2)+'\n')
    if not all(r['passed'] for r in results):raise SystemExit(1)

if __name__=='__main__':main()
