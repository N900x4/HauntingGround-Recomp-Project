"""Find switch jump tables (jr reg fed by lw from lui/addiu table base) and classify all jr/jalr."""
import r5900 as R, collections, sys, json
b=R.load('../../iso/SLUS_210.75'); W=R.words(b)
TEXT_END=0x3a1b80
n=(TEXT_END-0x100000)//4
def s16(x): return x-0x10000 if x&0x8000 else x
def w(a): return W[(a-0x100000)//4]
tables=[]; other=[]
for i in range(n):
    x=W[i]
    if R.decode(x)!='jr' or ((x>>21)&31)==31: continue
    a=0x100000+4*i; reg=(x>>21)&31
    base=None; bound=None; lwoff=0
    for j in range(i-1,max(0,i-14),-1):
        y=W[j]; op=y>>26
        if op==15:  # lui
            hi=(y&0xffff)<<16
            # find addiu with same reg between j and i
            for k in range(j+1,i):
                z=W[k]
                if z>>26==9 and ((z>>21)&31)==((y>>16)&31): base=(hi+s16(z&0xffff))&0xffffffff
                if z>>26==35 and ((z>>21)&31)==((y>>16)&31): base=(hi+s16(z&0xffff))&0xffffffff
            if base is None: base=hi
        if op==11 and bound is None: bound=y&0xffff   # sltiu
        if base is not None and bound is not None: break
    if base is not None and 0x100000<=base<0x47b200 and bound:
        ents=[w(base+4*k) for k in range(bound)]
        tables.append(dict(jr=a,base=base,n=bound,targets=ents))
    else:
        other.append(a)
print('jr(non-ra) total', len(tables)+len(other))
print('switch tables', len(tables), 'entries', sum(t['n'] for t in tables), 'distinct targets', len(set(x for t in tables for x in t['targets'])))
print('table location range', hex(min(t['base'] for t in tables)), hex(max(t['base'] for t in tables)))
bad=[t for t in tables if not all(0x100000<=x<TEXT_END for x in t['targets'])]
print('tables with out-of-text entries', len(bad))
print('other jr', len(other))
json.dump(dict(tables=tables,other=other), open(sys.argv[1]+'/jumptables.json','w'))
for a in other[:40]: print(hex(a), end=' ')
print()
