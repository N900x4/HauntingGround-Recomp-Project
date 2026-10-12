"""Classify jalr / jr-t9 call sites and find vtable-like pointer arrays in data."""
import r5900 as R, collections, sys, json
S=sys.argv[1]
b=R.load('../../iso/SLUS_210.75'); W=R.words(b)
TEXT_END=0x3a1b80; n=(TEXT_END-0x100000)//4
def w(a): return W[(a-0x100000)//4]
def s16(x): return x-0x10000 if x&0x8000 else x
J=json.load(open(S+'/jumptables.json'))
cls=collections.Counter(); slots=collections.Counter()
for i in range(n):
    x=W[i]; m=R.decode(x)
    if not (m=='jalr' or (m=='jr' and (x>>21)&31==25 and 0x100000+4*i in J['other'])): continue
    r=(x>>21)&31; kind='fnptr_other'
    # look back for lw r, off(r2) and lw r2, 0(obj)
    for j in range(i-1,max(0,i-10),-1):
        y=W[j]
        if y>>26==35 and ((y>>16)&31)==r:   # lw r, off(base)
            base=(y>>21)&31; off=s16(y&0xffff)
            for k in range(j-1,max(0,j-10),-1):
                z=W[k]
                if z>>26==35 and ((z>>16)&31)==base:
                    if s16(z&0xffff)==0 or True: kind='virtual(lw;lw;jalr)'; slots[off]+=1
                    break
            else:
                kind='lw_from_struct/table'
            if base==28: kind='gp_global_fnptr'
            break
        if y>>26==15 and ((y>>16)&31)==r: kind='lui_const'; break
        d = (y>>11)&31 if y>>26==0 else None
        if d==r: kind='computed'; break
    cls[(m,kind)]+=1
for k,v in cls.most_common(): print(k,v)
print('distinct vslot offsets', len(slots), 'max', max(slots))
# vtable-ish runs in data: consecutive words that are text pointers, outside jump tables
jt=set()
for t in J['tables']:
    for k in range(t['n']): jt.add(t['base']+4*k)
runs=[]; cur=[]
for a in range(TEXT_END, 0x47b200, 4):
    v=w(a)
    if 0x100000<=v<TEXT_END and a not in jt: cur.append(a)
    else:
        if cur and len(cur)>=1: runs.append(cur)
        cur=[]
lens=collections.Counter(min(len(r),20) for r in runs)
print('pointer runs in data (non-jumptable):', len(runs), 'total ptrs', sum(len(r) for r in runs))
print('run length histogram (20=20+):', sorted(lens.items()))
big=[r for r in runs if len(r)>=3]
print('runs>=3', len(big), 'ptrs', sum(len(r) for r in big))
print('ctor/sinit table excluded? sinit island ptrs are > text, ok')
json.dump([[r[0],len(r)] for r in runs], open(S+'/ptrruns.json','w'))
