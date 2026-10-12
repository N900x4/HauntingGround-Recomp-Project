#!/usr/bin/env python3
"""Find VU microprograms: VIF MPG vifcodes (cmd 0x4A) whose payload looks like VU code,
plus clusters of the VU NOP pair (lower 0x8000033C / upper 0x000002FF).
usage: hw_vu_find.py FILE [base_va] [file_off_of_base]"""
import sys,struct
NOPL=0x8000033C; NOPU=0x000002FF
def scan(path, base=0, off0=0, quiet=False):
    d=open(path,'rb').read()
    n=len(d)//4
    w=struct.unpack_from('<%dI'%n,d)
    res=[]
    # MPG vifcode: cmd byte 0x4A (bit7 = irq ok -> 0x4A or 0xCA), num bits 16-23 (0=256), imm = load addr (in dwords)
    for i in range(n-2):
        c=w[i]
        if (c>>24)&0x7f!=0x4A: continue
        num=(c>>16)&0xff or 256
        addr=c&0xffff
        if addr>=2048: continue
        # MPG payload must be 8-byte aligned: vifcode at odd word index
        if i%2!=1: continue
        j=i+1
        if j+num*2>n: continue
        pl=w[j:j+num*2]
        lows=pl[0::2]; ups=pl[1::2]
        nops=sum(1 for x in lows if x==NOPL)+sum(1 for x in ups if x==NOPU)
        # upper ops: bits 0-5 opcode; flag bits 27-31 (I,E,M,D,T); bits 25-26 dest? Score E bit presence
        ebit=sum(1 for x in ups if x&(1<<30))
        if nops*4 < num:   # require >=25% nop slots
            continue
        res.append((i*4, c, num, addr, nops, ebit))
    if not quiet:
        for (o,c,num,addr,nops,ebit) in res:
            va = base + o - off0 if base else o
            print(f"{path.split('/')[-1]} off=0x{o:x} va=0x{va:x} vif=0x{c:08x} num={num} vuaddr=0x{addr:x} ({addr*8:#x}B) nops={nops} ebits={ebit}")
    return res,w
if __name__=='__main__':
    p=sys.argv[1]
    base=int(sys.argv[2],0) if len(sys.argv)>2 else 0
    off0=int(sys.argv[3],0) if len(sys.argv)>3 else 0
    scan(p,base,off0)
