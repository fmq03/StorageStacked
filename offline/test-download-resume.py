"""Exercise the customer downloader with real curl and a local HTTP server."""
import http.server
import io
import json
import os
from pathlib import Path
import subprocess
import tarfile
import tempfile
import threading

script = Path(__file__).with_name('download-release.sh').resolve()
bundle = 'zhongxing-storagestacked-offline-20261009-burst'
payload = bytes(range(256))*400
contents = {name: b'{}\n' for name in ('SOURCE_VERSIONS.json', 'DELIVERY.json',
                                      'ACCEPTANCE.json', 'VALIDATION.json')}
contents.update({'run.sh': b'#!/bin/bash\n', '使用说明.md': b'offline\n',
                 'zhongxing-storagestacked-offline.tar.gz': payload})
archive = io.BytesIO()
with tarfile.open(fileobj=archive, mode='w') as tar:
    for name, value in contents.items():
        info = tarfile.TarInfo(bundle+'/'+name)
        info.size = len(value)
        tar.addfile(info, io.BytesIO(value))
data = archive.getvalue()
parts = {f'{bundle}.tar.part-{i:04d}': data[start:start+20000]
         for i, start in enumerate(range(0, len(data), 20000))}
names = list(parts)
resources = dict(parts)
resources.update({'PARTS.tsv': ''.join(f'{n}\t{len(v)}\n' for n, v in parts.items()).encode(),
                  'extract.sh': script.with_name('extract-release.sh').read_bytes(),
                  'PACKAGE.json': b'{}\n', 'SPLIT_ACCEPTANCE.json': b'{}\n'})
patch_files = ('run.sh', 'docker-compat-entrypoint.py', 'run-seccomp-check.sh', 'DOCKER_COMPATIBILITY.md')
resources.update({name: script.with_name(name).read_bytes() for name in patch_files})
requests = []
short_sent = False
drop_sent = False


class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_GET(self):
        global short_sent, drop_sent
        name = self.path.lstrip('/')
        offset = self.headers.get('Range')
        requests.append((name, offset))
        if name == 'PACKAGE.json' and not drop_sent:
            drop_sent = True
            self.connection.close()
            return
        value = resources[name]
        if name == names[3] and offset:
            self.send_response(416)
            self.send_header('Content-Range', f'bytes */{len(value)}')
            self.send_header('Content-Length', '0')
            self.end_headers()
            return
        if name == names[4] and not short_sent:
            short_sent = True
            value = b'<html>proxy response</html>'
        if offset:
            start = int(offset.split('=')[1].split('-')[0])
            self.send_response(206)
            self.send_header('Content-Range', f'bytes {start}-{len(value)-1}/{len(value)}')
            value = value[start:]
        else:
            self.send_response(200)
        self.send_header('Content-Length', str(len(value)))
        self.end_headers()
        self.wfile.write(value)


server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
thread = threading.Thread(target=server.serve_forever, daemon=True)
thread.start()
try:
    with tempfile.TemporaryDirectory(prefix='storagestacked-resume-') as tmp:
        root = Path(tmp)
        for index, name in enumerate(names):
            if index == 0:
                (root/(name+'.downloading')).write_bytes(parts[name])
            elif index == 1:
                (root/(name+'.downloading')).write_bytes(parts[name]+b'excess')
            elif index in (2, 3):
                (root/(name+'.downloading')).write_bytes(parts[name][:123])
            elif index > 4:
                (root/name).write_bytes(parts[name])
        env = dict(os.environ, STORAGE_RELEASE_BASE=f'http://127.0.0.1:{server.server_port}',
                   NO_PROXY='127.0.0.1', no_proxy='127.0.0.1', STORAGE_DOWNLOAD_JOBS='4')
        result = subprocess.run(['bash', str(script), str(root)], env=env,
                                capture_output=True, text=True, timeout=120)
        assert result.returncode == 0, result.stdout+'\n'+result.stderr
        for name, value in contents.items():
            if name != 'run.sh':
                assert (root/bundle/name).read_bytes() == value, name
        assert (root/bundle/'run-original.sh').read_bytes() == contents['run.sh']
        for name in patch_files:
            assert (root/bundle/name).read_bytes() == script.with_name(name).read_bytes(), name
        missing = root/'missing-patch'
        missing.mkdir()
        (missing/'extract.sh').write_bytes(script.with_name('extract-release.sh').read_bytes())
        (missing/'PARTS.tsv').write_bytes(resources['PARTS.tsv'])
        rejected = subprocess.run(['bash', str(missing/'extract.sh')],
                                  capture_output=True, text=True, timeout=10)
        assert rejected.returncode == 1 and '缺少启动补丁' in rejected.stderr
        assert not (missing/bundle).exists()
        assert not any(n == names[0] for n, _ in requests), 'Complete temporary part was downloaded again'
        assert [(n, r) for n, r in requests if n == names[1]] == [(names[1], None)]
        assert (names[2], 'bytes=123-') in requests
        assert [(n, r) for n, r in requests if n == names[3]] == [(names[3], 'bytes=123-'), (names[3], None)]
        assert [(n, r) for n, r in requests if n == names[4]] == [(names[4], None), (names[4], None)]
        print(result.stdout, end='')
        print(result.stderr, end='')
        print(json.dumps({'passed': True, 'checks': ['complete temporary file promoted',
              'oversized temporary file restarted', 'partial file resumed',
              '416 restarted without Range', 'short HTTP 200 response retried',
              'connection failure retried', 'complete archive extracted',
              'current launcher and compatibility helper installed; frozen launcher preserved',
              'missing patch rejected before extraction']}, ensure_ascii=False))
finally:
    server.shutdown()
    server.server_close()
