"""Verify bounded waits, visible progress and cancellation with real HTTP/curl."""
import http.server
import io
import json
import os
from pathlib import Path
import signal
import socket
import subprocess
import tarfile
import tempfile
import threading
import time

script = Path(__file__).with_name('download-release.sh').resolve()
bundle = 'zhongxing-storagestacked-offline-20261009-burst'
archive = io.BytesIO()
with tarfile.open(fileobj=archive, mode='w') as tar:
    for name in ('run.sh', '使用说明.md', 'SOURCE_VERSIONS.json', 'DELIVERY.json',
                 'ACCEPTANCE.json', 'VALIDATION.json', 'zhongxing-storagestacked-offline.tar.gz'):
        value = b'test fixture\n'
        info = tarfile.TarInfo(bundle+'/'+name)
        info.size = len(value)
        tar.addfile(info, io.BytesIO(value))
part_name = bundle+'.tar.part-0000'
payload = archive.getvalue()
resources = {part_name: payload, 'PARTS.tsv': f'{part_name}\t{len(payload)}\n'.encode(),
             'extract.sh': script.with_name('extract-release.sh').read_bytes(),
             'PACKAGE.json': b'{}\n', 'SPLIT_ACCEPTANCE.json': b'{}\n'}
patch_files = ('run.sh', 'docker-compat-entrypoint.py', 'run-seccomp-check.sh', 'DOCKER_COMPATIBILITY.md')
resources.update({name: Path(__file__).with_name(name).read_bytes() for name in patch_files})


def check(mode):
    seen = set()
    connected = threading.Event()
    disconnected = threading.Event()

    class Handler(http.server.BaseHTTPRequestHandler):
        def log_message(self, *args):
            pass

        def do_GET(self):
            name = self.path.lstrip('/')
            first = name not in seen
            seen.add(name)
            value = resources[name]
            self.send_response(200)
            self.send_header('Content-Length', str(len(value)))
            self.end_headers()
            if mode == 'interrupt' and name == part_name:
                connected.set()
                self.connection.settimeout(5)
                try:
                    if not self.connection.recv(1):
                        disconnected.set()
                except (ConnectionResetError, BrokenPipeError):
                    disconnected.set()
                except socket.timeout:
                    pass
                return
            if first and mode == 'stall' and name == part_name:
                time.sleep(8)
            if first and mode == 'deadline' and name == 'PARTS.tsv':
                time.sleep(4)
            try:
                self.wfile.write(value)
            except (ConnectionResetError, BrokenPipeError):
                pass

    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        with tempfile.TemporaryDirectory(prefix='storagestacked-stall-') as tmp:
            env = dict(os.environ, STORAGE_RELEASE_BASE=f'http://127.0.0.1:{server.server_port}',
                       NO_PROXY='127.0.0.1', no_proxy='127.0.0.1', STORAGE_DOWNLOAD_JOBS='1',
                       STORAGE_STALL_SECONDS='6' if mode == 'stall' else '60',
                       STORAGE_REQUEST_SECONDS='2' if mode == 'deadline' else '30')
            process = subprocess.Popen(['bash', str(script), tmp], env=env,
                                       stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            if mode == 'interrupt':
                assert connected.wait(10), 'Download did not begin'
                process.send_signal(signal.SIGINT)
                out, err = process.communicate(timeout=8)
                assert process.returncode == 130, (process.returncode, out, err)
                assert disconnected.wait(3), 'curl connection remained active after interrupt'
            else:
                out, err = process.communicate(timeout=35)
                assert process.returncode == 0, out+'\n'+err
                assert 'curl=28' in err, err
                assert (Path(tmp)/bundle/'run.sh').is_file()
                if mode == 'stall':
                    assert '进度：' in err, err
            print(json.dumps({'case': mode, 'passed': True}, ensure_ascii=False), flush=True)
    finally:
        server.shutdown()
        server.server_close()


for mode in ('stall', 'deadline', 'interrupt'):
    check(mode)
