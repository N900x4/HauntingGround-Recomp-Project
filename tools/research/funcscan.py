"""Independent function-boundary estimate for the main text (0x100000-0x3a1b80) + sinit island."""
import r5900 as R, sys, collections
b=R.load('../../iso/SLUS_210.75'); W=R.words(b)
TEXT=(0x100000,0x3a1b80)
def w(a): return W[(a-0x100000)//4]
def s16(x): return x-0x10000 if x&0x8000 else x
n=(TEXT[1]-TEXT[0])//4
jrra=[0x100000+4*i for i in range(n) if W[i]==0x03e00008]
prol=[0x100000+4*i for i in range(n) if (W[i]>>16)==0x27bd and (W[i]&0x8000)]
jal=set(((W[i]&0x3ffffff)<<2) for i in range(n) if W[i]>>26==3)
# branch targets (to exclude from "after jr ra" starts)
bt=set()
for i in range(n):
    x=W[i]; m=R.decode(x)
    if m in R.BRANCH: bt.add(0x100000+4*i+4+4*s16(x&0xffff))
    elif m=='j': bt.add((x&0x3ffffff)<<2)
# data pointers into text
dp=set()
for i in range(n, len(W)):
    v=W[i]
    if TEXT[0]<=v<TEXT[1] and v%4==0: dp.add(v)
after=set()
for a in jrra:
    c=a+8
    while c<TEXT[1] and w(c)==0: c+=4
    if c<TEXT[1] and c not in bt: after.add(c)
starts=jal|after|(dp & set(prol))
# also: prologue instructions that are first non-nop after a previous terminator j (tail call)
for i in range(1,n):
    pass
print('jr ra', len(jrra)); print('addiu sp,sp,-N', len(prol)); print('distinct jal targets', len(jal))
print('starts after jr ra (not branch targets)', len(after)); print('data words pointing into text', len(dp), 'of which at prologue', len(dp&set(prol)))
print('union estimate', len(starts))
print('data ptrs to text that are jal targets', len(dp&jal), 'or after-jr starts', len(dp&after))
gh=set(int(l.split('\t')[0],16) for l in open(sys.argv[1]+'/ghfuncs.tsv'))
ghT=set(x for x in gh if x<TEXT[1])
print('ghidra in text', len(ghT), 'mine-not-ghidra', len(starts-ghT), 'ghidra-not-mine', len(ghT-starts))
open(sys.argv[1]+'/mystarts.txt','w').write('\n'.join(hex(x) for x in sorted(starts)))
open(sys.argv[1]+'/dataptrs.txt','w').write('\n'.join(hex(x) for x in sorted(dp)))
import json
J=json.load(open(sys.argv[1]+'/jumptables.json'))
jtt=set(x for t in J['tables'] for x in t['targets'])
jtloc=set(t['base']+4*k for t in J['tables'] for k in range(t['n']))
dp2=set()
for i in range(n, len(W)):
    a=0x100000+4*i
    if a in jtloc: continue
    v=W[i]
    if TEXT[0]<=v<TEXT[1] and v%4==0: dp2.add(v)
after2=after-jtt
starts2=jal|dp2|after2
print('refined: after-jr minus jumptable targets', len(after2), 'non-jumptable data ptrs', len(dp2))
print('refined union', len(starts2), 'ghidra', len(ghT), 'both', len(starts2&ghT), 'mine-only', len(starts2-ghT), 'ghidra-only', len(ghT-starts2))
mo=sorted(starts2-ghT)
# how many mine-only are inside a ghidra function body?
gb=[(int(l.split('\t')[1],16),int(l.split('\t')[2],16)) for l in open(sys.argv[1]+'/ghfuncs.tsv')]
gb.sort(); import bisect
lo=[x[0] for x in gb]
inside=sum(1 for a in mo if (lambda k: k>=0 and gb[k][0]<=a<=gb[k][1])(bisect.bisect_right(lo,a)-1))
print('mine-only inside ghidra bodies', inside, 'outside any ghidra function', len(mo)-inside)
covered=sum(int(l.split('\t')[3]) for l in open(sys.argv[1]+'/ghfuncs.tsv') if int(l.split('\t')[0],16)<TEXT[1])
print('ghidra body bytes in text', covered, 'of', TEXT[1]-TEXT[0])
open(sys.argv[1]+'/mystarts.txt','w').write('\n'.join(hex(x) for x in sorted(starts2)))
open(sys.argv[1]+'/mineonly.txt','w').write('\n'.join(hex(x) for x in mo))
# --- strict estimate: Ghidra ∪ jal targets ∪ (non-jumptable data pointers whose target follows a terminator) ∪ ctor table
def after_term(t):
    c=t-4
    while c>=TEXT[0] and w(c)==0: c-=4
    # c is last non-nop; terminator if c-4 is jr/j (c is delay slot) or c itself is delay slot of jr ra with nop
    for d in (c-4, c):
        m=R.decode(w(d)) if d>=TEXT[0] else ''
        if m in ('jr','j'): return True
    return False
dpt=set(x for x in dp2 if after_term(x))
strict=ghT|jal|dpt
print('strict: ghidra', len(ghT), '+jal', len(jal-ghT), '+dataptr-after-terminator', len(dpt-ghT-jal), '=', len(strict))
open(sys.argv[1]+'/mystarts.txt','w').write('\n'.join(hex(x) for x in sorted(strict)))
