#!/usr/bin/env python3
"""CRI ADX decoder (+ demux of ADX audio from Sofdec .SFD).  adx.py IN.(ADX|SFD) OUT.wav"""
import sys, struct, math, wave, numpy as np
def sfd_audio(d):
    out=bytearray(); i=0
    while True:
        i=d.find(b'\x00\x00\x01\xc0',i)
        if i<0: break
        ln=struct.unpack_from('>H',d,i+4)[0]; p=i+6
        while d[p]==0xff: p+=1
        if d[p]&0xc0==0x40: p+=2
        if d[p]&0xf0==0x20: p+=5
        elif d[p]&0xf0==0x30: p+=10
        elif d[p]==0x0f: p+=1
        out+=d[p:i+6+ln]; i=i+6+ln
    return bytes(out)
def decode(a):
    assert a[0]==0x80, 'not ADX'
    off=struct.unpack_from('>H',a,2)[0]+4; bs=a[5]; ch=a[7]; rate=struct.unpack_from('>I',a,8)[0]
    total=struct.unpack_from('>I',a,12)[0]; cut=struct.unpack_from('>H',a,16)[0]
    x=math.sqrt(2)-math.cos(2*math.pi*cut/rate); y=math.sqrt(2)-1; z=(x-math.sqrt((x+y)*(x-y)))/y
    c1=int(z*2*4096); c2=int(-(z*z)*4096)
    spf=(bs-2)*2; nfr=(len(a)-off)//(bs*ch)
    out=np.zeros((nfr*spf,ch),np.int16); h=[[0,0] for _ in range(ch)]
    p=off
    for f in range(nfr):
        for c in range(ch):
            blk=a[p:p+bs]; p+=bs
            scale=struct.unpack_from('>H',blk,0)[0]
            if scale & 0x8000: return out[:f*spf], rate, ch  # end marker
            h1,h2=h[c]; base=f*spf
            for k in range(spf):
                b=blk[2+k//2]; n=(b>>4) if k%2==0 else (b&15)
                if n>=8: n-=16
                s=n*(scale+1)+((c1*h1+c2*h2)>>12)
                s=max(-32768,min(32767,s)); out[base+k,c]=s; h2,h1=h1,s
            h[c]=[h1,h2]
    return out[:min(total,len(out))], rate, ch
if __name__=='__main__':
    d=open(sys.argv[1],'rb').read()
    if d[0]!=0x80: d=sfd_audio(d)
    pcm,rate,ch=decode(d)
    w=wave.open(sys.argv[2],'wb'); w.setnchannels(ch); w.setsampwidth(2); w.setframerate(rate); w.writeframes(pcm.tobytes()); w.close()
    print(sys.argv[2], rate, ch, len(pcm)/rate,'s')
