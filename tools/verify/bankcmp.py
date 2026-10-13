import os
import sys, struct, numpy as np
sys.path.insert(0, '/home/kali/ps2-work/HauntingGround-Recomp-Project/tools/formats')
import hdbd
exec(open(os.path.join(os.path.dirname(os.path.abspath(__file__)),'adxcmp.py')).read().split("ext,sz=found[sys.argv[1]]")[0].replace('sys.argv[1]','X'))
def rd(p):
    ext,sz=found[p]; return sec(ext,(sz+2047)//2048)[:sz]
hd=rd(sys.argv[1]); bd=rd(sys.argv[2])
info=hdbd.parse_hd(hd)
raw=open(sys.argv[3],'rb').read()[44:]
cap=np.frombuffer(raw[:len(raw)//4*4],dtype=np.int16).reshape(-1,2).astype(np.float64)
t0,t1=float(sys.argv[4]),float(sys.argv[5])
seg=cap[int(t0*48000):int(t1*48000)].mean(axis=1)
vag=info[0]
print('samples in bank:', len(vag))
for i,v in enumerate(vag):
    off,rate=v[0],v[1]
    pcm=hdbd.decode_vag(bd,off)[0]
    if len(pcm)<200: continue
    ref=np.interp(np.arange(0,len(pcm),rate/48000.0),np.arange(len(pcm)),pcm)
    w=ref[:min(len(ref),48000)]
    n=len(seg)+len(w); F=np.fft.rfft(seg,2*n); G=np.fft.rfft(w,2*n)
    cc=np.fft.irfft(F*np.conj(G))[:len(seg)]
    lag=int(np.argmax(np.abs(cc))); L=min(len(seg)-lag,len(w))
    if L<100: continue
    r=np.corrcoef(seg[lag:lag+L],w[:L])[0,1]
    print('sample %2d rate %5d len %.2fs: best lag %.3fs corr %.3f'%(i,rate,len(pcm)/rate,t0+lag/48000,r))
