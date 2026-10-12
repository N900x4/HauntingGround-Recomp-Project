#!/usr/bin/env python3
"""callargs.py START END : for each jal in range, constants put in a0-a3 by the 6 preceding instrs (+delay slot)."""
import sys, struct, collections
B=open('iso/SLUS_210.75','rb').read(); code=B[0x80:0x80+0x37b200]
W=struct.unpack('<%dI'%(len(code)//4),code)
s,e=(int(x,16) for x in sys.argv[1:3])
res=collections.defaultdict(list)
for i in range((s-0x100000)//4,(e-0x100000)//4):
    w=W[i]
    if w>>26!=3: continue
    tgt=(w&0x3ffffff)<<2
    args={}
    for j in list(range(i-6,i))+[i+1]:
        x=W[j]; op=x>>26; rs=(x>>21)&31; rt=(x>>16)&31; imm=x&0xffff
        if op in (9,0xd) and rs==0 and 4<=rt<=7: args['a%d'%(rt-4)]=imm if op==0xd or imm<0x8000 else imm-0x10000
    res[tgt].append((0x100000+i*4,args))
for t,v in sorted(res.items(), key=lambda kv:-len(kv[1])):
    print(hex(t), len(v), ' '.join('%x:%s'%(a, ','.join('%s=%#x'%kv for kv in sorted(ar.items()))) for a,ar in v[:12]))
