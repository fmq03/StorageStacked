"""Model validation, memory layout and an independent float64 full-prefix reference."""
import json
import math
import struct
from pathlib import Path

WEIGHTS, KEY_CACHE, VALUE_CACHE = 0x90000000, 0x90010000, 0x90020000
TRACE, TOKENS, REPORT = 0x90030000, 0x90040000, 0x90050000

def load_model(path):
    m=path if isinstance(path,dict) else json.loads(Path(path).read_text())
    a=m['architecture']
    assert m['format']=='coral-tiny-char-v1' and a['layers']==1
    assert a['dim']==8 and a['heads']==2 and a['hidden_dim']==16 and a['context_length']==16
    assert a['normalization']=='layernorm' and a['activation']=='relu' and a['epsilon']==1e-5
    d,f,v,c=a['dim'],a['hidden_dim'],a['vocab_size'],a['context_length']
    assert 2<=v<=32 and len(m['vocabulary'])==len(set(m['vocabulary']))==v
    shapes={'embedding':[v,d],'position':[c,d], 'q':[d,d],'k':[d,d],'v':[d,d],'o':[d,d],
            'ff1':[d,f],'ff1_bias':[f],'ff2':[f,d],'ff2_bias':[d],'lm_head':[d,v],'lm_bias':[v]}
    for n in ('norm1','norm2','norm_final'):
        shapes[n+'_weight']=[d];shapes[n+'_bias']=[d]
    assert set(m['tensors'])==set(shapes)
    for name,shape in shapes.items():
        t=m['tensors'][name]
        assert t['shape']==shape and len(t['data'])==math.prod(shape),name
        assert all(type(x) in (int,float) and math.isfinite(x) and abs(x)<100 for x in t['data']),name
    return m

def layout(m):
    flat,offsets=[],{}
    for name,t in m['tensors'].items():
        offsets[name]=len(flat);flat+=t['data']
        flat += [0.0]*(-len(flat)%4)
    a=m['architecture'];d=a['dim']
    counts={'embedding':d,'norm1':d,'q':d,'k':d,'v':d,
            'attention':a['heads']*a['context_length'],'attention_output':d,
            'residual':d,'norm2':d,'ff':a['hidden_dim'],'hidden':d,
            'norm_final':d,'logits':a['vocab_size']}
    traces={};cursor=0
    for name,count in counts.items():
        traces[name]={'offset':cursor,'count':count};cursor+=(count+3)//4*4
    assert len(flat)*4<0x10000 and cursor*a['context_length']*4<0x10000
    return flat,offsets,traces,cursor

def encode(m,text):
    if not isinstance(text,str) or not text: raise ValueError('prompt must be a nonempty string')
    try:return [m['vocabulary'].index(c) for c in text]
    except ValueError:raise ValueError('prompt contains characters absent from model vocabulary') from None

def forward(m,tokens):
    """Full causal matrix computation, without the device's incremental KV logic."""
    a=m['architecture'];d,h,f,c=a['dim'],a['heads'],a['hidden_dim'],a['context_length'];dh=d//h
    w={k:v['data'] for k,v in m['tensors'].items()}
    def norm(x,name):
        mean=sum(x)/d;var=sum((v-mean)**2 for v in x)/d
        return [(v-mean)/math.sqrt(var+a['epsilon'])*w[name+'_weight'][j]+w[name+'_bias'][j] for j,v in enumerate(x)]
    def linear(x,name,bias=None):
        width=m['tensors'][name]['shape'][1]
        return [sum(x[i]*w[name][i*width+j] for i in range(len(x)))+(w[bias][j] if bias else 0) for j in range(width)]
    records=[]
    for pos,token in enumerate(tokens):
        x=[w['embedding'][token*d+j]+w['position'][pos*d+j] for j in range(d)]
        n=norm(x,'norm1')
        records.append({'embedding':x,'norm1':n,**{name:linear(n,name) for name in ('q','k','v')}})
    for pos,r in enumerate(records):
        probs=[0.0]*(h*c);mixed=[]
        for head in range(h):
            scores=[sum(r['q'][head*dh+j]*records[t]['k'][head*dh+j] for j in range(dh))/math.sqrt(dh) for t in range(pos+1)]
            exps=[math.exp(x-max(scores)) for x in scores];ps=[x/sum(exps) for x in exps]
            probs[head*c:head*c+len(ps)]=ps
            mixed += [sum(ps[t]*records[t]['v'][head*dh+j] for t in range(pos+1)) for j in range(dh)]
        att=linear(mixed,'o');res=[x+y for x,y in zip(r['embedding'],att)]
        n2=norm(res,'norm2');ff=[max(0,x) for x in linear(n2,'ff1','ff1_bias')]
        hidden=[x+y for x,y in zip(res,linear(ff,'ff2','ff2_bias'))]
        nf=norm(hidden,'norm_final')
        r.update(attention=probs,attention_output=att,residual=res,norm2=n2,ff=ff,hidden=hidden,
                 norm_final=nf,logits=linear(nf,'lm_head','lm_bias'))
    return records

def reference(m,b):
    tokens=encode(m,b['prompt']);generated=[]
    for _ in range(b['generated_tokens']):
        logits=forward(m,tokens)[-1]['logits']
        token=max(range(len(logits)),key=logits.__getitem__)
        generated.append(token);tokens.append(token)
    processed=tokens[:-1]
    return {'tokens':processed,'generated':generated,'text':''.join(m['vocabulary'][i] for i in generated),
            'records':forward(m,processed)}
