#!/usr/bin/env python3
"""dis.py START END : disassemble SLUS_210.75 VA range (capstone MIPS64 LE; R5900 ops shown as .word)"""
import sys, capstone
B=open('iso/SLUS_210.75','rb').read()
s,e=int(sys.argv[1],16),int(sys.argv[2],16)
md=capstone.Cs(capstone.CS_ARCH_MIPS, capstone.CS_MODE_MIPS64|capstone.CS_MODE_LITTLE_ENDIAN)
for a in range(s,e,4):
    w=B[a-0x100000+0x80:a-0x100000+0x84]
    ins=list(md.disasm(w,a))
    print('%08x: %s'%(a, (ins[0].mnemonic+' '+ins[0].op_str) if ins else '.word 0x%08x'%int.from_bytes(w,'little')))
