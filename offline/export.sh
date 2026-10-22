#!/usr/bin/env bash
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
mode=${1:-all}
case "$mode" in all|--save-only|--verify-only) ;; *) echo '用法：export.sh [--save-only|--verify-only]' >&2; exit 2 ;; esac
image=${STORAGE_IMAGE:-zhongxing-storagestacked:offline-20261009-burst}
destination=${STORAGE_EXPORT_DIR:-$here/dist/zhongxing-storagestacked-offline-20261009-burst}
validation=${STORAGE_VALIDATION_DIR:-$here/validation/offline-20261009-burst}
mkdir -p "$destination"
mkdir -p "$validation"
docker image inspect "$image" >/dev/null
archive=$destination/zhongxing-storagestacked-offline.tar.gz
if [[ $mode == --verify-only ]]; then
    [[ -f $archive && -f $destination/SOURCE_VERSIONS.json && -f $destination/run.sh ]] || { echo '缺少已导出的归档或交付入口。' >&2; exit 1; }
else
if [[ -e $archive ]]; then echo "交付包已存在：$archive；请先移到其他位置。" >&2; exit 1; fi
cp "$here/run.sh" "$destination/run.sh"
cp "$here/docker-compat-entrypoint.py" "$destination/docker-compat-entrypoint.py"
cp "$here/run-seccomp-check.sh" "$destination/run-seccomp-check.sh"
cp "$here/DOCKER_COMPATIBILITY.md" "$destination/DOCKER_COMPATIBILITY.md"
cp "$here/README.md" "$destination/使用说明.md"
docker run --rm --pull=never --network=none --entrypoint /bin/bash "$image" \
    -c 'cat "$STORAGE_WORKSPACE/SOURCE_VERSIONS.json"' > "$destination/SOURCE_VERSIONS.json"
if [[ -x /usr/bin/pigz ]] && /usr/bin/pigz -V >/dev/null 2>&1; then
    docker save "$image" | /usr/bin/pigz -p 8 -1 > "$archive.part"
else
    docker save "$image" | gzip -1 > "$archive.part"
fi
mv "$archive.part" "$archive"
fi
if [[ $mode == --save-only ]]; then
    printf '归档保存完成，尚未进行导入验收：%s\n' "$archive"
    exit 0
fi
# Test first-use auto-import: retain the generated image under a backup tag,
# remove only its delivery tag, and let the customer launcher restore it.
backup_image="${image}-export-backup"
docker tag "$image" "$backup_image"
docker image rm "$image" > "$validation/untag-delivery.log" 2>&1
if docker image inspect "$image" >/dev/null 2>&1; then
    echo '交付镜像标签仍存在，无法验证首次自动导入。' >&2; exit 1
fi
customer_results="$validation/customer-results/$(date +%Y%m%d-%H%M%S)-$$"
STORAGE_IMAGE="$image" STORAGE_RESULTS_DIR="$customer_results" bash "$destination/run.sh" all > "$validation/delivery-all.log" 2>&1
sed -n '/^Loaded image:/p' "$validation/delivery-all.log" > "$validation/load.log"
test -s "$validation/load.log"
docker image rm "$backup_image" > "$validation/remove-export-backup.log" 2>&1
python3 - "$here" "$destination" "$image" "$validation" "$customer_results" <<'PY'
import datetime,json,subprocess,sys
from pathlib import Path
here,dest=map(Path,sys.argv[1:3]);image=sys.argv[3]
validation=Path(sys.argv[4]);customer_results=Path(sys.argv[5])
summaries=list(customer_results.glob('*/summary.json'))
assert len(summaries)==1
acceptance=json.loads(summaries[0].read_text())
assert acceptance['passed'] is True and len(acceptance['cases'])==5
versions=json.loads((dest/'SOURCE_VERSIONS.json').read_text())
assert acceptance['source_versions']==versions
# A package is accepted only after the matching full regression has passed.
maker=json.loads((validation/'acceptance.json').read_text())
assert maker['passed'] is True and len(maker['runs'])==1
test_path=here/maker['runs'][0]['path']
regression=json.loads(test_path.read_text())
assert regression['passed'] is True and regression['command']=='test'
assert regression['source_versions']==versions
embedded={'axi':json.loads((test_path.parent/regression['cases']['axi']['summary']).read_text())}
execution={}
for device in ('vortex','coralnpu'):
    paths=regression['cases'][device]['summaries']
    assert len(paths)==1
    embedded[device]=json.loads((test_path.parent/paths[0]).read_text())
    assert embedded[device]['passed'] is True
    assert embedded[device]['regression_runs']==1
    assert set(embedded[device]['cases'])=={'smoke','llm'}
    assert embedded[device]['cases']['smoke']['output']==42
    assert embedded[device]['cases']['llm']['output']=='blu'
    llm_paths=list((test_path.parent/device/'user/llm/result').glob('*/llm/llm_summary.json'))
    assert len(llm_paths)==1
    llm=json.loads(llm_paths[0].read_text())
    assert llm['passed'] is True
    execution[device]=llm['execution']
