import sys, struct, numpy as np
sys.path.insert(0, '/home/kali/ps2-work/HauntingGround-Recomp-Project/tools/formats')
import adx
CVM='/home/kali/ps2-work/Haunting Ground/iso/DATA.CVM'; HDR=3
f=open(CVM,'rb')
def sec(n,c=1): f.seek((n+HDR)*2048); return f.read(2048*c)
pvd=sec(16); root=pvd[156:190]
found={}
def walk(lsn,size,path):
    d=sec(lsn,(size+2047)//2048); i=0
    while i<len(d):
        l=d[i]
        if l==0: i=(i//2048+1)*2048; continue
        ext,sz=struct.unpack_from('<I',d,i+2)[0],struct.unpack_from('<I',d,i+10)[0]
        fl=d[i+25]; nl=d[i+32]; name=d[i+33:i+33+nl]
        if name not in (b'\0',b'\1'):
            p=path+'/'+name.decode('latin1').split(';')[0]
            if fl&2: walk(ext,sz,p)
            else: found[p]=(ext,sz)
        i+=l
walk(struct.unpack_from('<I',root,2)[0],struct.unpack_from('<I',root,10)[0],'')
ext,sz=found[sys.argv[1]]
a=sec(ext,(sz+2047)//2048)[:sz]
pcm,rate,ch=adx.decode(a)
print('ADX',sys.argv[1],'rate',rate,'ch',ch,'seconds',len(pcm)/rate)
raw=open(sys.argv[2],'rb').read()[44:]
cap=np.frombuffer(raw[:len(raw)//4*4],dtype=np.int16).reshape(-1,2).astype(np.float64)
t0,t1=float(sys.argv[3]),float(sys.argv[4])
seg=cap[int(t0*48000):int(t1*48000),0]
ref=pcm[:,0].astype(np.float64)
if rate!=48000: print('note: ADX rate differs; resampling by index'); ref=np.interp(np.arange(0,len(ref),rate/48000.0),np.arange(len(ref)),ref)
# find where in the capture the ADX starts: correlate first 2 s of ADX against the segment via FFT
w=ref[:96000]
n=len(seg)+len(w)
F=np.fft.rfft(seg,2*n); G=np.fft.rfft(w,2*n)
cc=np.fft.irfft(F*np.conj(G))[:len(seg)]
lag=int(np.argmax(np.abs(cc)))
L=min(len(seg)-lag, len(ref), 48000*30)
x=seg[lag:lag+L]; y=ref[:L]
r=np.corrcoef(x,y)[0,1]; gain=np.dot(x,y)/np.dot(y,y)
print('best lag %.3f s into the window; correlation over %.1f s: %.4f; gain %.3f'%(lag/48000, L/48000, r, gain))
