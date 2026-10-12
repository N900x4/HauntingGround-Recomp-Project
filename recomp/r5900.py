"""Minimal R5900 (PS2 EE) instruction classifier/decoder used for the feasibility study.
decode(word) -> (mnemonic, fields dict). Covers the full EE ISA at mnemonic level
(MIPS III subset + EE extensions: MMI, COP0, COP1 EE-FPU, COP2 macro VU0)."""
import struct

BASE = 0x100000
def load(path):
    d = open(path, 'rb').read()
    return d[0x80:0x80 + 0x37B200]

SPECIAL = {0:'sll',2:'srl',3:'sra',4:'sllv',6:'srlv',7:'srav',8:'jr',9:'jalr',10:'movz',11:'movn',
 12:'syscall',13:'break',15:'sync',16:'mfhi',17:'mthi',18:'mflo',19:'mtlo',20:'dsllv',22:'dsrlv',23:'dsrav',
 24:'mult',25:'multu',26:'div',27:'divu',32:'add',33:'addu',34:'sub',35:'subu',36:'and',37:'or',38:'xor',39:'nor',
 40:'mfsa',41:'mtsa',42:'slt',43:'sltu',44:'dadd',45:'daddu',46:'dsub',47:'dsubu',48:'tge',49:'tgeu',50:'tlt',51:'tltu',
 52:'teq',54:'tne',56:'dsll',58:'dsrl',59:'dsra',60:'dsll32',62:'dsrl32',63:'dsra32'}
REGIMM = {0:'bltz',1:'bgez',2:'bltzl',3:'bgezl',8:'tgei',9:'tgeiu',10:'tlti',11:'tltiu',12:'teqi',14:'tnei',
 16:'bltzal',17:'bgezal',18:'bltzall',19:'bgezall',24:'mtsab',25:'mtsah'}
PRIMARY = {2:'j',3:'jal',4:'beq',5:'bne',6:'blez',7:'bgtz',8:'addi',9:'addiu',10:'slti',11:'sltiu',12:'andi',13:'ori',
 14:'xori',15:'lui',20:'beql',21:'bnel',22:'blezl',23:'bgtzl',24:'daddi',25:'daddiu',26:'ldl',27:'ldr',30:'lq',31:'sq',
 32:'lb',33:'lh',34:'lwl',35:'lw',36:'lbu',37:'lhu',38:'lwr',39:'lwu',40:'sb',41:'sh',42:'swl',43:'sw',44:'sdl',45:'sdr',
 46:'swr',47:'cache',49:'lwc1',51:'pref',54:'lqc2',55:'ld',57:'swc1',62:'sqc2',63:'sd'}
MMI = {0:'madd',1:'maddu',4:'plzcw',16:'mfhi1',17:'mthi1',18:'mflo1',19:'mtlo1',24:'mult1',25:'multu1',26:'div1',
 27:'divu1',32:'madd1',33:'maddu1',48:'pmfhl',49:'pmthl',52:'psllh',54:'psrlh',55:'psrah',60:'psllw',62:'psrlw',63:'psraw'}
MMI0 = {0:'paddw',1:'psubw',2:'pcgtw',3:'pmaxw',4:'paddh',5:'psubh',6:'pcgth',7:'pmaxh',8:'paddb',9:'psubb',10:'pcgtb',
 16:'paddsw',17:'psubsw',18:'pextlw',19:'ppacw',20:'paddsh',21:'psubsh',22:'pextlh',23:'ppach',24:'paddsb',25:'psubsb',
 26:'pextlb',27:'ppacb',30:'pext5',31:'ppac5'}
MMI1 = {1:'pabsw',2:'pceqw',3:'pminw',4:'padsbh',5:'pabsh',6:'pceqh',7:'pminh',10:'pceqb',16:'padduw',17:'psubuw',
 18:'pextuw',20:'padduh',21:'psubuh',22:'pextuh',24:'paddub',25:'psubub',26:'pextub',27:'qfsrv'}
MMI2 = {0:'pmaddw',2:'psllvw',3:'psrlvw',4:'pmsubw',8:'pmfhi',9:'pmflo',10:'pinth',12:'pmultw',13:'pdivw',14:'pcpyld',
 16:'pmaddh',17:'phmadh',18:'pand',19:'pxor',20:'pmsubh',21:'phmsbh',26:'pexeh',27:'prevh',28:'pmulth',29:'pdivbw',
 30:'pexew',31:'prot3w'}
MMI3 = {0:'pmadduw',3:'psravw',8:'pmthi',9:'pmtlo',10:'pinteh',12:'pmultuw',13:'pdivuw',14:'pcpyud',18:'por',19:'pnor',
 26:'pexch',27:'pcpyh',30:'pexcw'}
COP1S = {0:'add.s',1:'sub.s',2:'mul.s',3:'div.s',4:'sqrt.s',5:'abs.s',6:'mov.s',7:'neg.s',22:'rsqrt.s',24:'adda.s',
 25:'suba.s',26:'mula.s',28:'madd.s',29:'msub.s',30:'madda.s',31:'msuba.s',36:'cvt.w.s',40:'max.s',41:'min.s',
 48:'c.f.s',50:'c.eq.s',52:'c.lt.s',54:'c.le.s'}
