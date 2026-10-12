#!/usr/bin/env python3
"""Minimal VU (VU0/VU1 micro mode) disassembler + characteriser for the MPG packets
embedded in SLUS_210.75 (DMA RET packets at 0x3a1b90..0x3ac7b0).
usage: hw_vudis.py [--dis N]   (N = packet index to fully disassemble)"""
import struct,sys,collections
ELF='iso/SLUS_210.75'
d=open(ELF,'rb').read()
def va2o(v): return v-0x100000+0x80
def rd(v,n=1): return struct.unpack_from('<%dI'%n,d,va2o(v))
BC='xyzw'
def dest(w): return ''.join(c for i,c in enumerate('xyzw') if w&(1<<(24-i)))
UP={0x1C:'MULq',0x1D:'MAXi',0x1E:'MULi',0x1F:'MINIi',0x20:'ADDq',0x21:'MADDq',0x22:'ADDi',0x23:'MADDi',
    0x24:'SUBq',0x25:'MSUBq',0x26:'SUBi',0x27:'MSUBi',0x28:'ADD',0x29:'MADD',0x2A:'MUL',0x2B:'MAX',
    0x2C:'SUB',0x2D:'MSUB',0x2E:'OPMSUB',0x2F:'MINI'}
for k,n in enumerate(['ADD','SUB','MADD','MSUB','MAX','MINI','MUL']):
    for b in range(4): UP[k*4+b]=n+BC[b]
US={0x10:'ITOF0',0x11:'ITOF4',0x12:'ITOF12',0x13:'ITOF15',0x14:'FTOI0',0x15:'FTOI4',0x16:'FTOI12',0x17:'FTOI15',
    0x1C:'MULAq',0x1D:'ABS',0x1E:'MULAi',0x1F:'CLIP',0x20:'ADDAq',0x21:'MADDAq',0x22:'ADDAi',0x23:'MADDAi',
    0x24:'SUBAq',0x25:'MSUBAq',0x26:'SUBAi',0x27:'MSUBAi',0x28:'ADDA',0x29:'MADDA',0x2A:'MULA',0x2C:'SUBA',
    0x2D:'MSUBA',0x2E:'OPMULA',0x2F:'NOP'}
for k,n in [(0,'ADDA'),(1,'SUBA'),(2,'MADDA'),(3,'MSUBA'),(6,'MULA')]:
    for b in range(4): US[k*4+b]=n+BC[b]
LS={0x30:'MOVE',0x31:'MR32',0x34:'LQI',0x35:'SQI',0x36:'LQD',0x37:'SQD',0x38:'DIV',0x39:'SQRT',0x3A:'RSQRT',
    0x3B:'WAITQ',0x3C:'MTIR',0x3D:'MFIR',0x3E:'ILWR',0x3F:'ISWR',0x40:'RNEXT',0x41:'RGET',0x42:'RINIT',0x43:'RXOR',
    0x64:'MFP',0x68:'XTOP',0x69:'XITOP',0x6C:'XGKICK',0x70:'ESADD',0x71:'ERSADD',0x72:'ELENG',0x73:'ERLENG',
    0x74:'EATANxy',0x75:'EATANxz',0x76:'ESUM',0x78:'ESQRT',0x79:'ERSQRT',0x7A:'ERCPR',0x7B:'WAITP',0x7C:'ESIN',
    0x7D:'EATAN',0x7E:'EEXP'}
LO={0x00:'LQ',0x01:'SQ',0x04:'ILW',0x05:'ISW',0x08:'IADDIU',0x09:'ISUBIU',0x10:'FCEQ',0x11:'FCSET',0x12:'FCAND',
    0x13:'FCOR',0x14:'FSEQ',0x15:'FSSET',0x16:'FSAND',0x17:'FSOR',0x18:'FMEQ',0x1A:'FMAND',0x1B:'FMOR',0x1C:'FCGET',
    0x20:'B',0x21:'BAL',0x24:'JR',0x25:'JALR',0x28:'IBEQ',0x29:'IBNE',0x2C:'IBLTZ',0x2D:'IBGTZ',0x2E:'IBLEZ',0x2F:'IBGEZ'}
LOP={0x30:'IADD',0x31:'ISUB',0x32:'IADDI',0x34:'IAND',0x35:'IOR'}
def upper(u):
    op=u&0x3f
    ft,fs,fd=(u>>16)&31,(u>>11)&31,(u>>6)&31
    if op>=0x3C:
        n=US.get(((u>>6)&31)<<2|(u&3),'?U%x'%u)
        if n=='NOP': return 'nop'
        return f'{n}.{dest(u)} vf{fs},vf{ft}'
    n=UP.get(op,'?U%x'%u)
    return f'{n}.{dest(u)} vf{fd},vf{fs},vf{ft}'
