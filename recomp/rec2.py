#!/usr/bin/env python3
"""Tiny static recompiler PoC for the PS2 EE (R5900), Haunting Ground SLUS_210.75.

Translates whole functions to C in the classic static-recompiler style:
  * one C statement (block) per MIPS instruction, a label per instruction address
  * register context struct (128-bit GPRs, HI/LO + HI1/LO1, FPU regs + ACC + condition flag)
  * memory through accessor functions (rd32/wr32/...), RAM masked to 32 MB
  * delay slots: branch condition / jump target is evaluated first, then the delay-slot
    instruction is emitted, then the transfer; branch-likely nullifies the slot when not taken
  * jal -> direct C call when the callee is in the same unit, else dispatcher rec_call();
    jalr / jr <reg> (virtual tail call) -> rec_call(); jr <reg> with a known switch table -> C switch
Unsupported opcodes become a call to rec_unimpl() (interpreter fallback in a real project).

usage: rec.py out.c 0xADDR[:0xEND] ...   (END defaults to Ghidra function end if ghfuncs.tsv given via GHFUNCS)
"""
import sys, os, json, struct
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import r5900 as R

def s16(x): return x - 0x10000 if x & 0x8000 else x

GPRN = ['zero','at','v0','v1','a0','a1','a2','a3','t0','t1','t2','t3','t4','t5','t6','t7',
        's0','s1','s2','s3','s4','s5','s6','s7','t8','t9','k0','k1','gp','sp','fp','ra']

# ---------------------------------------------------------------- semantics
def g(r):  return f'ctx->r[{r}]'
def U64(r): return '0ULL' if r == 0 else f'ctx->r[{r}].u64[0]'
def S64(r): return '0LL' if r == 0 else f'ctx->r[{r}].s64[0]'
def U32(r): return '0U' if r == 0 else f'ctx->r[{r}].u32[0]'
def S32(r): return '0' if r == 0 else f'ctx->r[{r}].s32[0]'
def W64(r, e):        # write lower 64 bits (EE: upper 64 bits preserved by non-MMI ops)
    return '' if r == 0 else f'ctx->r[{r}].u64[0] = (uint64_t)({e});'
def W32S(r, e):       # 32-bit result, sign-extended to 64
    return '' if r == 0 else f'ctx->r[{r}].s64[0] = (int64_t)(int32_t)(uint32_t)({e});'
def F(n): return f'ctx->f[{n}]'

