#!/usr/bin/env python3
"""Minimal JSON-RPC client for editor navigation checks."""
import json, subprocess, threading
from pathlib import Path
from urllib.parse import unquote, urlparse
ROOT=Path(__file__).resolve().parents[1]

class Client:
    def __init__(self, command, analysis=None, interpreter=None, workspace=ROOT, configuration=None):
        self.analysis = analysis or {}
        self.configuration = configuration or {}
        self.interpreter = interpreter
        self.process = subprocess.Popen(command, cwd=workspace, stdin=subprocess.PIPE,
                                        stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
        self.timer = threading.Timer(600, self.process.kill)
        self.timer.start()
        self.opened = set()
        self.diagnostics = {}
        self.identifier = 1
        self.send({"id": 1, "method": "initialize", "params": {
            "processId": None, "rootUri": workspace.as_uri(),
            "workspaceFolders": [{"uri": workspace.as_uri(), "name": workspace.name}],
            "capabilities": {"workspace": {"configuration": True}, "textDocument": {
                "semanticTokens": {"requests": {"full": True}, "tokenTypes": [],
                                   "tokenModifiers": [], "formats": ["relative"]}}},
        }})
        self.capabilities = self.receive(1).get('capabilities', {})
        self.send({"method": "initialized", "params": {}})

    def send(self, message):
        message["jsonrpc"] = "2.0"
        body = json.dumps(message).encode()
        self.process.stdin.write(f"Content-Length: {len(body)}\r\n\r\n".encode() + body)
        self.process.stdin.flush()

    def receive(self, identifier):
        while True:
            headers = {}
            while True:
                line = self.process.stdout.readline()
                if not line:
                    raise RuntimeError("language server exited before its definition response")
                if line == b"\r\n":
                    break
                name, value = line.decode().split(":", 1)
                headers[name.lower()] = value.strip()
            message = json.loads(self.process.stdout.read(int(headers["content-length"])))
            if message.get('method') == 'textDocument/publishDiagnostics':
                self.diagnostics[message['params']['uri']] = message['params']['diagnostics']
            if message.get("method") == "workspace/configuration":
                values = []
                for item in message["params"]["items"]:
                    section = item.get("section")
                    values.append(self.analysis if section == "python.analysis" else
                                  {"pythonPath": self.interpreter, "analysis": self.analysis}
                                  if section == "python" else self.configuration.get(section, {}))
                self.send({"id": message["id"], "result": values})
            elif message.get("method") and "id" in message:
                self.send({"id": message["id"], "result": None})
            elif message.get("id") == identifier:
                if "error" in message:
                    raise RuntimeError(message["error"])
                return message.get("result")

    def definition(self, relative, needle, token, language):
        source = ROOT / relative
        content = source.read_text()
        row, line = next((index, line) for index, line in enumerate(content.splitlines()) if needle in line)
        column = line.index(token, line.index(needle))
        self.open(source, language)
        return self.definition_at(source, row, column + 1)

    def open(self, source, language):
        source = Path(source)
        if source not in self.opened:
            self.send({"method": "textDocument/didOpen", "params": {"textDocument": {
                "uri": source.as_uri(), "languageId": language, "version": 1, "text": source.read_text(),
            }}})
            self.opened.add(source)

    def request(self, method, params):
        self.identifier += 1
        self.send({'id': self.identifier, 'method': method, 'params': params})
        return self.receive(self.identifier)

    def definition_at(self, source, row, column):
        result = self.request('textDocument/definition', {
            'textDocument': {'uri': source.as_uri()}, 'position': {'line': row, 'character': column},
        })
        locations = result if isinstance(result, list) else [result] if result else []
        return [str(Path(unquote(urlparse(item.get("uri", item.get("targetUri"))).path)).resolve())
                for item in locations]

    def close(self):
        self.timer.cancel()
        try:
            self.send({'method':'exit','params':{}})
            self.process.wait(timeout=10)
        except (BrokenPipeError, subprocess.TimeoutExpired):
            self.process.kill()
            self.process.wait()

