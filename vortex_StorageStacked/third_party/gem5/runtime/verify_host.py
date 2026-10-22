"""Check executable roles, real CPU activity and application work partitions."""
import csv,json,math,re,struct
from llm_model import layout
from paths import dump


def check_execution(evidence,config):
    count=config['architecture']['host']['num_cpus']
    cores=evidence['cores']
    assert len(cores)==count and [core['cpu'] for core in cores]==list(range(count))
    assert evidence['active_cpus']==sum(core['instructions']>0 for core in cores)
    kind=config['benchmark']['name']
    if kind not in ('smoke','tiny_llm'):return
    assert evidence['active_cpus']==count,'A configured CPU did not execute instructions'
    assert all(core['l1i_accesses']>0 and core['l1d_accesses']>0 and core['translation_samples']>0 for core in cores),'Missing per-core cache/MMU activity'
    total=config['benchmark']['workers'] if kind=='smoke' else len(layout(config['llm_model'])[0])
    assert evidence['host_check_errors']==0,'Host-side output check failed or missing'
    assert set(evidence['phases'])=={'prepare','check'}
    for phase,workers in evidence['phases'].items():
        assert len(workers)==count and [w['worker'] for w in workers]==list(range(count)),phase+': missing/duplicate worker'
        assert len({w['tid'] for w in workers})==count and all(w['tid']>0 for w in workers),phase+': missing/duplicate thread'
        for i,worker in enumerate(workers):
            assert (worker['begin'],worker['end'])==(total*i//count,total*(i+1)//count),phase+': incomplete work partition'


def verify(root,config):
    # Read the actual ELF headers, rather than trusting output filenames.
    isa=config['architecture']['host'].get('isa','x86_64')
    for name,elf_class,machine in [('host.elf',2,{'x86_64':62,'aarch64':183}[isa]),('program.elf',1,243)]:
        with (root/'build'/name).open('rb') as stream:header=stream.read(20)
        assert header[:4]==b'\x7fELF' and header[4:6]==bytes([elf_class,1])
        assert struct.unpack_from('<H',header,18)[0]==machine,'Wrong executable ISA: '+name
    programs=json.loads((root/'build/programs.json').read_text())
    assert programs['host']['binary']=='host.elf' and programs['device']['binary']=='program.elf'
    assert programs['host']['isa']==('x86-64' if isa=='x86_64' else 'aarch64')
    for role in ('host','device'):
        assert programs[role]['sources']
        for name in programs[role]['sources']:
            assert not name.startswith('/') and '..' not in name.split('/')
            assert (root/'build/sources'/name).is_file(),'Missing compiled source snapshot'
    stats={}
    for line in (root/'stats.txt').read_text().splitlines():
        fields=line.split()
        if len(fields)>1:
            try:
                value=float(fields[1])
                if math.isfinite(value):stats[fields[0]]=value
            except ValueError:pass
    with (root/'mmu_translations.csv').open() as stream:translations=list(csv.DictReader(stream))
    cores=[]
    for i in range(config['architecture']['host']['num_cpus']):
        prefix='systemc_kernel.system.cpu'+str(i)
        cores.append({'cpu':i,'instructions':int(stats.get(prefix+'.thread_0.numInsts',0)),
                      'cycles':int(stats.get(prefix+'.numCycles',0)),
                      'l1i_accesses':int(stats.get(prefix+'.icache.demandAccesses::total',0)),
                      'l1d_accesses':int(stats.get(prefix+'.dcache.demandAccesses::total',0)),
                      'translation_samples':sum(r['tlb'].startswith(prefix+'.mmu.') for r in translations)})
    log=(root/'run.log').read_text();phases={}
    for phase,worker,tid,begin,end in re.findall(r'^HOST_TASK phase=(\w+) worker=(\d+) tid=(\d+) begin=(\d+) end=(\d+)$',log,re.M):
        phases.setdefault(phase,[]).append(dict(zip(('worker','tid','begin','end'),map(int,(worker,tid,begin,end)))))
    checks=re.findall(r'^HOST_CHECK_ERRORS (\d+)$',log,re.M)
    result={'programs':programs,'configured_cpus':len(cores),'active_cpus':sum(c['instructions']>0 for c in cores),
            'cores':cores,'phases':phases,'host_check_errors':int(checks[0]) if len(checks)==1 else None,
            'scope':'Per-core gem5 counters plus barrier-synchronized application thread partitions; counts are not a speedup measurement'}
    check_execution(result,config);result['passed']=True
    dump(root/'host_summary.json',result);return result
