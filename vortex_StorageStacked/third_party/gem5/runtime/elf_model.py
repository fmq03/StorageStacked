"""Carry the build's model snapshot inside a non-loaded ELF section."""
import json
import struct

def sections(data):
    if data[:6] != b'\x7fELF\x01\x01': raise ValueError('Expected a little-endian ELF32 program')
    off=struct.unpack_from('<I',data,32)[0]
    size,count,names=struct.unpack_from('<HHH',data,46)
    if size!=40: raise ValueError('Unsupported ELF section table')
    table=[list(struct.unpack_from('<10I',data,off+i*size)) for i in range(count)]
    strings=table[names]
    return table,names,data[strings[4]:strings[4]+strings[5]]

def attach(path,model):
    data=bytearray(path.read_bytes());table,index,names=sections(data)
    name_offset=len(names);names+=b'.tinyllm_model\0'
    table[index][4]=len(data);table[index][5]=len(names);data+=names
    payload=json.dumps(model,separators=(',',':')).encode()
    table.append([name_offset,1,0,0,len(data),len(payload),0,0,1,0]);data+=payload
    data+=bytes(-len(data)%4)
    struct.pack_into('<I',data,32,len(data));struct.pack_into('<H',data,48,len(table))
    for section in table:data+=struct.pack('<10I',*section)
    path.write_bytes(data)

def read(path):
    data=path.read_bytes();table,_,names=sections(data)
    for s in table:
        if names[s[0]:].split(b'\0',1)[0]==b'.tinyllm_model':return json.loads(data[s[4]:s[4]+s[5]])
    raise ValueError('ELF lacks its TinyLLM model snapshot; rebuild with MODEL_HEADER')
