#!/usr/bin/env python3
"""Refresh editor-only commands from existing native build metadata."""
import argparse
from collections import Counter
import json
import os
from pathlib import Path
import shlex
import subprocess

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / '.vscode'
SUFFIXES = {'.c', '.cc', '.cpp', '.cxx'}
HEADERS = {'.h', '.hh', '.hpp'}
EXCLUDED = {'.git', '.cache', 'build', 'result', 'results', 'bazel-out', 'bazel-bin', 'bazel-coralnpu', 'bazel-testlogs', '__pycache__'}
VORTEX = ROOT / 'vortex_StorageStacked'
NPU = ROOT / 'coralnpu_StorageStacked'
GEM5 = VORTEX / 'third_party/gem5'


def run(command, cwd=ROOT):
    return subprocess.check_output([str(x) for x in command], cwd=cwd, text=True, stderr=subprocess.PIPE)


def sources(base):
    for directory, dirs, files in os.walk(base):
        dirs[:] = [d for d in dirs if d not in EXCLUDED and not (Path(directory)/d).is_symlink()]
        for name in files:
            path = Path(directory)/name
            if path.suffix in SUFFIXES:
                yield path.resolve()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--gem5', action='store_true', help='Regenerate gem5 compilation metadata with SCons; run after build, never concurrently')
    args = parser.parse_args()
    if args.gem5:
        subprocess.run(['bash', str(OUT/'refresh_gem5_commands.sh')], cwd=ROOT, check=True)
    entries = {}; origin = {}; warnings = []

    def add(entry, label):
        directory = Path(entry['directory']).resolve()
        source = (directory/entry['file']).resolve()
        if not source.is_file() or ROOT not in source.parents or source.suffix not in SUFFIXES | HEADERS:
            return
        if any(part in EXCLUDED for part in source.relative_to(ROOT).parts):
            return
        if source in entries:
            return
        words = entry.get('arguments') or shlex.split(entry['command'])
        words = list(words)
        if words and Path(words[0]).name == 'ccache':
            words.pop(0)
        # clangd uses the host system compiler for standard headers. Keep actual
        # include paths, language, target and macros from the native command.
        words[0] = '/usr/bin/clang' if source.suffix == '.c' else '/usr/bin/clang++'
        # This GCC-only option changes path spelling, not target semantics.
        words = [word for word in words if word != '-fno-canonical-system-headers']
        if label == 'bazel-coralnpu':
            # native_build.py supplies this through CPLUS_INCLUDE_PATH, which
            # Bazel's action argument list alone does not retain.
            words += ['-isystem', str(NPU/'coralnpu/.cache/tools/xpu-native/include')]
            # A later SDK build prunes unused dependency links from execroot.
            # Keep the same dependencies via Bazel's persistent external tree.
            external = directory.parents[1]/'external'
            words = [str(external/word.removeprefix('external/')) if word.startswith('external/')
                     else '-I'+str(external/word.removeprefix('-Iexternal/')) if word.startswith('-Iexternal/')
                     else word for word in words]
        if source not in entries:
            entries[source] = {'directory':str(directory), 'file':str(source), 'arguments':words}
            origin[source] = label

    for database in sorted((OUT/'projects').glob('*/commands.json')):
        for entry in json.loads(database.read_text()):
            add(entry, 'user-project-build')

    db = ROOT/'axi_StorageStacked/build/compile_commands.json'
    if db.exists():
        for entry in json.loads(db.read_text()):add(entry, 'cmake-axi')
    actions_file=OUT/'bazel_actions.json'
    if actions_file.exists():
        execroot=(NPU/'coralnpu/bazel-out').resolve().parent
        for action in json.loads(actions_file.read_text()).get('actions',[]):
            words=action.get('arguments',[])
            if '-c' not in words:continue
            source=words[words.index('-c')+1]
            add({'directory':str(execroot),'file':source,'arguments':words}, 'bazel-coralnpu')
    configuration=json.loads((NPU/'user/smoke/config.json').read_text())
    u=configuration['ucie']; n=configuration['npu']
    link='link-'+'-'.join(format(u[k],'.12g') for k in ('lanes','rate_gtps','bits_per_symbol','tat_ns'))
    variant=f"i{n['itcm_kib']}-d{n['dtcm_kib']}-l0{int(n.get('fetch_l0_enabled', False))}"
    builds = [NPU/'coralnpu/.cache/build'/(link+'-'+variant),NPU/'coralnpu/.cache/build/memsim']
    for build in builds:
        if (build/'build.ninja').exists():
            commands = json.loads(run([NPU/'coralnpu/.cache/tools/toolchain/bin/ninja','-C',build,'-t','compdb']))
            for entry in commands:add(entry, 'ninja-coralnpu')
    for isa in ['AXI','AXI_ARM']:
        db=GEM5/'build'/isa/'compile_commands.json'
        if db.exists():
            for entry in json.loads(db.read_text()):
                add(entry, 'scons-'+isa)
                installed=(Path(entry['directory'])/entry['file']).resolve()
                original=VORTEX/'third_party/vortex/sim/simx/gem5'/installed.name
                if installed.parent==GEM5/'src/dev/vortex' and original.is_file() and original.read_bytes()==installed.read_bytes():
                    words=entry.get('arguments') or shlex.split(entry['command'])
                    words=[str(original) if w==entry['file'] or w==str(installed) else w for w in words]
                    add({'directory':entry['directory'],'file':str(original),'arguments':words}, 'scons-identical-source')
        else:warnings.append('No gem5 '+isa+' database; run with --gem5 after building')
    dry=OUT/'make_commands.json'
    if dry.exists():
        for entry in json.loads(dry.read_text()):add(entry,'make-vortex')

    active_roots = [ROOT/'axi_StorageStacked/storage_axi', ROOT/'axi_StorageStacked/mem_sim',ROOT/'axi_StorageStacked/axi2flit/systemc',ROOT/'axi_StorageStacked/ucie-model',VORTEX/'integration',VORTEX/'user',VORTEX/'third_party/vortex',NPU/'integration',NPU/'user',NPU/'coralnpu/native',NPU/'coralnpu/hw_sim']
    all_sources = sorted(set(p for base in active_roots for p in sources(base)))
    real_entries = dict(entries)
    templates={}
    for template,entry in real_entries.items():
        for parent in template.parents:
            if parent==ROOT:break
            templates.setdefault(parent,(template,entry))
    fallback=[]
    for source in all_sources:
        if source in entries:continue
        # Optional files have no active build action. Borrow the closest command
        # within their own platform and explicitly mark the entry as inferred.
        group = NPU if NPU in source.parents else VORTEX if VORTEX in source.parents else ROOT/'axi_StorageStacked'
        best=None
        for parent in source.parents:
            if parent==ROOT:break
            if parent in templates:
                best=templates[parent];break
            if parent==group:break
        if source.parent == VORTEX/'third_party/vortex/sim/simx/gem5' and source.name == 'vortex_gpgpu_dev.cc':
            installed = GEM5/'src/dev/vortex/vortex_gpgpu_dev.cc'
            if installed in real_entries:
                best = (installed, real_entries[installed])
        if best:
            template,e=best;words=e['arguments'][:]
            words=[str(source) if w==str(template) or (not w.startswith('-') and (Path(e['directory'])/w).resolve()==template) else w for w in words]
            words += ['-I'+str(source.parent)]
            if source.suffix=='.c':
                words[0]='/usr/bin/clang';words=[w for w in words if not w.startswith('-std=c++')]
        else:
            words=['/usr/bin/clang' if source.suffix=='.c' else '/usr/bin/clang++','-std=c11' if source.suffix=='.c' else '-std=c++17','-I'+str(source.parent),'-c',str(source)]
        if NPU/'coralnpu/native' in source.parents or NPU/'coralnpu/hw_sim' in source.parents:
            words += ['-I'+str(NPU/'coralnpu/native'),'-I'+str(NPU/'coralnpu/hw_sim'),'-I/usr/share/verilator/include']
        entries[source]={'directory':str(ROOT),'file':str(source),'arguments':words} if not best else {'directory':e['directory'],'file':str(source),'arguments':words}
        origin[source]='inferred';fallback.append(str(source.relative_to(ROOT)))
    scoped_roots = [project/folder for project in (VORTEX,NPU) for folder in ('user','integration')]
    scoped_files=[]
    for base in scoped_roots:
        for directory,dirs,files in os.walk(base):
            dirs[:]=[d for d in dirs if d not in EXCLUDED]
            for name in files:
                file=(Path(directory)/name).resolve()
                if file.suffix not in SUFFIXES | HEADERS:continue
                scoped_files.append(file)
                if file.suffix not in HEADERS:continue
                owners=[file.with_suffix(suffix) for suffix in ('.cc','.cpp')]
                if VORTEX/'user' in file.parents:owners=[file.parent/'kernel.cpp',file.parent/'host.cpp',*owners]
                if NPU/'user' in file.parents:owners=[file.parent/'tinyllm.cpp',*owners]
                owners += sorted(p for p in entries if p.parent==file.parent and p.suffix in SUFFIXES)
                owner=next((p for p in owners if p in entries),None)
                if owner is None:raise RuntimeError('No header context: '+str(file))
                template=entries[owner]; words=template['arguments'][:]
                words=[str(file) if w==str(owner) or (not w.startswith('-') and (Path(template['directory'])/w).resolve()==owner) else w for w in words]
                words[1:1]=['-x','c++-header']
                entries[file]={'directory':template['directory'],'file':str(file),'arguments':words}
                origin[file]='explicit-header-context'
    scoped_inferred=[str(p.relative_to(ROOT)) for p in scoped_files if origin.get(p)=='inferred']
    if scoped_inferred:raise RuntimeError('user/integration require real commands: '+str(scoped_inferred))
    database=sorted(entries.values(),key=lambda e:e['file'])
    (OUT/'compile_commands.json').write_text(json.dumps(database,indent=2)+'\n')
    # Independent executables reuse qualified C++ names. Never let one
    # platform's background index merge definitions from the other platform.
    for label,platform in [('vortex',VORTEX),('coralnpu',NPU),('common',None)]:
        folder=OUT/'databases'/label;folder.mkdir(parents=True,exist_ok=True)
        selected=[e for e in database if (platform and platform in Path(e['file']).parents and platform/'user' not in Path(e['file']).parents)
                  or ROOT/'axi_StorageStacked' in Path(e['file']).parents]
        (folder/'compile_commands.json').write_text(json.dumps(selected,indent=2)+'\n')
    for platform in (VORTEX,NPU):
        for project in sorted((platform/'user').iterdir()):
            if not (project/'src/Makefile').is_file() or not (project/'config.json').is_file():continue
            label=platform.name.removesuffix('_StorageStacked')+'-'+project.name
            folder=OUT/'databases'/label;folder.mkdir(parents=True,exist_ok=True)
            selected=[e for e in database if project in Path(e['file']).parents]
            (folder/'compile_commands.json').write_text(json.dumps(selected,indent=2)+'\n')
            relative=os.path.relpath(folder,project)
            (project/'.clangd').write_text('# Compilation context for this independent executable.\nCompileFlags:\n  CompilationDatabase: '+relative+'\n')
    report={'entries':len(database),'origins':dict(Counter(origin.values())),'source_files_in_scope':len(all_sources),'missing':[str(p.relative_to(ROOT)) for p in all_sources if p not in entries],'inferred':fallback,'warnings':warnings,'user_integration_files':[str(p.relative_to(ROOT)) for p in scoped_files],'user_integration_inferred':scoped_inferred}
    (OUT/'index_coverage.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps({k:v for k,v in report.items() if k!='inferred'},indent=2))


if __name__=='__main__':main()