def lower(l,pc):
    if l>>25==0x40:
        op=l&0x3f
        if op>=0x3C:
            n=LS.get(((l>>6)&31)<<2|(l&3),'?L%x'%l)
            if l==0x8000033C: return 'nop'
            return f'{n}.{dest(l)} ft{(l>>16)&31},fs{(l>>11)&31}'
        return f'{LOP.get(op,"?L%x"%l)} vi{(l>>6)&31},vi{(l>>11)&31},vi{(l>>16)&31}'
    op=l>>25
    n=LO.get(op,'?L%08x'%l)
    if n in('B','BAL') or n.startswith('IB'):
        imm=l&0x7ff
        if imm&0x400: imm-=0x800
        return f'{n} vi{(l>>16)&31},vi{(l>>11)&31} ->{pc+1+imm:#x}'
    if n in ('IADDIU','ISUBIU'):
        return f'{n} vi{(l>>16)&31},vi{(l>>11)&31},{(l&0x7ff)|((l>>10)&0x7800):#x}'
    return f'{n}.{dest(l)} vi/vf{(l>>16)&31},{(l&0x7ff):#x}(vi{(l>>11)&31})'
# ---- parse DMA RET packets
pk=[]
pre=(0,0)
va=0x3a1b90
while va<0x3ac7b0:
    t0,=rd(va); qwc=t0&0xffff
    if (t0>>28)&7!=6:
        pre=rd(va+8,2); va+=16
        if not (t0==0x10000000): print(f'  [other tag @{va-16:#x}: {t0:08x} vif {pre[0]:08x} {pre[1]:08x}]'); break
        continue
    vifs=list(rd(va+8,2)); body=va+16; end=body+qwc*16
    items=[]
    p=va+8
    while p<end:
        c,=rd(p); cmd=(c>>24)&0x7f
        if cmd==0x4A:
            num=(c>>16)&0xff or 256; addr=c&0xffff
            items.append(('MPG',addr,num,p+4)); p+=4+num*8
        else:
            items.append(('VIF',c)); p+=4
    items=[('VIF',pre[0],pre[1])]+items; pre=(0,0)
    pk.append((va,qwc,items)); va=end
    while va<0x3ac7b0 and rd(va)[0]==0: va+=16
def analyse(items):
    code={}
    for it in items:
        if it[0]=='MPG':
            _,a,n,p=it
            for i in range(n):
                code[a+i]=rd(p+i*8,2)
    return code
vifname={0:'NOP',1:'STCYCL',2:'OFFSET',3:'BASE',4:'ITOP',5:'STMOD',6:'MSKPATH3',7:'MARK',0x10:'FLUSHE',0x11:'FLUSH',0x13:'FLUSHA',0x14:'MSCAL',0x15:'MSCALF',0x17:'MSCNT'}
want=int(sys.argv[2]) if len(sys.argv)>2 and sys.argv[1]=='--dis' else None
for idx,(va,qwc,items) in enumerate(pk):
    code=analyse(items)
    lo,hi=min(code),max(code)
    cnt=collections.Counter()
    ends=[]; 
    for pc in sorted(code):
        l,u=code[pc]
        if u&(1<<30): ends.append(pc)
        un=upper(u).split('.')[0].split(' ')[0]
        ln='LOI' if u&(1<<31) else lower(l,pc).split('.')[0].split(' ')[0]
        cnt[un]+=1; cnt[ln]+=1
    vifs=[vifname.get((c>>24)&0x7f,'%02x'%((c>>24)&0x7f))+('(%#x)'%(c&0xffff) if (c>>24)&0x7f in (2,3,4,0x14) else '') for t,*r in items if t=='VIF' for c in r if c]
    keys=['XGKICK','XTOP','XITOP','CLIP','DIV','RSQRT','SQRT','ESIN','EATAN','EEXP','ERSQRT','ERCPR','ESQRT','ELENG','ESUM','MFP','ITOF0','ITOF4','FTOI0','FTOI4','FTOI12','FTOI15','ITOF12','ITOF15','ILWR','ISWR','LQI','SQI','LQD','SQD','MULAx','OPMULA','RNEXT','RGET','MAX','MINIi','MAXi','FCAND','FCSET','FMAND','FSAND','FCGET','B','BAL','JR','JALR','IBEQ','IBNE']
    stats=' '.join(f'{k}={cnt[k]}' for k in keys if cnt[k])
    print(f'#{idx:02d} tag@{va:#x} qwc={qwc:#x} vu[{lo:#x}..{hi:#x}] {len(code)} instr, E-bits@{[hex(e) for e in ends]} vif:{vifs}')
    print('     ',stats)
    if want==idx:
        for pc in sorted(code):
            l,u=code[pc]
            fl=''.join(c for i,c in enumerate('IEMDT') if u&(1<<(31-i)))
            lw=f'LOI {l:#010x}' if u&(1<<31) else lower(l,pc)
            print(f'  {pc:04x} {fl:3s} {upper(u):32s} {lw}')
