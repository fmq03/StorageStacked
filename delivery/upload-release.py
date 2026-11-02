"""Upload independent release assets with bounded concurrency and resume by size."""
import argparse
from collections import deque
import concurrent.futures
import datetime
import json
from pathlib import Path
import subprocess
import threading
import time
from urllib.parse import urlencode


class UploadPacer:
    """Keep upload attempts within a rolling API window, including after resume."""
    window = 3605.0

    def __init__(self, journal, limit=450, clock=time.time, wait=None):
        if not 1 <= limit <= 450:
            raise ValueError('hourly upload limit must be in 1..450')
        self.journal, self.limit, self.clock = journal, limit, clock
        self.stopped = threading.Event()
        self.wait = wait or self.stopped.wait
        self.lock = threading.Lock()
        self.starts = deque(json.loads(journal.read_text()) if journal.exists() else [])
        self.next_start = self.starts[-1]+1.1 if self.starts else 0.0

    def reserve(self):
        with self.lock:
            while True:
                if self.stopped.is_set():
                    raise RuntimeError('Upload cancelled')
                now = self.clock()
                while self.starts and now-self.starts[0] >= self.window:
                    self.starts.popleft()
                delay = max(0.0, self.next_start-now)
                if len(self.starts) >= self.limit:
                    delay = max(delay, self.starts[0]+self.window-now)
                if delay > 0:
                    if delay >= 30:
                        print(f'按接口写入额度等待：约 {delay:.0f} 秒；已上传文件保留。', flush=True)
                    self.wait(min(delay, 30.0))
                    continue
                self.starts.append(now)
                self.next_start = now+1.1
                self.journal.parent.mkdir(parents=True, exist_ok=True)
                temporary = self.journal.with_suffix(self.journal.suffix+'.part')
                temporary.write_text(json.dumps(list(self.starts))+'\n')
                temporary.replace(self.journal)
                return


def gh(*args, timeout=900, before_attempt=None):
    for attempt in range(4):
        try:
            if before_attempt:
                before_attempt()
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
    parser.add_argument('--tag', default='offline-20261022')
    parser.add_argument('--workers', type=int, default=4)
    parser.add_argument('--hourly-upload-limit', type=int, default=450)
    parser.add_argument('--report', type=Path, required=True)
    options = parser.parse_args()
    # A draft is discoverable through the authenticated release list before
    # its tag becomes available through the published-release endpoint.
    candidates = gh('api', '--paginate', f'repos/{options.repo}/releases?per_page=100',
                    '--jq', '.[] | {id,draft,html_url,tag_name,upload_url}')
    releases = [item for item in map(json.loads, candidates.splitlines())
                if item['tag_name'] == options.tag]
    if len(releases) != 1:
        raise RuntimeError('未找到唯一对应的 Release：'+options.tag)
    release = releases[0]
    endpoint = f'repos/{options.repo}/releases/{release["id"]}/assets'
    upload_url = release['upload_url'].split('{', 1)[0]

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
    pacer = UploadPacer(options.report.with_suffix('.requests.json'), options.hourly_upload_limit)
    print(f'待上传 {len(pending)}/{len(files)} 个文件，总计 {total_bytes} 字节。', flush=True)

    def upload(path):
        nonlocal completed, completed_bytes
        for attempt in range(6):
            try:
                if pacer.stopped.is_set():
                    raise RuntimeError('Upload cancelled')
                existing = remote.get(path.name) if attempt == 0 else assets().get(path.name)
                if existing:
                    if existing['state'] == 'uploaded' and existing['size'] == path.stat().st_size:
                        break
                    gh('api', '--method', 'DELETE', f'repos/{options.repo}/releases/assets/{existing["id"]}')
                uploaded = json.loads(gh(
                    'api', '--method', 'POST', upload_url+'?'+urlencode({'name': path.name}),
                    '--header', 'Content-Type: application/octet-stream', '--input', path,
                    '--jq', '{id,name,size,state}', before_attempt=pacer.reserve))
                if (uploaded['name'] != path.name or uploaded['size'] != path.stat().st_size
                        or uploaded['state'] != 'uploaded'):
                    raise RuntimeError('Remote upload response does not match the local file')
                break
            except (RuntimeError, subprocess.TimeoutExpired) as error:
                if attempt == 5:
                    raise RuntimeError(f'{path.name}: {error}') from error
                delay = min(180, 15*2**attempt)
                print(f'重试 {path.name}，{delay} 秒后：{error}', flush=True)
                if pacer.stopped.wait(delay):
                    raise RuntimeError('Upload cancelled')
        with lock:
            completed += 1
            completed_bytes += path.stat().st_size
            print(f'已上传 {completed}/{len(files)}；{completed_bytes}/{total_bytes} 字节；{path.name}', flush=True)

    with concurrent.futures.ThreadPoolExecutor(max_workers=options.workers) as executor:
        futures = {executor.submit(upload, path): path for path in pending}
        failures = []
        try:
            for future in concurrent.futures.as_completed(futures):
                try:
                    future.result()
                except Exception as error:
                    failures.append(str(error))
                    print(f'失败：{error}', flush=True)
        except BaseException:
            pacer.stopped.set()
            for future in futures:
                future.cancel()
            raise
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