SUPPORTED = set()
def sem(ins, a):
    """Return C for a non-control-transfer instruction (or None if unsupported)."""
    w = ins; op = w >> 26; rs = (w >> 21) & 31; rt = (w >> 16) & 31; rd = (w >> 11) & 31
    sa = (w >> 6) & 31; imm = w & 0xffff; si = s16(imm); m = R.decode(w)
    ea = f'(uint32_t)({U32(rs)} + {si})'
    fs = rd; ft = rt; fd = sa
    T = {
     'nop': '', 'sync': '/*sync*/', 'pref': '/*pref*/', 'ei': 'ee_ei(ctx);', 'di': 'ee_di(ctx);', 'cache': '/*cache*/',
     'sll': W32S(rd, f'{U32(rt)} << {sa}'), 'srl': W32S(rd, f'{U32(rt)} >> {sa}'),
     'sra': W32S(rd, f'{S32(rt)} >> {sa}'),
     'sllv': W32S(rd, f'{U32(rt)} << ({U32(rs)} & 31)'), 'srlv': W32S(rd, f'{U32(rt)} >> ({U32(rs)} & 31)'),
     'srav': W32S(rd, f'{S32(rt)} >> ({U32(rs)} & 31)'),
     'dsll': W64(rd, f'{U64(rt)} << {sa}'), 'dsrl': W64(rd, f'{U64(rt)} >> {sa}'), 'dsra': W64(rd, f'{S64(rt)} >> {sa}'),
     'dsll32': W64(rd, f'{U64(rt)} << {sa+32}'), 'dsrl32': W64(rd, f'{U64(rt)} >> {sa+32}'),
     'dsra32': W64(rd, f'{S64(rt)} >> {sa+32}'),
     'dsllv': W64(rd, f'{U64(rt)} << ({U32(rs)} & 63)'), 'dsrlv': W64(rd, f'{U64(rt)} >> ({U32(rs)} & 63)'),
     'dsrav': W64(rd, f'{S64(rt)} >> ({U32(rs)} & 63)'),
     'addu': W32S(rd, f'{U32(rs)} + {U32(rt)}'), 'add': W32S(rd, f'{U32(rs)} + {U32(rt)}'),
     'subu': W32S(rd, f'{U32(rs)} - {U32(rt)}'), 'sub': W32S(rd, f'{U32(rs)} - {U32(rt)}'),
     'daddu': W64(rd, f'{U64(rs)} + {U64(rt)}'), 'dadd': W64(rd, f'{U64(rs)} + {U64(rt)}'),
     'dsubu': W64(rd, f'{U64(rs)} - {U64(rt)}'), 'dsub': W64(rd, f'{U64(rs)} - {U64(rt)}'),
     'and': W64(rd, f'{U64(rs)} & {U64(rt)}'), 'or': W64(rd, f'{U64(rs)} | {U64(rt)}'),
     'xor': W64(rd, f'{U64(rs)} ^ {U64(rt)}'), 'nor': W64(rd, f'~({U64(rs)} | {U64(rt)})'),
     'slt': W64(rd, f'{S64(rs)} < {S64(rt)}'), 'sltu': W64(rd, f'{U64(rs)} < {U64(rt)}'),
     'movz': '' if rd == 0 else f'if ({U64(rt)} == 0) {g(rd)}.u64[0] = {U64(rs)};',
     'movn': '' if rd == 0 else f'if ({U64(rt)} != 0) {g(rd)}.u64[0] = {U64(rs)};',
     'mfhi': W64(rd, 'ctx->hi'), 'mflo': W64(rd, 'ctx->lo'), 'mthi': f'ctx->hi = {U64(rs)};', 'mtlo': f'ctx->lo = {U64(rs)};',
     'mfhi1': W64(rd, 'ctx->hi1'), 'mflo1': W64(rd, 'ctx->lo1'), 'mthi1': f'ctx->hi1 = {U64(rs)};', 'mtlo1': f'ctx->lo1 = {U64(rs)};',
     'mult': f'ee_mult(ctx, 0, {rd}, {S32(rs)}, {S32(rt)});', 'mult3': f'ee_mult(ctx, 0, {rd}, {S32(rs)}, {S32(rt)});',
     'multu': f'ee_multu(ctx, 0, {rd}, {U32(rs)}, {U32(rt)});', 'multu3': f'ee_multu(ctx, 0, {rd}, {U32(rs)}, {U32(rt)});',
     'mult1': f'ee_mult(ctx, 1, {rd}, {S32(rs)}, {S32(rt)});', 'mult1_3': f'ee_mult(ctx, 1, {rd}, {S32(rs)}, {S32(rt)});',
     'multu1': f'ee_multu(ctx, 1, {rd}, {U32(rs)}, {U32(rt)});', 'multu1_3': f'ee_multu(ctx, 1, {rd}, {U32(rs)}, {U32(rt)});',
     'madd': f'ee_madd(ctx, 0, {rd}, {S32(rs)}, {S32(rt)});', 'madd1': f'ee_madd(ctx, 1, {rd}, {S32(rs)}, {S32(rt)});',
     'div': f'ee_div(ctx, 0, {S32(rs)}, {S32(rt)});', 'divu': f'ee_divu(ctx, 0, {U32(rs)}, {U32(rt)});',
     'div1': f'ee_div(ctx, 1, {S32(rs)}, {S32(rt)});', 'divu1': f'ee_divu(ctx, 1, {U32(rs)}, {U32(rt)});',
     'addiu': W32S(rt, f'{U32(rs)} + {si}'), 'addi': W32S(rt, f'{U32(rs)} + {si}'),
     'daddiu': W64(rt, f'{U64(rs)} + (int64_t){si}'), 'daddi': W64(rt, f'{U64(rs)} + (int64_t){si}'),
     'slti': W64(rt, f'{S64(rs)} < (int64_t){si}'), 'sltiu': W64(rt, f'{U64(rs)} < (uint64_t)(int64_t){si}'),
     'andi': W64(rt, f'{U64(rs)} & {imm}u'), 'ori': W64(rt, f'{U64(rs)} | {imm}u'), 'xori': W64(rt, f'{U64(rs)} ^ {imm}u'),
     'lui': W64(rt, f'(int64_t)(int32_t){(imm << 16)}u'),
     'lb': W64(rt, f'(int64_t)(int8_t)rd8({ea})'), 'lbu': W64(rt, f'rd8({ea})'),
     'lh': W64(rt, f'(int64_t)(int16_t)rd16({ea})'), 'lhu': W64(rt, f'rd16({ea})'),
     'lw': W64(rt, f'(int64_t)(int32_t)rd32({ea})'), 'lwu': W64(rt, f'rd32({ea})'), 'ld': W64(rt, f'rd64({ea})'),
     'lq': '' if rt == 0 else f'rd128({ea}, &{g(rt)});',
     'sb': f'wr8({ea}, (uint8_t){U32(rt)});', 'sh': f'wr16({ea}, (uint16_t){U32(rt)});',
     'sw': f'wr32({ea}, {U32(rt)});', 'sd': f'wr64({ea}, {U64(rt)});',
     'sq': f'wr128({ea}, &{g(rt)});' if rt else f'{{ GPR z = {{0}}; wr128({ea}, &z); }}',
     'lwl': '' if rt == 0 else f'ee_lwl(ctx, {rt}, {ea});', 'lwr': '' if rt == 0 else f'ee_lwr(ctx, {rt}, {ea});',
     'ldl': '' if rt == 0 else f'ee_ldl(ctx, {rt}, {ea});', 'ldr': '' if rt == 0 else f'ee_ldr(ctx, {rt}, {ea});',
     'swl': f'ee_swl(ctx, {rt}, {ea});', 'swr': f'ee_swr(ctx, {rt}, {ea});',
     'sdl': f'ee_sdl(ctx, {rt}, {ea});', 'sdr': f'ee_sdr(ctx, {rt}, {ea});',
     # ---- COP1 (EE FPU: single precision only, ACC register, no IEEE specials -> fpu_clamp)
     'lwc1': f'ctx->fi[{rt}] = rd32({ea});', 'swc1': f'wr32({ea}, ctx->fi[{rt}]);',
     'mtc1': f'ctx->fi[{fs}] = {U32(rt)};', 'mfc1': W64(rt, f'(int64_t)(int32_t)ctx->fi[{fs}]'),
     'ctc1': f'ctx->fcr31 = {U32(rt)};', 'cfc1': W64(rt, '(int64_t)(int32_t)ctx->fcr31' if rd == 31 else '0x2e00'),
     'add.s': f'{F(fd)} = fpu_clamp({F(fs)} + {F(ft)});', 'sub.s': f'{F(fd)} = fpu_clamp({F(fs)} - {F(ft)});',
     'mul.s': f'{F(fd)} = fpu_clamp({F(fs)} * {F(ft)});', 'div.s': f'{F(fd)} = ee_divs({F(fs)}, {F(ft)});',
     'sqrt.s': f'{F(fd)} = sqrtf(fabsf({F(ft)}));', 'abs.s': f'{F(fd)} = fabsf({F(fs)});',
     'neg.s': f'{F(fd)} = -{F(fs)};', 'mov.s': f'{F(fd)} = {F(fs)};',
     'rsqrt.s': f'{F(fd)} = ee_divs({F(fs)}, sqrtf(fabsf({F(ft)})));',
     'adda.s': f'ctx->acc = fpu_clamp({F(fs)} + {F(ft)});', 'suba.s': f'ctx->acc = fpu_clamp({F(fs)} - {F(ft)});',
     'mula.s': f'ctx->acc = fpu_clamp({F(fs)} * {F(ft)});',
     'madd.s': f'{F(fd)} = fpu_clamp(ctx->acc + fpu_clamp({F(fs)} * {F(ft)}));',
     'msub.s': f'{F(fd)} = fpu_clamp(ctx->acc - fpu_clamp({F(fs)} * {F(ft)}));',
     'madda.s': f'ctx->acc = fpu_clamp(ctx->acc + fpu_clamp({F(fs)} * {F(ft)}));',
     'msuba.s': f'ctx->acc = fpu_clamp(ctx->acc - fpu_clamp({F(fs)} * {F(ft)}));',
     'max.s': f'{F(fd)} = fmaxf({F(fs)}, {F(ft)});', 'min.s': f'{F(fd)} = fminf({F(fs)}, {F(ft)});',
     'cvt.s.w': f'{F(fd)} = (float)(int32_t)ctx->fi[{fs}];', 'cvt.w.s': f'ctx->fi[{fd}] = (uint32_t)ee_cvtw({F(fs)});',
     'c.f.s': 'ctx->fcc = 0;', 'c.eq.s': f'ctx->fcc = {F(fs)} == {F(ft)};',
     'c.lt.s': f'ctx->fcc = {F(fs)} < {F(ft)};', 'c.le.s': f'ctx->fcc = {F(fs)} <= {F(ft)};',
     # ---- MMI subset (128-bit)
     'pcpyld': '' if rd == 0 else f'{{ uint64_t l = {g(rt)}.u64[0], h = {g(rs)}.u64[0]; {g(rd)}.u64[0] = l; {g(rd)}.u64[1] = h; }}',
     'pcpyud': '' if rd == 0 else f'{{ uint64_t l = {g(rs)}.u64[1], h = {g(rt)}.u64[1]; {g(rd)}.u64[0] = l; {g(rd)}.u64[1] = h; }}',
     'por': '' if rd == 0 else f'{g(rd)}.u64[0] = {g(rs)}.u64[0] | {g(rt)}.u64[0]; {g(rd)}.u64[1] = {g(rs)}.u64[1] | {g(rt)}.u64[1];',
     'pand': '' if rd == 0 else f'{g(rd)}.u64[0] = {g(rs)}.u64[0] & {g(rt)}.u64[0]; {g(rd)}.u64[1] = {g(rs)}.u64[1] & {g(rt)}.u64[1];',
     'pxor': '' if rd == 0 else f'{g(rd)}.u64[0] = {g(rs)}.u64[0] ^ {g(rt)}.u64[0]; {g(rd)}.u64[1] = {g(rs)}.u64[1] ^ {g(rt)}.u64[1];',
     'pnor': '' if rd == 0 else f'{g(rd)}.u64[0] = ~({g(rs)}.u64[0] | {g(rt)}.u64[0]); {g(rd)}.u64[1] = ~({g(rs)}.u64[1] | {g(rt)}.u64[1]);',
     'paddw': '' if rd == 0 else f'{{ GPR t; for (int i = 0; i < 4; i++) t.u32[i] = {g(rs)}.u32[i] + {g(rt)}.u32[i]; {g(rd)} = t; }}',
     'psubw': '' if rd == 0 else f'{{ GPR t; for (int i = 0; i < 4; i++) t.u32[i] = {g(rs)}.u32[i] - {g(rt)}.u32[i]; {g(rd)} = t; }}',
     'paddub': '' if rd == 0 else f'{{ GPR t; for (int i = 0; i < 16; i++) {{ unsigned v = {g(rs)}.u8[i] + {g(rt)}.u8[i]; t.u8[i] = v > 255 ? 255 : v; }} {g(rd)} = t; }}',
     'pextlw': '' if rd == 0 else f'{{ GPR t; t.u32[0] = {g(rt)}.u32[0]; t.u32[1] = {g(rs)}.u32[0]; t.u32[2] = {g(rt)}.u32[1]; t.u32[3] = {g(rs)}.u32[1]; {g(rd)} = t; }}',
     'pextuw': '' if rd == 0 else f'{{ GPR t; t.u32[0] = {g(rt)}.u32[2]; t.u32[1] = {g(rs)}.u32[2]; t.u32[2] = {g(rt)}.u32[3]; t.u32[3] = {g(rs)}.u32[3]; {g(rd)} = t; }}',
     'pmfhl': '' if rd == 0 else f'ee_pmfhl(ctx, {rd}, {sa});',
     'plzcw': '' if rd == 0 else f'ee_plzcw(ctx, {rd}, {rs});',
     # ---- system
     'syscall': f'rec_syscall(ctx, 0x{a:x});', 'break': f'rec_break(ctx, 0x{a:x});',
     'mfsa': W64(rd, 'ctx->sa'), 'mtsa': f'ctx->sa = {U32(rs)} & 15;',
     # ---- bootgate additions
     'padduw': '' if rd == 0 else f'{{ GPR t; for (int i = 0; i < 4; i++) {{ uint64_t v = (uint64_t){g(rs)}.u32[i] + {g(rt)}.u32[i]; t.u32[i] = v > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)v; }} {g(rd)} = t; }}',
     'mtsah': f'ctx->sa = (({U32(rs)} ^ {imm}) & 7) << 1;', 'mtsab': f'ctx->sa = (({U32(rs)} ^ {imm}) & 15);',
     'maddu': f'ee_maddu(ctx, 0, {rd}, {U32(rs)}, {U32(rt)});', 'maddu1': f'ee_maddu(ctx, 1, {rd}, {U32(rs)}, {U32(rt)});',
    }
    if m in T:
        SUPPORTED.add(m)
        return T[m]
    # bootgate: interpreter fallbacks (interp.c) instead of rec_unimpl
    if m == 'eret': return 'ctx->ret = ee_eret(ctx); return; /* eret: continue at EPC/ErrorEPC (as a return) */'
    if m == 'lqc2': return f'ee_lqc2(ctx, {rt}, {ea});'
    if m == 'sqc2': return f'ee_sqc2(ctx, {rt}, {ea});'
    if op == 28: return f'ee_mmi(ctx, 0x{a:x}, 0x{w:08x}u); /* {m} */'
    if op == 16 and rs in (0, 4): return f'ee_cop0(ctx, 0x{a:x}, 0x{w:08x}u); /* {m} */'
    if op == 18 and not m.startswith('bc2'): return f'ee_cop2(ctx, 0x{a:x}, 0x{w:08x}u); /* {m} */'
    return None

