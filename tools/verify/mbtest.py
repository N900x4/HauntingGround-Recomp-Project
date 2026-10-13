import re,sys
src=open('/home/kali/ps2-work/HauntingGround-Recomp-Project/recomp/ipu.c').read()
tab=src[src.index('static const Vlc t_dct0[]'):]
tab=tab[:tab.index('VLC_END')]
dct={c:(int(a),int(b)) for c,a,b in re.findall(r'\{"([01]+)",(-?\d+),(-?\d+)\}',tab)}
dcl={'100':0,'00':1,'01':2,'101':3,'110':4,'1110':5,'11110':6,'111110':7,'1111110':8}
dcc={'00':0,'01':1,'10':2,'110':3,'1110':4,'11110':5,'111110':6,'1111110':7,'11111110':8}
words=sys.argv[1].split()
bits=''.join(bin(int(w,16))[2:].zfill(32) for w in words)
def vlc(t,p):
    for L in range(1,17):
        if bits[p:p+L] in t: return t[bits[p:p+L]],p+L
    return None,p
def block(p,cc):
    s,p=vlc(dcc if cc else dcl,p)
    if s is None: return None,p,'bad dc'
    p+=s; i=1; n=0
    while True:
        e,p2=vlc(dct,p)
        if e is None: return None,p,'bad ac'
        p=p2
        if e[0]==-1: return n,p,'ok'
        if e[0]==-2: p+=6+8; i+=1; n+=1; continue
        p+=1; i+=e[0]+1; n+=1
        if i>64: return None,p,'overflow'
for off in range(0,4):
    p=off; res=[]
    for b in range(6):
        n,p,st=block(p,b>=4); res.append((n,st))
        if st!='ok': break
    print(off,res,'end',p,'next',bits[p:p+12])

def mb(p, trace=False):
    p0=p
    if bits[p]!='1': return None
    p+=1
    if bits[p]!='1': return None
    p+=1
    out=[]
    for b in range(6):
        s,p=vlc(dcc if b>=4 else dcl,p)
        if s is None: return None
        d=bits[p:p+s]; p+=s; acs=[]
        while True:
            e,p2=vlc(dct,p)
            if e is None: return None
            p=p2
            if e[0]==-1: break
            if e[0]==-2: return None
            acs.append((e[0],e[1],bits[p])); p+=1
        out.append((s,d,acs))
    return p-p0,out
