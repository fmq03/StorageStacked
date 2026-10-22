"""Exercise HTTPS redirects and interrupted CONNECT through a real proxy."""
import http.server
import io
import json
import os
from pathlib import Path
import select
import socket
import socketserver
import ssl
import subprocess
import tarfile
import tempfile
import threading
from urllib.parse import urlsplit

script = Path(os.environ.get('STORAGE_TEST_SCRIPT',
                             Path(__file__).with_name('download-release.sh'))).resolve()
extract = Path(os.environ.get('STORAGE_TEST_EXTRACT',
                              Path(__file__).with_name('extract-release.sh'))).resolve()
bundle = 'zhongxing-storagestacked-offline-20261009-burst'
name = bundle+'.tar.part-0000'
contents = {entry: b'test fixture\n' for entry in
            ('run.sh', '使用说明.md', 'SOURCE_VERSIONS.json', 'DELIVERY.json',
             'ACCEPTANCE.json', 'VALIDATION.json', 'zhongxing-storagestacked-offline.tar.gz')}
archive = io.BytesIO()
with tarfile.open(fileobj=archive, mode='w') as tar:
    for entry, value in contents.items():
        info = tarfile.TarInfo(bundle+'/'+entry)
        info.size = len(value)
        tar.addfile(info, io.BytesIO(value))
payload = archive.getvalue()
resources = {name: payload, 'PARTS.tsv': f'{name}\t{len(payload)}\n'.encode(),
             'extract.sh': extract.read_bytes(), 'PACKAGE.json': b'{}\n',
             'SPLIT_ACCEPTANCE.json': b'{}\n'}
patch_files = ('run.sh', 'docker-compat-entrypoint.py', 'run-seccomp-check.sh', 'DOCKER_COMPATIBILITY.md')
resources.update({name: Path(__file__).with_name(name).read_bytes() for name in patch_files})
connect_dropped = False
body_dropped = False
requests = []
redirects = []


class Origin(http.server.BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_GET(self):
        global body_dropped
        entry = self.path.lstrip('/')
        value = resources[entry]
        offset = self.headers.get('Range')
        requests.append((entry, offset))
        start = int(offset.split('=')[1].split('-')[0]) if offset else 0
        self.send_response(206 if offset else 200)
        if offset:
            self.send_header('Content-Range', f'bytes {start}-{len(value)-1}/{len(value)}')
        self.send_header('Content-Length', str(len(value)-start))
        self.end_headers()
        if entry == name and not body_dropped:
            body_dropped = True
            self.wfile.write(value[:123])
            self.wfile.flush()
            self.close_connection = True
        else:
            self.wfile.write(value[start:])


class Proxy(socketserver.StreamRequestHandler):
    def handle(self):
        global connect_dropped
        line = self.rfile.readline().decode().split()
        if len(line) != 3:
            return
        method, target, _ = line
        while self.rfile.readline() not in (b'\r\n', b'\n', b''):
            pass
        if method == 'GET':
            path = urlsplit(target).path
            redirects.append(path)
            location = f'https://localhost:{origin.server_port}{path}'
            self.wfile.write(('HTTP/1.1 302 Found\r\nLocation: '+location+
                              '\r\nContent-Length: 0\r\nConnection: close\r\n\r\n').encode())
            self.wfile.flush()
            return
        assert method == 'CONNECT'
        if not connect_dropped:
            connect_dropped = True
            return
        with socket.create_connection(('127.0.0.1', origin.server_port), timeout=5) as upstream:
            self.wfile.write(b'HTTP/1.1 200 Connection established\r\n\r\n')
            self.wfile.flush()
            while True:
                ready, _, _ = select.select([self.connection, upstream], [], [], 15)
                if not ready:
                    return
                for source in ready:
                    try:
                        data = source.recv(65536)
                        if not data:
                            return
                        (upstream if source is self.connection else self.connection).sendall(data)
                    except (ConnectionResetError, BrokenPipeError):
                        return


with tempfile.TemporaryDirectory(prefix='storagestacked-proxy-') as tmp:
    root = Path(tmp)
    config = root/'certificate.conf'
    config.write_text('[req]\ndistinguished_name=dn\nx509_extensions=extensions\nprompt=no\n'
                      '[dn]\nCN=localhost\n[extensions]\nsubjectAltName=DNS:localhost\n')
    subprocess.run(['openssl', 'req', '-x509', '-newkey', 'rsa:2048', '-nodes',
                    '-keyout', str(root/'key.pem'), '-out', str(root/'certificate.pem'),
                    '-days', '1', '-config', str(config)], check=True, capture_output=True)
    origin = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Origin)
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.load_cert_chain(root/'certificate.pem', root/'key.pem')
    origin.socket = context.wrap_socket(origin.socket, server_side=True)
    proxy = socketserver.ThreadingTCPServer(('127.0.0.1', 0), Proxy)
    proxy.daemon_threads = True
    for server in (origin, proxy):
        threading.Thread(target=server.serve_forever, daemon=True).start()
    try:
        proxy_url = f'http://127.0.0.1:{proxy.server_address[1]}'
        env = dict(os.environ, STORAGE_RELEASE_BASE='http://release.test',
                   http_proxy=proxy_url, https_proxy=proxy_url,
                   HTTP_PROXY=proxy_url, HTTPS_PROXY=proxy_url,
                   ALL_PROXY=proxy_url, NO_PROXY='', no_proxy='',
                   CURL_CA_BUNDLE=str(root/'certificate.pem'),
                   STORAGE_DOWNLOAD_JOBS='1', STORAGE_REQUEST_SECONDS='15')
        result = subprocess.run(['bash', str(script), str(root/'downloads')], env=env,
                                capture_output=True, text=True, timeout=90)
        assert result.returncode == 0, result.stdout+'\n'+result.stderr
        assert 'Proxy CONNECT aborted' in result.stderr, result.stderr
        assert redirects and 'curl=56' in result.stderr, result.stderr
        # curl versions differ in retaining the redirect status after CONNECT fails.
        assert 'HTTP=302' in result.stderr or 'HTTP=000' in result.stderr, result.stderr
        assert (name, 'bytes=123-') in requests, requests
        for entry, value in contents.items():
            if entry != 'run.sh':
                assert (root/'downloads'/bundle/entry).read_bytes() == value, entry
        for entry in patch_files:
            assert (root/'downloads'/bundle/entry).read_bytes() == resources[entry], entry
        print(result.stdout, end='')
        print(result.stderr, end='')
        print(json.dumps({'passed': True, 'checks': [
            'HTTP 302 to HTTPS through proxy', 'CONNECT abort retried',
            'TLS certificate verified', 'interrupted body resumed with Range',
            'complete archive extracted']}, ensure_ascii=False))
    finally:
        for server in (proxy, origin):
            server.shutdown()
            server.server_close()
