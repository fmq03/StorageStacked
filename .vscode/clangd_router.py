#!/usr/bin/env python3
"""Isolate independent platforms' clangd indices behind one VS Code LSP endpoint.

Independent executables reuse main, kernel_main and storage_axi::Master. A
single clangd index merges their USRs even with correct compile commands.
This transport routes documents to separate unmodified clangd processes; it
never synthesizes navigation results. Non-LSP CLI options go straight to clangd.
"""
import copy
import ctypes
import json
import os
from pathlib import Path
import subprocess
import sys
import threading
from urllib.parse import unquote, urlparse

ROOT=Path(__file__).resolve().parents[1]
BINARY=ROOT/'.vscode/tools/clangd_22.1.0/bin/clangd'
if any(arg in ('--version','--help','--help-hidden') or arg.startswith('--check') for arg in sys.argv[1:]):
    os.execv(str(BINARY),[str(BINARY),*sys.argv[1:]])
PLATFORMS=['vortex_StorageStacked','coralnpu_StorageStacked','axi_StorageStacked']
for platform in PLATFORMS[:2]:
    PLATFORMS += [str(path.relative_to(ROOT)) for path in sorted((ROOT/platform/'user').iterdir())
                  if (path/'src/Makefile').is_file() and (path/'config.json').is_file()]
LOCK=threading.RLock()
PROCESSES=[]
PENDING={}
SERVER_REQUESTS={}
SERIAL=0
OUTPUT_LOCK=threading.Lock()
BACKEND_LOCKS=[threading.Lock() for _ in PLATFORMS]
LIBC=ctypes.CDLL(None)

def parent_death_signal():
    LIBC.prctl(1,15)  # PR_SET_PDEATHSIG, SIGTERM (Remote-SSH runs on Linux).

def read(stream):
    headers={}
    while True:
        line=stream.readline()
        if not line:return None
        if line in (b'\r\n',b'\n'):break
        name,value=line.decode().split(':',1);headers[name.lower()]=value.strip()
    return json.loads(stream.read(int(headers['content-length'])))

def write(stream,message,lock):
    body=json.dumps(message,separators=(',',':')).encode()
    with lock:
        stream.write(f'Content-Length: {len(body)}\r\n\r\n'.encode()+body);stream.flush()

def output(message):write(sys.stdout.buffer,message,OUTPUT_LOCK)
def send(index,message):write(PROCESSES[index].stdin,message,BACKEND_LOCKS[index])

def route(params):
    def uris(value):
        if isinstance(value,str) and value.startswith('file://'):yield value
        elif isinstance(value,dict):
            for item in value.values():yield from uris(item)
        elif isinstance(value,list):
            for item in value:yield from uris(item)
    for uri in uris(params):
        path=Path(unquote(urlparse(uri).path))
        for index,name in sorted(enumerate(PLATFORMS),key=lambda item:len(item[1]),reverse=True):
            if path==ROOT/name or ROOT/name in path.parents:return index
    return 2

def backend(index):
    while True:
        message=read(PROCESSES[index].stdout)
        if message is None:return
        with LOCK:
            if message.get('method') in ('$/progress','window/workDoneProgress/create'):
                params=message.get('params',{})
                if 'token' in params:params['token']=f"backend-{index}:{params['token']}"
            if message.get('method')=='client/registerCapability':
                for registration in message['params']['registrations']:
                    registration['id']=f"backend-{index}:{registration['id']}"
            if message.get('method')=='client/unregisterCapability':
                for registration in message['params'].get('unregisterations',[]):
                    registration['id']=f"backend-{index}:{registration['id']}"
            if 'method' not in message and 'id' in message:
                pending=PENDING.get(message['id'])
                if pending is None:continue
                pending['responses'][index]=message
                if len(pending['responses'])!=pending['count']:continue
                del PENDING[message['id']]
                chosen=copy.deepcopy(pending['responses'][min(pending['responses'])]);chosen['id']=pending['original']
                if pending['method']=='workspace/symbol':
                    chosen.pop('error',None)
                    unique={}
                    for response in pending['responses'].values():
                        for symbol in response.get('result') or []:
                            unique[json.dumps(symbol,sort_keys=True)]=symbol
                    chosen['result']=list(unique.values())
                output(chosen)
            elif 'method' in message and 'id' in message:
                original=message['id'];message['id']=f'backend-{index}-{original}'
                SERVER_REQUESTS[message['id']]=(index,original)
                output(message)
            else:output(message)

def start(params):
    for index,name in enumerate(PLATFORMS):
        args=[str(BINARY),*sys.argv[1:]]
        # Database discovery remains controlled by the checked-in .clangd.
        process=subprocess.Popen(args,stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=sys.stderr,cwd=ROOT/name,preexec_fn=parent_death_signal)
        PROCESSES.append(process)
        threading.Thread(target=backend,args=(index,),daemon=True).start()

def main():
    global SERIAL
    while True:
        message=read(sys.stdin.buffer)
        if message is None:break
        with LOCK:
            if 'method' not in message:
                target=SERVER_REQUESTS.pop(message.get('id'),None)
                if target:
                    index,identifier=target;message['id']=identifier;send(index,message)
                continue
            method=message['method'];params=message.get('params',{})
            if method=='initialize':start(params)
            if method=='exit':break
            if not PROCESSES:continue
            broadcast=method in ('initialize','initialized','shutdown','workspace/symbol','workspace/didChangeConfiguration','workspace/didChangeWatchedFiles','$/cancelRequest')
            targets=list(range(len(PROCESSES))) if broadcast else [route(params)]
            if method=='window/workDoneProgress/cancel' and str(params.get('token','')).startswith('backend-'):
                prefix,token=params['token'].split(':',1)
                targets=[int(prefix.removeprefix('backend-'))];params['token']=token
            if method=='workspace/didChangeWorkspaceFolders':continue
            if 'id' in message:
                SERIAL+=1
                PENDING[SERIAL]={'original':message['id'],'method':method,'count':len(targets),'responses':{}}
                message['id']=SERIAL
            elif method=='$/cancelRequest':
                identifiers=[identifier for identifier,pending in PENDING.items() if pending['original']==params.get('id')]
                for identifier in identifiers:
                    for index in targets:send(index,{'jsonrpc':'2.0','method':method,'params':{'id':identifier}})
                continue
            for index in targets:
                forwarded=copy.deepcopy(message)
                if method=='initialize':
                    folder=ROOT/PLATFORMS[index]
                    forwarded['params'].update(rootUri=folder.as_uri(),rootPath=str(folder),workspaceFolders=[{'uri':folder.as_uri(),'name':folder.name}])
                send(index,forwarded)
    for process in PROCESSES:
        process.terminate()
    for process in PROCESSES:
        process.wait()

if __name__=='__main__':
    try:main()
    finally:
        for process in PROCESSES:
            if process.poll() is None:process.kill()