COND = {'beq': '{a} == {b}', 'bne': '{a} != {b}', 'beql': '{a} == {b}', 'bnel': '{a} != {b}',
        'blez': '(int64_t){a} <= 0', 'bgtz': '(int64_t){a} > 0', 'blezl': '(int64_t){a} <= 0', 'bgtzl': '(int64_t){a} > 0',
        'bltz': '(int64_t){a} < 0', 'bgez': '(int64_t){a} >= 0', 'bltzl': '(int64_t){a} < 0', 'bgezl': '(int64_t){a} >= 0',
        'bltzal': '(int64_t){a} < 0', 'bgezal': '(int64_t){a} >= 0',
        'bc1t': 'ctx->fcc', 'bc1f': '!ctx->fcc', 'bc1tl': 'ctx->fcc', 'bc1fl': '!ctx->fcc'}
for k in list(COND) + ['j', 'jal', 'jr', 'jalr']: SUPPORTED.add(k)

def fname(a): return f'f_{a:08x}'

class Unit:
    def __init__(self, blob, funcs, jumptables, known_funcs):
        self.b = blob; self.W = R.words(blob); self.funcs = funcs; self.jt = jumptables; self.known = known_funcs
        self.unimpl = []
        self.targets = set()          # every static branch / j target in .text
        for i, x in enumerate(self.W[:(0x3a1b80 - 0x100000) // 4]):
            m = R.decode(x); pc = 0x100000 + 4 * i
            if m in R.BRANCH: self.targets.add(pc + 4 + 4 * s16(x & 0xffff))
            elif m == 'j': self.targets.add((x & 0x3ffffff) << 2)
    def w(self, a): return self.W[(a - 0x100000) // 4]
    def slot(self, a):
        x = self.w(a); c = sem(x, a)
        if c is None:
            self.unimpl.append((a, R.decode(x))); return f'rec_unimpl(ctx, 0x{a:x}, 0x{x:08x}); /* {R.decode(x)} */'
        return c
    def call(self, t):
        return f'{fname(t)}(ctx);' if t in self.known else f'rec_call(ctx, 0x{t:x}u);'
    def in_code(self, a):
        return 0x100000 <= a < 0x3a1b80 or 0x464480 <= a < 0x469460
    def reach(self, start):
        """bootgate: the function body is every instruction statically reachable from its entry (branches,
        intra-function j, switch tables), not [start, next start). Shared tails get duplicated."""
        Rs = set(); todo = [start]; starts = self.known
        while todo:
            a = todo.pop()
            if a in Rs or not self.in_code(a): continue
            if a != start and a in starts and a not in self.reach_extra: continue   # fallthrough/branch into another function
            Rs.add(a)
            x = self.w(a); m = R.decode(x); rs = (x >> 21) & 31
            if m in COND:
                Rs.add(a + 4); t = a + 4 + 4 * s16(x & 0xffff)
                if t not in starts or t == start: todo.append(t)
                todo.append(a + 8)
            elif m == 'j':
                Rs.add(a + 4); t = (a & 0xf0000000) | ((x & 0x3ffffff) << 2)
                if t not in starts or t == start: todo.append(t)
            elif m in ('jal', 'jalr'):
                Rs.add(a + 4); todo.append(a + 8)
            elif m == 'jr':
                Rs.add(a + 4)
                if rs != 31 and a in self.jt: todo.extend(self.jt[a])
            elif m == 'eret':
                pass
            else:
                todo.append(a + 4)
        return Rs
    def func(self, start, end=None, idx=0):
        self.reach_extra = set()
        Rs = self.reach(start); self.last_end = max(Rs) + 4
        XFER = set(COND) | {'j', 'jal', 'jr', 'jalr', 'eret'}
        def runlen(f):     # instructions executed from f to the next control transfer (with its delay slot): the block cost
            n = 0
            while f in Rs and n < 128:
                n += 1
                if R.decode(self.w(f)) in XFER: return n + 1
                f += 4
            return max(n, 1)
        def cyc(f): return f'REC_CYC({runlen(f)}); '
        out = [f'void {fname(start)}(Ctx *ctx) {{', f'  REC_ENTER({idx}, 0x{start:x}u); REC_CYC({runlen(start)});']
        order = sorted(Rs); i = 0; emitted_ds = set()
        def fall(f, nxt):   # control continues at f after the instruction just emitted
            if f not in Rs: out.append(f'  REC_FALLTHROUGH(0x{start:x}u, 0x{f:x}u); /* runs into the next function */')
            elif nxt != f: out.append(f'  goto L_{f:x};')
        if order[0] != start: out.append(f'  goto L_{start:x};')
        while i < len(order):
            a = order[i]; i += 1
            if a in emitted_ds: continue
            nxt_head = next((o for o in order[i:] if o not in emitted_ds and o != a + 4), None)
            x = self.w(a); m = R.decode(x); rs = (x >> 21) & 31; rt = (x >> 16) & 31
            lbl = f'L_{a:x}:;'
            if m in COND or m in ('j', 'jal', 'jr', 'jalr'):
                ds = self.slot(a + 4); emitted_ds.add(a + 4)
                ft = None   # fallthrough address after this transfer, if any
                if m in COND:
                    t = a + 4 + 4 * s16(x & 0xffff)
                    cond = COND[m].format(a=U64(rs), b=U64(rt))
                    link = f' ctx->r[31].u64[0] = 0x{a+8:x};' if m.endswith('al') else ''
                    if m in R.LIKELY:
                        code = f'if ({cond}) {{{link} {ds} goto L_{t:x}; }}'
                    else:
                        code = f'{{ int c = {cond};{link} {ds} if (c) goto L_{t:x}; }}'
                    if t not in Rs: code = code.replace(f'goto L_{t:x};', f'{{ {self.call(t)} return; }}')
                    elif t <= a: code = code.replace(f'goto L_{t:x};', f'{{ {cyc(t)}REC_POLL(); goto L_{t:x}; }}')   # loop back-edge = interrupt safe point
                    else: code = code.replace(f'goto L_{t:x};', f'{{ {cyc(t)}goto L_{t:x}; }}')
                    ft = a + 8
                    if a + 8 in Rs: code += f' {cyc(a + 8)}' if m not in R.LIKELY else ''
                    if m in R.LIKELY and a + 8 in Rs: code = code + f' else {{ {cyc(a + 8)}}}' 
                elif m == 'j':
                    t = (a & 0xf0000000) | ((x & 0x3ffffff) << 2)
                    code = (f'{ds} {cyc(t)}' + ('REC_POLL(); ' if t <= a else '') + f'goto L_{t:x};') if t in Rs else f'{ds} {self.call(t)} return; /* tail call */'
                elif m == 'jal':
                    t = (a & 0xf0000000) | ((x & 0x3ffffff) << 2)
                    code = f'ctx->r[31].u64[0] = 0x{a+8:x}; {ds} {self.call(t)} REC_CHKRET(0x{a+8:x}u); {cyc(a + 8) if a + 8 in Rs else ""}'
                    ft = a + 8
                elif m == 'jr' and rs == 31:
                    code = f'{{ uint32_t t = {U32(31)}; {ds} ctx->ret = t; return; }}'
                elif m == 'jr' and a in self.jt:
                    cases = ' '.join(f'case 0x{tt:x}: goto L_{tt:x};' if tt in Rs else
                                     f'case 0x{tt:x}: rec_call(ctx, t); return; /* target is another function */'
                                     for tt in sorted(set(self.jt[a])))
                    code = (f'{{ uint32_t t = {U32(rs)}; {ds} switch (t) {{ {cases} '
                            f'default: rec_bad_jump(ctx, 0x{a:x}, t); }} }}')
                elif m == 'jr':
                    code = f'{{ uint32_t t = {U32(rs)}; {ds} rec_call(ctx, t); return; }} /* indirect tail call */'
                else:  # jalr
                    rd = (x >> 11) & 31
                    chk = f' REC_CHKRET(0x{a+8:x}u);' if rd == 31 else ''
                    code = f'{{ uint32_t t = {U32(rs)}; ctx->r[{rd}].u64[0] = 0x{a+8:x}; {ds} rec_call(ctx, t);{chk} {cyc(a + 8) if a + 8 in Rs else ""}}}'
                    ft = a + 8
                out.append(f'  {lbl} {code}')
                if a + 4 in self.targets and a + 4 in Rs:   # a branch enters this delay slot (libkernel 0x26f74c)
                    out.append(f'  if (0) {{ L_{a+4:x}:; {ds} ' + (f'goto L_{a+8:x}; }}' if a + 8 in Rs else f'REC_FALLTHROUGH(0x{start:x}u, 0x{a+8:x}u); }}'))
                else:
                    out.append(f'  L_{a+4:x}:; /* delay slot */')
                if ft is not None: fall(ft, nxt_head)
                continue
            out.append(f'  {lbl} {self.slot(a)}')
            if m == 'eret': continue
            fall(a + 4, nxt_head)
        out.append('}')
        return '\n'.join(out)

def main():
    poc = os.path.dirname(os.path.abspath(__file__))
    exe = os.path.join(os.environ.get('HG_GAME_DIR', os.path.join(poc, '..', 'game')), 'SLUS_210.75')
    blob = R.load(exe)
    S = os.environ.get('SCRATCH', os.path.join(poc, '..', 'data'))
    ends = {}
    if os.path.exists(os.path.join(S, 'ghfuncs.tsv')):
        for l in open(os.path.join(S, 'ghfuncs.tsv')):
            p = l.split('\t'); ends[int(p[0], 16)] = int(p[2], 16) + 1
    jt = {}
    if os.path.exists(os.path.join(S, 'jumptables.json')):
        for t in json.load(open(os.path.join(S, 'jumptables.json')))['tables']: jt[t['jr']] = t['targets']
    specs = []
    for arg in sys.argv[2:]:
        s, _, e = arg.partition(':'); s = int(s, 16); specs.append((s, int(e, 16) if e else ends[s]))
    u = Unit(blob, None, jt, set(s for s, _ in specs))
    body = ['/* generated by rec.py from SLUS_210.75 -- do not edit */', '#include "ee_runtime.h"', '']
    body += [f'void {fname(s)}(Ctx *ctx);' for s, _ in specs] + ['']
    for s, e in specs: body += [u.func(s, e), '']
    body += ['const RecEntry rec_table[] = {'] + [f'  {{0x{s:x}u, {fname(s)}}},' for s, _ in specs] + ['  {0, 0}', '};']
    open(sys.argv[1], 'w').write('\n'.join(body) + '\n')
    print(f'wrote {sys.argv[1]}: {len(specs)} functions, unimplemented sites: {u.unimpl}')

if __name__ == '__main__':
    main()
