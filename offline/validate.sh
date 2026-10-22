#!/usr/bin/env bash
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
mkdir -p "$here/validation"
validation=${STORAGE_VALIDATION_DIR:-$here/validation/offline-20261022}
mkdir -p "$validation"
run_results="$here/results/check-$(date +%Y%m%d-%H%M%S)-$$"
export STORAGE_RESULTS_DIR="$run_results"
"$here/run.sh" check > "$validation/check.log" 2>&1
mkdir -p "$run_results"
arm_config="$run_results/arm-cache-smoke.json"
python3 - "$here/../vortex_StorageStacked/user/smoke/config.json" "$arm_config" <<'PY'
import json,sys
from pathlib import Path
c=json.loads(Path(sys.argv[1]).read_text())
c['host']['isa']='aarch64'
c['gpu'].update(l1i_ways=2,l1d_ways=2,l1i_latency_cycles=3,l1d_latency_cycles=3,
                l2_enabled=True,l2_ways=8,l2_latency_cycles=6)
Path(sys.argv[2]).write_text(json.dumps(c,indent=2)+'\n')
PY
arm_pid=
stop_arm() {
    if [[ -n $arm_pid ]]; then
        kill -TERM "$arm_pid" 2>/dev/null || :
        wait "$arm_pid" 2>/dev/null || :
    fi
}
trap stop_arm EXIT
STORAGE_RESULTS_DIR="$run_results/arm-cache" "$here/run.sh" vortex smoke --config "$arm_config" > "$validation/arm-cache.log" 2>&1 &
arm_pid=$!
test_code=0
"$here/run.sh" test > "$validation/test.log" 2>&1 || test_code=$?
arm_code=0
wait "$arm_pid" || arm_code=$?
arm_pid=
[[ $test_code == 0 && $arm_code == 0 ]] || {
    echo "镜像验收失败：完整回归=$test_code，ARM/GPU 缓存 SMOKE=$arm_code；请查看 $validation。" >&2
    exit 1
}
python3 - "$here" "$run_results" "$validation" <<'PY'
import json,re,sys
from pathlib import Path
here=Path(sys.argv[1]); result_root=Path(sys.argv[2]); validation=Path(sys.argv[3]); summaries=[]
for p in result_root.glob('*/summary.json'):
    s=json.loads(p.read_text())
    if s['command'] == 'test':
        assert s['passed'] is True,p
        summaries.append({'path':str(p.relative_to(here)),'command':s['command'],'cases':s['cases']})
assert len(summaries)==1
arm_paths=list((result_root/'arm-cache').glob('*/summary.json'));assert len(arm_paths)==1
arm=json.loads(arm_paths[0].read_text());assert arm['passed'] is True
arm_case=arm_paths[0].parent/arm['cases']['vortex/smoke']['summary']
device=json.loads(arm_case.read_text());assert device['passed'] and device['smoke']['output']==42
config=json.loads((arm_case.parent/'input.json').read_text());assert config['host']['isa']=='aarch64'
assert {k:config['gpu'][k] for k in ('l1i_ways','l1d_ways','l1i_latency_cycles','l1d_latency_cycles','l2_enabled','l2_ways','l2_latency_cycles')}==dict(l1i_ways=2,l1d_ways=2,l1i_latency_cycles=3,l1d_latency_cycles=3,l2_enabled=True,l2_ways=8,l2_latency_cycles=6)
full=json.loads((here/summaries[0]['path']).read_text());assert arm['source_versions']==full['source_versions']
elf=(arm_case.parent/'build/host.elf').read_bytes()
assert elf[:4]==b'\x7fELF' and elf[5]==1 and int.from_bytes(elf[18:20],'little')==183
assert device['host_execution']['active_cpus']==config['host']['num_cpus']
prefetches=sum(map(int,re.findall(r'\.mmu\.prefetchFaults\s+(\d+)',(arm_case.parent/'stats.txt').read_text())))
assert prefetches>0,'ARM fixture did not exercise device prefetch rejection'
variant={'passed':True,'path':str(arm_paths[0].relative_to(here)),
         'device_summary':str(arm_case.relative_to(here)),
         'host_elf_machine':183,'configuration':config,
         'uncacheable_prefetches_rejected':prefetches,
         'output':42,'host_execution':device['host_execution']}
(validation/'acceptance.json').write_text(json.dumps({'passed':True,'network':'none','pull':'never','host_mounts':['results (read-write)','compatibility helper (read-only)','ARM configuration (read-only)'],'runs':summaries,'benchmark_variants':{'arm_gpu_cache_smoke':variant}},ensure_ascii=False,indent=2)+'\n')
print('PASS：工具检查、完整回归及 ARM/GPU 缓存 SMOKE；导出脚本另行验证归档首次导入与默认入口。')
PY
