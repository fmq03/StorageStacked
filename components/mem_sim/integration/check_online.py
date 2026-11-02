#!/usr/bin/env python3
"""Exercise the C ABI: masked bytes, rejected splits, queue retry and completions."""
import ctypes as c
import json
from pathlib import Path
import sys

library, output = map(Path, sys.argv[1:])
output.mkdir(parents=True,exist_ok=True)
lib=c.CDLL(str(library.resolve()))
lib.ss_mem_create.argtypes=[c.c_char_p,c.c_uint,c.c_uint,c.c_uint,c.c_uint64,c.c_char_p]
lib.ss_mem_create.restype=c.c_void_p
lib.ss_mem_error.restype=c.c_char_p
for name in ('ss_mem_step','ss_mem_finish','ss_mem_destroy'):
    getattr(lib,name).argtypes=[c.c_void_p]
lib.ss_mem_destroy.restype=None
lib.ss_mem_clock.argtypes=[c.c_void_p];lib.ss_mem_clock.restype=c.c_uint64
lib.ss_mem_submit.argtypes=[c.c_void_p,c.c_uint64,c.c_uint64,c.c_uint,c.c_int,c.c_void_p,c.c_void_p]
class Response(c.Structure):
    _fields_=[('id',c.c_uint64),('issued',c.c_uint64),('done',c.c_uint64),
              ('size',c.c_uint32),('status',c.c_uint32),('data',c.c_uint8*64)]
lib.ss_mem_pop.argtypes=[c.c_void_p,c.POINTER(Response)]
h=lib.ss_mem_create(b'hbm4',2,1,1,8192,str(output).encode())
assert h,lib.ss_mem_error()
data=(c.c_uint8*8)(*range(1,9));mask=(c.c_uint8*8)(1,0,1,0,1,0,1,0)
received={}

def step():
    assert lib.ss_mem_step(h)==0,lib.ss_mem_error()
    while True:
        r=Response();got=lib.ss_mem_pop(h,c.byref(r));assert got>=0,lib.ss_mem_error()
        if not got: break
        assert r.id not in received and r.status==0
        assert 0<r.issued<=r.done<=lib.ss_mem_clock(h)
        received[r.id]=bytes(r.data[:r.size])

try:
    # A child must not cross a native transaction boundary.
    assert lib.ss_mem_submit(h,999,31,8,1,data,mask)==-1
    assert lib.ss_mem_submit(h,1,3,8,1,data,mask)==1
    assert lib.ss_mem_submit(h,1,3,8,1,data,mask)==-1  # duplicate live ID
    assert lib.ss_mem_submit(h,2,35,8,1,data,mask)==0  # full ingress: no side effects
    assert lib.ss_mem_pop(h,c.byref(Response()))==0
    retried=False
    for _ in range(10000):
        step()
        if not retried:
            result=lib.ss_mem_submit(h,2,35,8,1,data,mask);assert result>=0
            retried=bool(result)
        if len(received)==2:break
    assert retried and set(received)=={1,2}
    assert lib.ss_mem_submit(h,3,3,8,0,None,None)==1
    for _ in range(10000):
        step()
        if 3 in received:break
    assert received[3]==bytes([1,0,3,0,5,0,7,0])
    assert lib.ss_mem_finish(h)==0,lib.ss_mem_error()
    core=json.loads((output/'memsim_core.json').read_text())
    assert core['passed'] and core['submitted']==core['returned']==3 and core['submit_stalls']>0
finally:
    lib.ss_mem_destroy(h)
masked_read=list(received[3])
# A GPU BAR needs more than 32 address bits. Check real low/high storage, sparse
# zero initialization, masked writes and capacity rejection through the C ABI.
large=output/'large';large.mkdir(exist_ok=True)
assert not lib.ss_mem_create(b'hbm4',8,1,1,(1<<33)+1,str(large).encode())
h=lib.ss_mem_create(b'hbm4',8,1,1,1<<33,str(large).encode())
assert h,lib.ss_mem_error()
received={}
def transact(ident,address,write=False,payload=None,byte_mask=None):
    assert lib.ss_mem_submit(h,ident,address,8,int(write),payload,byte_mask)==1,lib.ss_mem_error()
    for _ in range(10000):
        step()
        if ident in received:return received[ident]
    raise AssertionError('Native memory response timed out')
try:
    assert lib.ss_mem_submit(h,99,1<<33,8,0,None,None)==-1
    assert transact(10,(1<<32)+3)==bytes(8)
    transact(11,3,True,data,mask)
    high=(c.c_uint8*8)(*range(11,19))
    transact(12,(1<<32)+3,True,high,mask)
    assert transact(13,3)==bytes(masked_read)
    assert transact(14,(1<<32)+3)==bytes([11,0,13,0,15,0,17,0])
    assert lib.ss_mem_finish(h)==0,lib.ss_mem_error()
    import csv
    with (large/'memsim_image.csv').open() as f: materialized=list(csv.DictReader(f))
    assert len(materialized)<1024, 'Large address window must stay sparse'
finally:
    lib.ss_mem_destroy(h)
(output/'api_check.json').write_text(json.dumps({'passed':True,'masked_read':masked_read,'queue_retry':retried,
    'large_window_bytes':1<<33,'high_address_no_alias':True,'sparse_zero_initialized':True})+'\n')
print('Online C ABI PASS: masked data, full-queue retry, duplicate/boundary rejection, real completions')
