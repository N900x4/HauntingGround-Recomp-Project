#!/usr/bin/env python3
"""PS-ADPCM encoder + HD/BD sample replacement.
  vagenc.py replace BANK.HD BANK.BD IDX NEW.wav OUT_PREFIX [--rate R]
Rewrites the Vagi offsets/rates in HD and repacks BD. Mono 16-bit WAV in; resampled to R (default: original rate).
"""
import sys, struct, wave, numpy as np
sys.path.insert(0, __import__('os').path.dirname(__file__))
from hdbd import parse_hd, decode_vag
COEF=[(0,0),(60,0),(115,-52),(98,-55),(122,-60)]

def encode(pcm):
    pcm=np.asarray(pcm,dtype=np.float64)
    n=(len(pcm)+27)//28*28; pcm=np.pad(pcm,(0,n-len(pcm)))
    out=bytearray(16)                     # leading silent block, like Sony's tools
    h1=h2=0.0
    for b in range(0,n,28):
        blk=pcm[b:b+28]; best=None
        for f,(c1,c2) in enumerate(COEF):
            for sh in range(13):
                y1,y2=h1,h2; err=0; nib=[]
                for s in blk:
                    pred=(y1*c1+y2*c2)/64.0
                    q=int(round((s-pred)*(1<<sh)/4096.0)); q=max(-8,min(7,q))
                    r=max(-32768,min(32767,((q<<12)>>sh)+pred))
                    err+=(s-r)**2; nib.append(q&15); y2,y1=y1,r
                if best is None or err<best[0]: best=(err,f,sh,nib,y1,y2)
        _,f,sh,nib,h1,h2=best
        hdr=bytes([(f<<4)|sh, 0])
        out+=hdr+bytes(nib[i]|(nib[i+1]<<4) for i in range(0,28,2))
    out+=bytes([0,1])+bytes(14)            # end flag block
    out[1]=0
    out[-16+1]=1; out[-32+1]|=0             # keep flag 1 on last block only
    return bytes(out)

def resample(x,src,dst):
    if src==dst: return x
    t=np.arange(int(len(x)*dst/src))*src/dst
    return np.interp(t,np.arange(len(x)),x)

def replace(hdp,bdp,idx,wavp,outp,rate=None):
    hd=bytearray(open(hdp,'rb').read()); bd=open(bdp,'rb').read()
    vagi,_=parse_hd(bytes(hd))
    vagi_o=struct.unpack_from('<I',hd,0x10+0x20)[0]
    n=struct.unpack_from('<I',hd,vagi_o+12)[0]
    ents=[vagi_o+o for o in struct.unpack_from('<%dI'%(n+1),hd,vagi_o+16)]
    # split BD into per-sample blobs using sorted offsets
    offs=sorted(set(v[0] for v in vagi))+[len(bd)]
    blob={o:bd[o:offs[i+1]] for i,o in enumerate(offs[:-1])}
    w=wave.open(wavp); sr=w.getframerate(); x=np.frombuffer(w.readframes(w.getnframes()),'<i2').astype(float)
    if w.getnchannels()==2: x=x.reshape(-1,2).mean(1)
    rate=rate or vagi[idx][1]
    new=encode(resample(x,sr,rate))
    newbd=bytearray(); remap={}
    for i,(o,r,lf,_) in enumerate(vagi):
        data=new if i==idx else blob[o]
        if i!=idx and o in remap: continue
        remap[o if i!=idx else ('new',)]=len(newbd)
        start=len(newbd); newbd+=data
        struct.pack_into('<I',hd,ents[i],start)
        if i==idx: struct.pack_into('<H',hd,ents[i]+4,rate)
    struct.pack_into('<I',hd,0x10+0x10,len(newbd))   # Head: BD size
    open(outp+'.HD','wb').write(hd); open(outp+'.BD','wb').write(newbd)
    return len(bd),len(newbd),len(new),rate

if __name__=='__main__':
    a=sys.argv
    if a[1]=='replace':
        rate=int(a[a.index('--rate')+1]) if '--rate' in a else None
        print('old BD %d new BD %d new sample %d bytes @%d Hz'%replace(a[2],a[3],int(a[4],0),a[5],a[6],rate))
