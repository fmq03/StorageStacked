"""Independent numerical oracle: NumPy FP32 scalar-order accumulation, BF16 RNE."""
import numpy as np

def bf16(x):
    u=np.asarray(x,dtype=np.float32).view(np.uint32)
    return ((u.astype(np.uint64)+0x7fff+((u>>16)&1))>>16).astype(np.uint16)

def fp32(x):
    return (np.asarray(x,dtype=np.uint16).astype(np.uint32)<<16).view(np.float32)

def prepare(keys):
    a=fp32(keys)
    acc=np.zeros((a.shape[0],a.shape[2]),dtype=np.float32)
    for token in range(a.shape[1]):
        acc=np.add(acc,a[:,token,:],dtype=np.float32)
    return bf16(np.multiply(acc,np.float32(1/a.shape[1]),dtype=np.float32))

def query(means,q,current,k):
    w=fp32(means); q=fp32(q)
    scores=np.zeros(w.shape[0],dtype=np.float32)
    for d in range(q.size):
        prod=np.multiply(w[:,d],q[d],dtype=np.float32)
        scores=np.add(scores,prod,dtype=np.float32)
    winners=sorted(range(current),key=lambda i:(-float(scores[i]),i))[:max(0,k-1)]
    idx=[current]+winners
    return sum(1<<i for i in idx),idx,scores
