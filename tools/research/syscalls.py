"""List syscall wrappers (li v1,N; syscall) with Ghidra names and jal call counts (game vs lib)."""
import r5900 as R, collections, sys
S=sys.argv[1]
b=R.load('../../iso/SLUS_210.75'); W=R.words(b)
n=(0x3a1b80-0x100000)//4
names={int(l.split('\t')[0],16):l.split('\t')[4] for l in open(S+'/ghfuncs.tsv')}
G=[(0x11c770,0x1c2d38),(0x1efb40,0x226808),(0x25c440,0x26bcc0),(0x277e90,0x29ff10),(0x2a7630,0x319f48),(0x31caa0,0x3a1b80)]
def isgame(a): return any(lo<=a<hi for lo,hi in G)
calls=collections.defaultdict(lambda:[0,0])
for i in range(n):
    w=W[i]
    if w>>26==3:
        t=(w&0x3ffffff)<<2; calls[t][0 if isgame(0x100000+4*i) else 1]+=1
rows=[]
for i in range(n):
    if (W[i]&0xfc00003f)!=0xc: continue
    a=0x100000+4*i; num=None
    for j in range(i-1,i-4,-1):
        w=W[j]
        if (w>>16)==0x2403: num=w&0xffff; num = num-0x10000 if num&0x8000 else num; break
    start=a-4 if (W[i-1]>>16)==0x2403 else a
    rows.append((a,num,names.get(start,names.get(a-8,'?')),calls[start]))
print('syscall sites', len(rows))
print('| addr | num | ghidra name | jal callers game/lib |'); print('|---|---|---|---|')
for a,num,nm,c in rows: print(f'| {a:#x} | {num} | {nm} | {c[0]}/{c[1]} |')
