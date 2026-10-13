import struct
CVM='/home/kali/ps2-work/Haunting Ground/iso/DATA.CVM'; HDR=3
f=open(CVM,'rb')
def sec(n,c=1): f.seek((n+HDR)*2048); return f.read(2048*c)
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
root=sec(16)[156:190]
walk(struct.unpack_from('<I',root,2)[0],struct.unpack_from('<I',root,10)[0],'')
def read(p):
    ext,sz=found[p]; return sec(ext,(sz+2047)//2048)[:sz]
