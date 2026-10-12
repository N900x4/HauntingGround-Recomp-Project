#!/usr/bin/env python3
"""Find lui/addiu(ori/lw/sw) references to target VAs in SLUS_210.75. xref.py 0xADDR..."""
import sys, struct
B=open('iso/SLUS_210.75','rb').read(); base=0x100000; off=0x80
code=B[off:off+0x37b200]; n=len(code)//4
W=struct.unpack('<%dI'%n, code)
targets={int(a,16) for a in sys.argv[1:]}
hi={}
for i,w in enumerate(W):
    op=w>>26; rs=(w>>21)&31; rt=(w>>16)&31; imm=w&0xffff
    if op==0x0f: hi[rt]=(imm<<16, i); continue
    if op in (0x09,0x0d,0x23,0x2b,0x20,0x24,0x21,0x25,0x28,0x29,0x31,0x39,0x1e,0x1f,0x37,0x3f) and rs in hi and i-hi[rs][1]<64:
        v = hi[rs][0] + (imm if op==0x0d else (imm-0x10000 if imm&0x8000 else imm))
        if v&0xffffffff in targets: print(hex(v), 'at', hex(base+i*4), 'lui@', hex(base+hi[rs][1]*4))