assert execution['coralnpu']['core']=='RvvCoreMiniAxi'
assert execution['coralnpu']['linear']=='RVV FP32'
assert execution['coralnpu']['rvv_linear_calls']==42
assert embedded['coralnpu']['burst_tests']['passed'] is True
assert set(embedded['coralnpu']['burst_tests']['cases'])=={'1','2','4','8','16'}
for case in embedded['coralnpu']['burst_tests']['cases'].values():
    assert case['passed'] is True
coral_llm_case=list((test_path.parent/'coralnpu/user/llm/result').glob('*/llm/summary.json'))
assert len(coral_llm_case)==1
coral_run=json.loads(coral_llm_case[0].read_text())
coral_burst=coral_run['axi_burst']
assert coral_burst['beats_per_request']==4
assert coral_burst['write_beats']==4*coral_burst['write_bursts']
assert coral_burst['read_beats']==4*coral_burst['read_bursts']
assert coral_burst['write_bursts']+coral_burst['read_bursts']==coral_run['sources']['coralnpu']['transactions']
customer_coral=json.loads((summaries[0].parent/acceptance['cases']['coralnpu/llm']['summary']).read_text())
assert customer_coral['passed'] is True
assert customer_coral['axi_burst']==coral_burst
assert execution['vortex']['threads']==execution['vortex']['active_threads']==8
assert execution['vortex']['warps']==2 and execution['vortex']['linear_calls']==42
verification={'passed':True,'image':image,'platform':'linux/amd64',
              'network':'none','pull':'never','source_versions':versions,
              'regressions':embedded,'execution':execution,'coralnpu_axi_burst':coral_burst,'maker_evidence':str(test_path),
              'customer_first_use':{'automatic_import':True,
                  'image_tag_absent_before_launch':True,'cases':acceptance['cases'],
                  'maker_evidence':str(summaries[0])}}
(dest/'VALIDATION.json').write_text(json.dumps(verification,ensure_ascii=False,indent=2)+'\n')
(dest/'ACCEPTANCE.json').write_text(json.dumps(acceptance,ensure_ascii=False,indent=2)+'\n')
report={'passed':True,'created_utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),
        'image':image,'platform':'linux/amd64','network':'none','pull':'never',
        'source_versions':versions,
        'image_bytes':json.loads(subprocess.check_output(['docker','image','inspect',image],text=True))[0]['Size'],
        'files':[{'name':p.name,'bytes':p.stat().st_size} for p in sorted(dest.iterdir()) if p.is_file() and p.name!='DELIVERY.json'],
        'actual_import_result':(validation/'load.log').read_text().strip(),
        'acceptance_summary':'ACCEPTANCE.json',
        'maker_evidence':str(summaries[0]),
        'cases':acceptance['cases']}
(dest/'DELIVERY.json').write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n')
(validation/'delivery.json').write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n')
print(dest)
print('导出、导入、交付入口断网验收通过。')
PY
