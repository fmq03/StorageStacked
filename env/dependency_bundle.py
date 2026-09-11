#!/usr/bin/env python3
"""Package and restore pinned Linux x86-64 sources and original download caches."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
from urllib.parse import urlsplit

ROOT = Path(__file__).resolve().parents[1]
LOCKS = ('sources.lock.json', 'conda-linux-64.lock',
         'xpu-runtime-linux-64.lock', 'xpu-artifacts.lock.json')
MAMBA_URL = 'https://micro.mamba.pm/api/micromamba/linux-64/2.3.3'
MAMBA_SHA = 'e7274528ceb9c20d048a428d6c22d7e02e268f8ffb762c4c365422347c8b8ba2'


def run(*args, cwd=None):
    return subprocess.check_output(args, cwd=cwd, text=True).strip()


def digest(path, algorithm='sha256'):
    h = hashlib.new(algorithm)
    with path.open('rb') as source:
        for block in iter(lambda: source.read(1024 * 1024), b''):
            h.update(block)
    return h.hexdigest()


def copy_file(source, destination):
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, destination)


def submodules(repo, prefix=''):
    if not (repo / '.gitmodules').exists():
        return []
    lines = run('git', 'config', '--file', str(repo / '.gitmodules'),
                '--get-regexp', r'^submodule\..*\.path$').splitlines()
    records = []
    for line in lines:
        key, relative = line.split(None, 1)
        child = repo / relative
        path = str(Path(prefix) / relative)
        revision = run('git', 'ls-tree', 'HEAD', '--', relative, cwd=repo).split()[2]
        if run('git', 'rev-parse', 'HEAD', cwd=child) != revision:
            raise RuntimeError('Submodule HEAD differs from its parent: ' + path)
        url = run('git', 'remote', 'get-url', 'origin', cwd=child)
        if not url.startswith(('https://', 'http://', 'git@')):
            raise RuntimeError('Expected a public upstream URL: ' + path)
        records.append(dict(path=path, parent=prefix or '.', relative=relative,
                            key=key[:-5], revision=revision, url=url))
        records.extend(submodules(child, path))
    return records


def write_manifest(folder, modules):
    manifest = {
        'format': 1, 'platform': 'linux-x86_64',
        'workspace_revision': run('git', 'rev-parse', 'HEAD', cwd=ROOT),
        'locks': {name: digest(ROOT / 'env' / name) for name in LOCKS},
        'submodules': modules,
        'files': {str(p.relative_to(folder)): {'sha256': digest(p), 'bytes': p.stat().st_size}
                  for p in sorted(folder.rglob('*')) if p.is_file() and p != folder / 'manifest.json'},
    }
    (folder / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')


def pack(folder, deps):
    if run('git', 'status', '--porcelain', '--ignore-submodules=all', cwd=ROOT):
        raise RuntimeError('Commit main-repository source changes before packaging')
    if folder.exists():
        raise RuntimeError('Choose a new package directory: ' + str(folder))
    folder.mkdir(parents=True)
    modules = submodules(ROOT)
    for item in modules:
        target = folder / 'git' / (item['path'] + '.git')
        target.parent.mkdir(parents=True, exist_ok=True)
        # Bare local clones retain shallow boundaries and contain no working-tree
        # patches, build products, credentials or machine-specific Git config.
        subprocess.run(['git', 'clone', '--bare', '--no-hardlinks',
                        str(ROOT / item['path']), str(target)], check=True)
        subprocess.run(['git', '--git-dir', str(target), 'remote', 'set-url',
                        'origin', item['url']], check=True)
        item['bundle_path'] = str(target.relative_to(folder))
    subprocess.run(['git', 'bundle', 'create', str(folder / 'system.bundle'),
                    'HEAD', '--branches'], cwd=ROOT, check=True)
    for name in LOCKS:
        copy_file(ROOT / 'env' / name, folder / 'locks' / name)

    downloads = []
    for name in ('conda-linux-64.lock', 'xpu-runtime-linux-64.lock'):
        for line in (ROOT / 'env' / name).read_text().splitlines():
            if not line.startswith('https://'):
                continue
            url, md5 = line.rsplit('#', 1)
            filename = Path(urlsplit(url).path).name
            source = deps / 'mamba/pkgs' / filename
            if not source.exists() or digest(source, 'md5') != md5:
                raise RuntimeError('Missing or incorrect cached package: ' + str(source))
            relative = 'cache/mamba/pkgs/' + filename
            copy_file(source, folder / relative)
            downloads.append(dict(url=url, md5=md5, path=relative))

    archive = deps / 'downloads/micromamba-2.3.3.tar.bz2'
    if not archive.exists():
        archive.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run(['curl', '-fsSL', '--retry', '3', MAMBA_URL, '-o', str(archive)], check=True)
    if digest(archive) != MAMBA_SHA:
        raise RuntimeError('Micromamba archive checksum mismatch')
    copy_file(archive, folder / 'cache/downloads' / archive.name)
    downloads.append(dict(url=MAMBA_URL, sha256=MAMBA_SHA,
                          path='cache/downloads/' + archive.name))

    artifacts = json.loads((ROOT / 'env/xpu-artifacts.lock.json').read_text())
    for name, relative in (('bazel-8.6.0-linux-x86_64', 'xpu-tools/bin/bazel'),
                           ('lz4-1.10.0.tar.gz', 'xpu-downloads/lz4-1.10.0.tar.gz')):
        source = deps / relative
        if digest(source) != artifacts[name]['sha256']:
            raise RuntimeError('Artifact checksum mismatch: ' + str(source))
        copy_file(source, folder / 'cache' / relative)
        downloads.append(dict(artifacts[name], path='cache/' + relative))
    for item in artifacts['vortex']:
        relative = 'xpu-downloads/vortex/' + Path(urlsplit(item['url']).path).name
        source = deps / relative
        if digest(source) != item['sha256']:
            raise RuntimeError('Vortex archive checksum mismatch: ' + str(source))
        copy_file(source, folder / 'cache' / relative)
        downloads.append(dict(item, path='cache/' + relative))
    for item in artifacts['cmake_sources']:
        relative = 'xpu-downloads/cmake/' + item['name'] + '-' + item['revision'] + '.tar.gz'
        source = deps / relative
        if digest(source) != item['sha256']:
            raise RuntimeError('CMake source checksum mismatch: ' + str(source))
        copy_file(source, folder / 'cache' / relative)
        downloads.append(dict(item, path='cache/' + relative))

    # Download cache only: no Bazel execution root, compiler outputs or server state.
    cache = deps / 'bazel/cache/repos/v1'
    if cache.exists():
        shutil.copytree(cache, folder / 'cache/bazel/cache/repos/v1')
    (folder / 'downloads.json').write_text(json.dumps(downloads, indent=2) + '\n')
    copy_file(ROOT / 'docs/setup.md', folder / 'SETUP.md')
    write_manifest(folder, modules)
    print('Package directory ready:', folder)
    print('Archive this whole directory and distribute its SHA256 separately.')


def verify(folder):
    manifest = json.loads((folder / 'manifest.json').read_text())
    if manifest['format'] != 1:
        raise RuntimeError('Unsupported package format')
    actual = {str(p.relative_to(folder)) for p in folder.rglob('*')
              if p.is_file() and p != folder / 'manifest.json'}
    if actual != set(manifest['files']):
        raise RuntimeError('Package file inventory differs from manifest')
    for name, expected in manifest['locks'].items():
        if digest(ROOT / 'env' / name) != expected:
            raise RuntimeError('Package does not match this checkout: ' + name)
    for relative, item in manifest['files'].items():
        path = folder / relative
        if path.stat().st_size != item['bytes'] or digest(path) != item['sha256']:
            raise RuntimeError('Package checksum mismatch: ' + relative)
    print('Package checksums and source/toolchain locks match.')
    return manifest


def install(folder, deps):
    manifest = verify(folder)
    for item in manifest['submodules']:
        parent = ROOT / item['parent']
        target = ROOT / item['path']
        if not (target / '.git').exists():
            # Temporary local transport: the published URLs remain in .gitmodules
            # and are restored in local config immediately after initialization.
            subprocess.run(['git', 'config', item['key'] + '.url',
                            str(folder / item['bundle_path'])], cwd=parent, check=True)
            try:
                subprocess.run(['git', '-c', 'protocol.file.allow=always', 'submodule',
                                'update', '--init', '--', item['relative']], cwd=parent, check=True)
            finally:
                subprocess.run(['git', 'config', item['key'] + '.url', item['url']],
                               cwd=parent, check=True)
            subprocess.run(['git', 'remote', 'set-url', 'origin', item['url']], cwd=target, check=True)
        if run('git', 'rev-parse', 'HEAD', cwd=target) != item['revision']:
            raise RuntimeError('Existing submodule has a different revision: ' + item['path'])
    shutil.copytree(folder / 'cache', deps, dirs_exist_ok=True)
    print('Source submodules and original download caches are ready.')
    print('Next: export SS_DEPS_ROOT=' + str(deps))
    print('      SS_OFFLINE=1 bash env/bootstrap_xpu.sh')
    print('      bash env/build_xpu.sh   # Bazel may download additional dependencies')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=('pack', 'verify', 'install'))
    parser.add_argument('directory', type=Path)
    parser.add_argument('--deps-root', type=Path,
                        default=Path(os.environ.get('SS_DEPS_ROOT',
                                     str(Path.home() / '.local/share/storagestacked-unified'))))
    args = parser.parse_args()
    folder = args.directory.expanduser().resolve()
    deps = args.deps_root.expanduser().resolve()
    if args.action == 'pack':
        pack(folder, deps)
    elif args.action == 'verify':
        verify(folder)
    else:
        install(folder, deps)


if __name__ == '__main__':
    main()
