import struct, capstone, sys
path, base, lo, hi = sys.argv[1], int(sys.argv[2],16), int(sys.argv[3],16), int(sys.argv[4],16)
d=open(path,'rb').read()
phoff=struct.unpack_from('<I',d,28)[0]; phnum=struct.unpack_from('<H',d,44)[0]
off=[struct.unpack_from('<I',d,phoff+32*i+4)[0] for i in range(phnum) if struct.unpack_from('<I',d,phoff+32*i)[0]==1][0]
md=capstone.Cs(capstone.CS_ARCH_MIPS, capstone.CS_MODE_MIPS32|capstone.CS_MODE_LITTLE_ENDIAN)
for a in range(lo, hi, 4):
    w=d[off+a-base:off+a-base+4]; ins=list(md.disasm(w,a-base))
    print('%08x (+%05x): %s'%(a, a-base, (ins[0].mnemonic+' '+ins[0].op_str) if ins else '.word %08x'%struct.unpack('<I',w)[0]))