VU1 = {28:'vmulq',29:'vmaxi',30:'vmuli',31:'vminii',32:'vaddq',33:'vmaddq',34:'vaddi',35:'vmaddi',36:'vsubq',37:'vmsubq',
 38:'vsubi',39:'vmsubi',40:'vadd',41:'vmadd',42:'vmul',43:'vmax',44:'vsub',45:'vmsub',46:'vopmsub',47:'vmini',
 48:'viadd',49:'visub',50:'viaddi',52:'viand',53:'vior',56:'vcallms',57:'vcallmsr'}
for i,n in enumerate(['vaddbc','vsubbc','vmaddbc','vmsubbc','vmaxbc','vminibc','vmulbc']):
    for k in range(4): VU1[i*4+k] = n
VU2 = {28:'vmulaq',29:'vabs',30:'vmulai',31:'vclipw',32:'vaddaq',33:'vmaddaq',34:'vaddai',35:'vmaddai',36:'vsubaq',
 37:'vmsubaq',38:'vsubai',39:'vmsubai',40:'vadda',41:'vmadda',42:'vmula',44:'vsuba',45:'vmsuba',46:'vopmula',47:'vnop',
 48:'vmove',49:'vmr32',52:'vlqi',53:'vsqi',54:'vlqd',55:'vsqd',56:'vdiv',57:'vsqrt',58:'vrsqrt',59:'vwaitq',60:'vmtir',
 61:'vmfir',62:'vilwr',63:'viswr',64:'vrnext',65:'vrget',66:'vrinit',67:'vrxor',16:'vitof0',17:'vitof4',18:'vitof12',
 19:'vitof15',20:'vftoi0',21:'vftoi4',22:'vftoi12',23:'vftoi15'}
for i,n in enumerate(['vaddabc','vsubabc','vmaddabc','vmsubabc']):
    for k in range(4): VU2[i*4+k] = n
for k in range(4): VU2[24+k] = 'vmulabc'

def decode(w):
    op = w >> 26; rs = (w >> 21) & 31; rt = (w >> 16) & 31; rd = (w >> 11) & 31; sa = (w >> 6) & 31; fn = w & 63
    if w == 0: return 'nop'
    if op == 0:
        m = SPECIAL.get(fn)
        if m in ('mult', 'multu') and rd: m += '3'   # EE three-operand form
        return m or '?special'
    if op == 1: return REGIMM.get(rt, '?regimm')
    if op == 28:
        if fn == 8: return MMI0.get(sa, '?mmi0')
        if fn == 40: return MMI1.get(sa, '?mmi1')
        if fn == 9: return MMI2.get(sa, '?mmi2')
        if fn == 41: return MMI3.get(sa, '?mmi3')
        m = MMI.get(fn, '?mmi')
        if m in ('mult1', 'multu1') and rd: m += '_3'
        return m
    if op == 16:
        if rs == 0: return 'mfc0'
        if rs == 4: return 'mtc0'
        if rs == 8: return 'bc0'
        if rs == 16: return {1:'tlbr',2:'tlbwi',6:'tlbwr',8:'tlbp',24:'eret',56:'ei',57:'di'}.get(fn, '?cop0')
        return '?cop0'
    if op == 17:
        if rs == 0: return 'mfc1'
        if rs == 2: return 'cfc1'
        if rs == 4: return 'mtc1'
        if rs == 6: return 'ctc1'
        if rs == 8: return ['bc1f','bc1t','bc1fl','bc1tl'][rt & 3]
        if rs == 16: return COP1S.get(fn, '?cop1s')
        if rs == 20: return 'cvt.s.w' if fn == 32 else '?cop1w'
        return '?cop1'
    if op == 18:
        if rs == 1: return 'qmfc2'
        if rs == 2: return 'cfc2'
        if rs == 5: return 'qmtc2'
        if rs == 6: return 'ctc2'
        if rs == 8: return ['bc2f','bc2t','bc2fl','bc2tl'][rt & 3]
        if rs >= 16:
            if fn >= 60: return VU2.get((fn & 3) | (sa << 2), '?vu2')
            return VU1.get(fn, '?vu1')
        return '?cop2'
    return PRIMARY.get(op, '?op%d' % op)

def words(blob):
    return struct.unpack('<%dI' % (len(blob) // 4), blob[:len(blob) // 4 * 4])

BRANCH = {'beq','bne','blez','bgtz','bltz','bgez','bltzal','bgezal','beql','bnel','blezl','bgtzl','bltzl','bgezl',
 'bltzall','bgezall','bc1f','bc1t','bc1fl','bc1tl','bc2f','bc2t','bc2fl','bc2tl','bc0'}
LIKELY = {'beql','bnel','blezl','bgtzl','bltzl','bgezl','bltzall','bgezall','bc1fl','bc1tl','bc2fl','bc2tl'}
JUMPS = {'j','jal','jr','jalr'}
