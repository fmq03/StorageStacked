"""Upload independent release assets with bounded concurrency and resume by size."""
import argparse
import concurrent.futures
import datetime
import json
from pathlib import Path
import subprocess
import threading
import time


def gh(*args, timeout=900):
    for attempt in range(4):
        try:
            result = subprocess.run(['gh', *map(str, args)], capture_output=True, text=True, timeout=timeout)
            if not result.returncode:
                return result.stdout
            error = result.stderr.strip()[-1800:] or 'GitHub CLI failed'
            transient = any(word in error.lower() for word in
                            ('eof', 'timeout', 'connection reset', '502', '503', '504'))
            if not transient or attempt == 3:
                raise RuntimeError(error)
        except subprocess.TimeoutExpired:
            if attempt == 3:
                raise
        time.sleep(3*2**attempt)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('directory', type=Path)
    parser.add_argument('--repo', required=True)
    parser.add_argument('--tag', default='offline-20261009')
    parser.add_argument('--workers', type=int, default=4)
    parser.add_argument('--report', type=Path, required=True)
    options = parser.parse_args()
    # A draft is discoverable through the authenticated release list before
    # its tag becomes available through the published-release endpoint.
    candidates = gh('api', '--paginate', f'repos/{options.repo}/releases?per_page=100',
                    '--jq', '.[] | {id,draft,html_url,tag_name}')
    releases = [item for item in map(json.loads, candidates.splitlines())
                if item['tag_name'] == options.tag]
    if len(releases) != 1:
        raise RuntimeError('未找到唯一对应的 Release：'+options.tag)
    release = releases[0]
    endpoint = f'repos/{options.repo}/releases/{release["id"]}/assets'

    def assets():
        response = gh('api', '--paginate', endpoint+'?per_page=100',
                      '--jq', '.[] | {name,size,state,id,browser_download_url}')
        return {item['name']: item for item in map(json.loads, response.splitlines())}

    files = sorted(p for p in options.directory.iterdir() if p.is_file())
    assert files and len(files) < 1000
    assert all(p.stat().st_size <= 25000000 for p in files)
    remote = assets()
    pending = [p for p in files if p.name not in remote or
               remote[p.name]['size'] != p.stat().st_size or remote[p.name]['state'] != 'uploaded']
    completed = len(files)-len(pending)
    total_bytes = sum(p.stat().st_size for p in files)
    completed_bytes = sum(p.stat().st_size for p in files if p not in pending)
    lock = threading.Lock()
    start_lock = threading.Lock()
    next_start = 0.0
    print(f'待上传 {len(pending)}/{len(files)} 个文件，总计 {total_bytes} 字节。', flush=True)

    def upload(path):
        nonlocal completed, completed_bytes, next_start
        for attempt in range(6):
            try:
                existing = remote.get(path.name) if attempt == 0 else assets().get(path.name)
                if existing:
                    if existing['state'] == 'uploaded' and existing['size'] == path.stat().st_size:
                        break
                    gh('api', '--method', 'DELETE', f'repos/{options.repo}/releases/assets/{existing["id"]}')
                # Space upload starts to avoid a burst of content-creation requests.
                with start_lock:
                    delay = next_start-time.monotonic()
                    if delay > 0:
                        time.sleep(delay)
                    next_start = time.monotonic()+1.1
                gh('release', 'upload', options.tag, path, '--repo', options.repo)
                break
            except (RuntimeError, subprocess.TimeoutExpired) as error:
                if attempt == 5:
                    raise RuntimeError(f'{path.name}: {error}') from error
                delay = min(180, 15*2**attempt)
                print(f'重试 {path.name}，{delay} 秒后：{error}', flush=True)
                time.sleep(delay)
        with lock:
            completed += 1
            completed_bytes += path.stat().st_size
            print(f'已上传 {completed}/{len(files)}；{completed_bytes}/{total_bytes} 字节；{path.name}', flush=True)

    with concurrent.futures.ThreadPoolExecutor(max_workers=options.workers) as executor:
        futures = {executor.submit(upload, path): path for path in pending}
        failures = []
        for future in concurrent.futures.as_completed(futures):
            try:
                future.result()
            except Exception as error:
                failures.append(str(error))
                print(f'失败：{error}', flush=True)
        if failures:
            raise RuntimeError('有文件未完成；重新执行会跳过已经上传且大小相符的文件。\n'+'\n'.join(failures))
    remote = assets()
    expected = {p.name: p.stat().st_size for p in files}
    assert set(remote) == set(expected), '远端附件文件名集合与本地不一致'
    assert all(remote[name]['state'] == 'uploaded' and remote[name]['size'] == size
               for name, size in expected.items()), '远端附件状态或大小不符'
    report = {'passed': True, 'created_utc': datetime.datetime.now(datetime.timezone.utc).isoformat(),
              'repository': options.repo, 'release': release['html_url'], 'tag': options.tag,
              'assets': [remote[name] for name in sorted(remote)],
              'total_bytes': total_bytes, 'verification': 'remote filenames, sizes and uploaded state'}
    options.report.parent.mkdir(parents=True, exist_ok=True)
    options.report.write_text(json.dumps(report, ensure_ascii=False, indent=2)+'\n')
    print('所有附件上传完成，远端文件名、大小与状态核对通过。', flush=True)


if __name__ == '__main__':
    main()
