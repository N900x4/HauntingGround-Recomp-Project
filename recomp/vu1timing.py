#!/usr/bin/env python3
"""VU1 static timing analysis: for every Q consumer (an upper op with the Q broadcast, MULq/ADDq/...) and every P
consumer (MFP) of the game's 19 VU1 programs, decide when the result of the latest DIV/SQRT/RSQRT (Q) or EFU op (P)
arrives, under the interpreters' own timing model:

  * one cycle per instruction pair, plus pipeline stalls: a VF field written by either unit is readable 4 cycles
    after issue (a pair reading it earlier waits), and a DIV/SQRT/RSQRT waits for a busy FDIV unit;
  * Q arrives 7 (DIV, SQRT) or 13 (RSQRT) cycles after issue, P 11-54 cycles after an EFU op, counted in cycles
    (stalls included); WAITQ/WAITP wait for it (the whole pair: the upper op of a WAITQ pair sees the new Q);
  * every run starts from a drained pipeline (no stall pending, no Q/P pending) and ends with Q/P committed.

The model exists twice in the runtime: vu1_core.h (stall_cycles, note_writes; used by the fast interpreter and the
compiled programs) and vu1_ref.c (ref_stall, ref_writes; the lockstep oracle). Both are reproduced here and run side
by side; the one difference between them (ACC readiness: one value in vu1_core.h, per field in vu1_ref.c) is
reported wherever it changes a stall.

Method: an exact abstract execution. A node is (pc, pending branch target, E-bit pending); its timing state is the
set of VF fields not yet readable with their remaining cycles, the FDIV/EFU countdowns (q_wait, p_wait) and the
cycles since the latest issue. Branch conditions are not evaluated (both directions are followed), so the result
covers every path through the program. Entries: 0, every MSCAL immediate given, and the pc each run ends at (the
MSCNT entries), to a closure. Classification of a consumer over all the states that reach it:

  SAFE       the latest Q/P has arrived in every state (or none was issued in this run: Q/P from a previous run),
  OLD        it has not arrived in any state (the consumer certainly reads the previous Q/P),
  AMBIGUOUS  otherwise (the compiled code would need dynamic cycle counting there).

vu1gen.py uses analyse() to emit the compiled programs without cycle bookkeeping; this script prints the report.
usage: vu1timing.py [-v] [--entries PROG:PC,...]   (run from recomp/; reads ../game/SLUS_210.75 or $HG_GAME_DIR)"""
import sys, os
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

# ---------------------------------------------------------------- decode (mirrors vu1_core.h decode_upper/lower)
U_NOP, U_ARITH, U_OPMULA, U_OPMSUB, U_ITOF, U_FTOI, U_ABS, U_CLIP = range(8)
K_ADD, K_SUB, K_MADD, K_MSUB, K_MAX, K_MINI, K_MUL = range(7)
B_VEC, B_BC, B_Q, B_I = range(4)
LK = ('NONE LQ SQ ILW ISW IADDIU ISUBIU FCEQ FCSET FCAND FCOR FSEQ FSSET FSAND FSOR FMEQ FMAND FMOR FCGET B BAL JR JALR '
      'IBEQ IBNE IBLTZ IBGTZ IBLEZ IBGEZ IADD ISUB IADDI IAND IOR MOVE MR32 LQI SQI LQD SQD DIV SQRT RSQRT WAITQ MTIR MFIR '
      'ILWR ISWR RNEXT RGET RINIT RXOR MFP XTOP XITOP XGKICK WAITP EFU BADSPECIAL UNKNOWN').split()
EFU_LAT = {0x70: 11, 0x71: 18, 0x72: 18, 0x73: 24, 0x74: 54, 0x75: 54, 0x76: 12, 0x78: 12, 0x79: 18, 0x7A: 12,
           0x7C: 29, 0x7D: 54, 0x7E: 44}
EFU_NAME = {0x70: 'ESADD', 0x71: 'ERSADD', 0x72: 'ELENG', 0x73: 'ERLENG', 0x74: 'EATANxy', 0x75: 'EATANxz', 0x76: 'ESUM',
            0x78: 'ESQRT', 0x79: 'ERSQRT', 0x7A: 'ERCPR', 0x7C: 'ESIN', 0x7D: 'EATAN', 0x7E: 'EEXP'}
BRANCHES = ('B', 'BAL', 'IBEQ', 'IBNE', 'IBLTZ', 'IBGTZ', 'IBLEZ', 'IBGEZ')
FLAG_READS = ('FCEQ', 'FCSET', 'FCAND', 'FCOR', 'FSEQ', 'FSSET', 'FSAND', 'FSOR', 'FMEQ', 'FMAND', 'FMOR', 'FCGET')

class Dec:
    pass

def decode(lo, up):
    d = Dec(); d.lo, d.up = lo, up
    op = up & 63
    d.dest = (up >> 21) & 15; d.ft = (up >> 16) & 31; d.fs = (up >> 11) & 31; d.fd = (up >> 6) & 31
    d.uk = U_NOP; d.to_acc = 0; d.bsrc = B_VEC; d.bc = 0; d.kind = K_ADD
    if (op & 0x3C) != 0x3C:
        d.uk = U_ARITH
        if op < 0x1C: d.kind = op >> 2; d.bsrc = B_BC; d.bc = op & 3
        elif op == 0x2E: d.uk = U_OPMSUB
        else:
            d.kind, d.bsrc = {0x1C: (K_MUL, B_Q), 0x1D: (K_MAX, B_I), 0x1E: (K_MUL, B_I), 0x1F: (K_MINI, B_I),
                              0x20: (K_ADD, B_Q), 0x21: (K_MADD, B_Q), 0x22: (K_ADD, B_I), 0x23: (K_MADD, B_I),
                              0x24: (K_SUB, B_Q), 0x25: (K_MSUB, B_Q), 0x26: (K_SUB, B_I), 0x27: (K_MSUB, B_I),
                              0x28: (K_ADD, B_VEC), 0x29: (K_MADD, B_VEC), 0x2A: (K_MUL, B_VEC), 0x2B: (K_MAX, B_VEC),
                              0x2C: (K_SUB, B_VEC), 0x2D: (K_MSUB, B_VEC), 0x2F: (K_MINI, B_VEC)}.get(op, (K_MUL, B_VEC))
    else:
        sop = ((up >> 6) & 31) << 2 | (up & 3)
        if sop < 0x10: d.uk = U_ARITH; d.to_acc = 1; d.kind = sop >> 2; d.bsrc = B_BC; d.bc = sop & 3
        elif 0x18 <= sop < 0x1C: d.uk = U_ARITH; d.to_acc = 1; d.kind = K_MUL; d.bsrc = B_BC; d.bc = sop & 3
        elif sop < 0x14: d.uk = U_ITOF
        elif sop < 0x18: d.uk = U_FTOI
        elif sop == 0x1D: d.uk = U_ABS
        elif sop == 0x1F: d.uk = U_CLIP
        elif sop == 0x2E: d.uk = U_OPMULA; d.to_acc = 1
        elif sop == 0x2F: pass
        else:
            k = {0x1C: (K_MUL, B_Q), 0x1E: (K_MUL, B_I), 0x20: (K_ADD, B_Q), 0x21: (K_MADD, B_Q), 0x22: (K_ADD, B_I),
                 0x23: (K_MADD, B_I), 0x24: (K_SUB, B_Q), 0x25: (K_MSUB, B_Q), 0x26: (K_SUB, B_I), 0x27: (K_MSUB, B_I),
                 0x28: (K_ADD, B_VEC), 0x29: (K_MADD, B_VEC), 0x2A: (K_MUL, B_VEC), 0x2C: (K_SUB, B_VEC),
                 0x2D: (K_MSUB, B_VEC)}.get(sop)
            if k: d.uk = U_ARITH; d.to_acc = 1; d.kind, d.bsrc = k
    d.flags = (d.uk == U_ARITH and (d.to_acc or d.kind not in (K_MAX, K_MINI))) or d.uk in (U_OPMSUB, U_OPMULA)   # sets MAC/status
    d.ibit = up >> 31; d.ebit = (up >> 30) & 1
    d.lk = 'NONE'; d.sop = 0; d.imm = 0
    d.ldest = (lo >> 21) & 15; d.lft = (lo >> 16) & 31; d.lfs = (lo >> 11) & 31; d.lit = d.lft & 15; d.lis = d.lfs & 15
    d.lid = (lo >> 6) & 15; d.fsf = (lo >> 21) & 3; d.ftf = (lo >> 23) & 3
    if not d.ibit:
        lop = lo >> 25
        d.imm = ((lo & 0x7FF) ^ 0x400) - 0x400
        d.lk = {0x00: 'LQ', 0x01: 'SQ', 0x04: 'ILW', 0x05: 'ISW', 0x08: 'IADDIU', 0x09: 'ISUBIU', 0x10: 'FCEQ',
                0x11: 'FCSET', 0x12: 'FCAND', 0x13: 'FCOR', 0x14: 'FSEQ', 0x15: 'FSSET', 0x16: 'FSAND', 0x17: 'FSOR',
                0x18: 'FMEQ', 0x1A: 'FMAND', 0x1B: 'FMOR', 0x1C: 'FCGET', 0x20: 'B', 0x21: 'BAL', 0x24: 'JR',
                0x25: 'JALR', 0x28: 'IBEQ', 0x29: 'IBNE', 0x2C: 'IBLTZ', 0x2D: 'IBGTZ', 0x2E: 'IBLEZ',
                0x2F: 'IBGEZ'}.get(lop, 'UNKNOWN')
        if lop == 0x40:
            f6 = lo & 63
            if f6 in (0x30, 0x31, 0x32, 0x34, 0x35): d.lk = {0x30: 'IADD', 0x31: 'ISUB', 0x32: 'IADDI', 0x34: 'IAND', 0x35: 'IOR'}[f6]
            elif (f6 & 0x3C) != 0x3C: d.lk = 'BADSPECIAL'
            else:
                d.sop = sop = ((lo >> 6) & 31) << 2 | (lo & 3)
                d.lk = {0x30: 'MOVE', 0x31: 'MR32', 0x34: 'LQI', 0x35: 'SQI', 0x36: 'LQD', 0x37: 'SQD', 0x38: 'DIV',
                        0x39: 'SQRT', 0x3A: 'RSQRT', 0x3B: 'WAITQ', 0x3C: 'MTIR', 0x3D: 'MFIR', 0x3E: 'ILWR', 0x3F: 'ISWR',
                        0x40: 'RNEXT', 0x41: 'RGET', 0x42: 'RINIT', 0x43: 'RXOR', 0x64: 'MFP', 0x68: 'XTOP', 0x69: 'XITOP',
                        0x6C: 'XGKICK', 0x7B: 'WAITP'}.get(sop, 'EFU' if sop in EFU_LAT else 'UNKNOWN')
    # the VI register this pair writes (SETVI in vu1_core.h; vi0 is never written) and the ones a branch reads
    d.wvi = -1
    if d.lk in ('ILW', 'IADDIU', 'ISUBIU', 'FSEQ', 'FSAND', 'FSOR', 'FMEQ', 'FMAND', 'FMOR', 'FCGET', 'BAL', 'JALR',
                'IADDI', 'MTIR', 'ILWR', 'XTOP', 'XITOP'):
        d.wvi = d.lit
    elif d.lk in ('FCEQ', 'FCAND', 'FCOR'): d.wvi = 1
    elif d.lk in ('IADD', 'ISUB', 'IAND', 'IOR'): d.wvi = d.lid
    elif d.lk in ('LQI', 'LQD'): d.wvi = d.lis
    elif d.lk in ('SQI', 'SQD'): d.wvi = d.lit
    if d.lk in ('ILW', 'ILWR') and not d.ldest: d.wvi = -1     # no field selected: nothing written
    if d.wvi == 0: d.wvi = -1
    d.bvi = {'IBEQ': (d.lit, d.lis), 'IBNE': (d.lit, d.lis), 'JR': (d.lis,), 'JALR': (d.lis,)}.get(d.lk, (d.lis,) if d.lk.startswith('IB') else ())
    d.qcons = d.uk == U_ARITH and d.bsrc == B_Q
    d.pcons = d.lk == 'MFP'
    d.qlat = {'DIV': 7, 'SQRT': 7, 'RSQRT': 13}.get(d.lk, 0)
    d.plat = EFU_LAT.get(d.sop, 0) if d.lk == 'EFU' else 0
    return d

def mnemonic(d):
    if d.lk == 'EFU': return EFU_NAME[d.sop]
    return d.lk

# ---------------------------------------------------------------- the two timing models
# A timing state: (rdy, acc, qw, pw, qs, ps). rdy: sorted tuple of (reg, field, remaining cycles) for VF fields not yet
# readable (register 32 = ACC per field, used by the reference model), relative to the cycle of the last pair; acc: the
# single ACC readiness of vu1_core.h; qw/pw: q_wait/p_wait; qs/ps: cycles since the latest Q/P issue in this run
# (None: none issued yet; capped).
CAP = 64

def fields(mask):
    return [i for i in range(4) if mask & (8 >> i)]

def core_reads(d):
    """(reg, mask) pairs read by a pair, and whether ACC is read (vu1_core.h stall_cycles)"""
    r = []; acc = False
    if d.uk == U_ARITH:
        r.append((d.fs, d.dest))
        if d.bsrc == B_VEC: r.append((d.ft, d.dest))
        elif d.bsrc == B_BC: r.append((d.ft, 8 >> d.bc))
        acc = d.kind in (K_MADD, K_MSUB)
    elif d.uk == U_OPMULA: r += [(d.fs, 0xE), (d.ft, 0xE)]
    elif d.uk == U_OPMSUB: r += [(d.fs, 0xE), (d.ft, 0xE)]; acc = True
    elif d.uk in (U_ITOF, U_FTOI, U_ABS): r.append((d.fs, d.dest))
    elif d.uk == U_CLIP: r += [(d.fs, 0xE), (d.ft, 1)]
    if not d.ibit:
        if d.lk in ('SQ', 'SQI', 'SQD', 'MOVE'): r.append((d.lfs, d.ldest))
        elif d.lk in ('MR32', 'EFU'): r.append((d.lfs, 0xF))
        elif d.lk in ('MTIR', 'RINIT', 'RXOR'): r.append((d.lfs, 8 >> d.fsf))
        elif d.lk in ('DIV', 'RSQRT'): r += [(d.lfs, 8 >> d.fsf), (d.lft, 8 >> d.ftf)]
        elif d.lk == 'SQRT': r.append((d.lft, 8 >> d.ftf))
    return r, acc

def core_writes(d):
    w = []; acc = False
    if d.uk == U_ARITH:
        if d.to_acc: acc = True
        else: w.append((d.fd, d.dest))
    elif d.uk == U_OPMULA: acc = True
    elif d.uk == U_OPMSUB: w.append((d.fd, 0xE))
    elif d.uk in (U_ITOF, U_FTOI, U_ABS): w.append((d.ft, d.dest))
    if not d.ibit and d.lk in ('LQ', 'LQI', 'LQD', 'MOVE', 'MR32', 'MFIR', 'MFP', 'RGET', 'RNEXT'): w.append((d.lft, d.ldest))
    return w, acc

def ref_reads(d):
    """vu1_ref.c ref_stall, from the raw words (register 32 = ACC)"""
    up, lo = d.up, d.lo; r = []
    op = up & 63; dest = (up >> 21) & 15; ft = (up >> 16) & 31; fs = (up >> 11) & 31; bc = up & 3
    if (op & 0x3C) != 0x3C:
        if op < 0x1C:
            r += [(fs, dest), (ft, 8 >> bc)]
            if 8 <= op < 16: r.append((32, dest))
        elif op < 0x28:
            r.append((fs, dest))
            if op in (0x21, 0x23, 0x25, 0x27): r.append((32, dest))
        elif op == 0x2E: r += [(fs, 0xE), (ft, 0xE), (32, 0xE)]
        else:
            r += [(fs, dest), (ft, dest)]
            if op in (0x29, 0x2D): r.append((32, dest))
    else:
        sop = ((up >> 6) & 31) << 2 | (up & 3)
        if sop < 0x10 or 0x18 <= sop < 0x1C:
            r += [(fs, dest), (ft, 8 >> bc)]
            if 8 <= sop < 16: r.append((32, dest))
        elif sop < 0x18 or sop == 0x1D: r.append((fs, dest))
        elif sop == 0x1F: r += [(fs, 0xE), (ft, 1)]
        elif sop == 0x2E: r += [(fs, 0xE), (ft, 0xE)]
        elif 0x1C <= sop <= 0x27 and sop not in (0x1D, 0x1F):
            r.append((fs, dest))
            if sop in (0x21, 0x23, 0x25, 0x27): r.append((32, dest))
        elif 0x28 <= sop <= 0x2D and sop != 0x2B:
            r += [(fs, dest), (ft, dest)]
            if sop in (0x29, 0x2D): r.append((32, dest))
    fdiv = False
    if not (up & 0x80000000):
        lop = lo >> 25; ld = (lo >> 21) & 15; lft = (lo >> 16) & 31; lfs = (lo >> 11) & 31; fsf = (lo >> 21) & 3; ftf = (lo >> 23) & 3
        if lop == 0x01: r.append((lfs, ld))
        elif lop == 0x40 and (lo & 0x3C) == 0x3C:
            s2 = ((lo >> 6) & 31) << 2 | (lo & 3)
            if s2 in (0x30, 0x35, 0x37): r.append((lfs, ld))
            elif s2 == 0x31 or (0x70 <= s2 <= 0x7E and s2 not in (0x77, 0x7B)): r.append((lfs, 0xF))
            elif s2 in (0x3C, 0x42, 0x43): r.append((lfs, 8 >> fsf))
            if s2 in (0x38, 0x3A): r += [(lfs, 8 >> fsf), (lft, 8 >> ftf)]
            if s2 == 0x39: r.append((lft, 8 >> ftf))
            fdiv = s2 in (0x38, 0x39, 0x3A)
    return r, fdiv

def ref_writes(d):
    up, lo = d.up, d.lo; w = []
    op = up & 63; dest = (up >> 21) & 15; ft = (up >> 16) & 31; fd = (up >> 6) & 31
    if (op & 0x3C) != 0x3C: w.append((fd, 0xE if op == 0x2E else dest))
    else:
        sop = ((up >> 6) & 31) << 2 | (up & 3)
        if sop < 0x10 or (0x18 <= sop <= 0x2D and sop not in (0x1D, 0x1F, 0x2B)) or sop == 0x2E: w.append((32, 0xE if sop == 0x2E else dest))
        elif sop < 0x18 or sop == 0x1D: w.append((ft, dest))
    if not (up & 0x80000000):
        lop = lo >> 25; ld = (lo >> 21) & 15; lft = (lo >> 16) & 31
        if lop == 0x00: w.append((lft, ld))
        elif lop == 0x40 and (lo & 0x3C) == 0x3C:
            s2 = ((lo >> 6) & 31) << 2 | (lo & 3)
            if s2 in (0x30, 0x31, 0x34, 0x36, 0x3D, 0x40, 0x41, 0x64): w.append((lft, ld))
    return w

class Model:
    """one timing model, precomputed per pair: the fields it reads and writes"""
    def __init__(self, name, code_decoded):
        self.name = name; self.rd = []; self.wr = []; self.racc = []; self.wacc = []; self.fdiv = []
        for d in code_decoded:
            if name == 'core':
                r, a = core_reads(d); w, wa = core_writes(d); fd = not d.ibit and d.lk in ('DIV', 'SQRT', 'RSQRT')
            else:
                r, fd = ref_reads(d); a = False; w = ref_writes(d); wa = False
            self.rd.append(tuple((reg, f) for reg, m in r if reg for f in fields(m)))
            self.wr.append(tuple((reg, f) for reg, m in w if reg for f in fields(m)))
            self.racc.append(a); self.wacc.append(wa); self.fdiv.append(fd)

    def step(self, k, d, st):
        """advance the timing state over pair k; returns (stall, q_wait and p_wait seen by the pair's reads, new state)"""
        rdy, acc, qw, pw, qs, ps = st
        rd = dict(((reg, f), rem) for reg, f, rem in rdy)
        need = 0
        for key in self.rd[k]:
            need = max(need, rd.get(key, 0) - 1)
        if self.racc[k]: need = max(need, acc - 1)
        if self.fdiv[k] and qw > 1: need = max(need, qw - 1)
        stall = max(0, need); adv = 1 + stall
        if qw: qw = max(0, qw - adv)
        if pw: pw = max(0, pw - adv)
        if qs is not None: qs = min(CAP, qs + adv)
        if ps is not None: ps = min(CAP, ps + adv)
        if not d.ibit and d.lk == 'WAITQ': qw = 0
        if not d.ibit and d.lk == 'WAITP': pw = 0
        seen = (qw, pw, qs, ps)
        if d.qlat: qw = d.qlat; qs = 0
        if d.plat: pw = d.plat; ps = 0
        nrd = {key: rem - adv for key, rem in rd.items() if rem - adv > 0}
        for key in self.wr[k]: nrd[key] = 4
        acc = max(0, acc - adv)
        if self.wacc[k]: acc = 4
        return stall, seen, (tuple(sorted((r, f, v) for (r, f), v in nrd.items())), acc, qw, pw, qs, ps)

DRAINED = ((), 0, 0, 0, None, None)

# ---------------------------------------------------------------- the abstract execution
class Result:
    pass

def analyse(code, entries=(0,), max_states=2000000):
    """explore every path from the entries; returns a Result with, per node (pc, branch target or -1, E pending):
    the successors, and per pc the Q/P observations of both models"""
    n = len(code)
    dec = [decode(lo, up) for lo, up in code]
    models = [Model('core', dec), Model('ref', dec)]
    R = Result(); R.dec = dec; R.n = n
    R.nodes = {}          # node -> set of successor nodes (('end', tpc) for a run end, ('out', pc, bp) for leaving)
    R.qobs = {}           # pc -> set of (model, arrived, cycles since issue or None, q_wait)
    R.pobs = {}
    R.prev_vi = {}        # node -> set of VI registers written by the previous pair (-1 none)
    R.stall_diff = set()  # pcs where the two models stall differently
    R.entries = []; R.tpcs = set(); R.leaves = set(); R.dynamic = set()
    R.nstates = 0
    seen = set(); work = []
    todo = list(entries); done_e = set()
    def add(node, sts, pv):
        key = (node, sts, pv)
        if key not in seen:
            seen.add(key); work.append(key)
    while todo:
        e = todo.pop()
        if e in done_e or not (0 <= e < n): continue
        done_e.add(e); R.entries.append(e)
        add((e, -1, 0), (DRAINED, DRAINED), -1)
        while work:
            (pc, bp, eb), sts, pv = work.pop()
            R.nstates += 1
            if R.nstates > max_states: raise RuntimeError('state explosion')
            node = (pc, bp, eb); d = dec[pc]
            R.prev_vi.setdefault(node, set()).add(pv)
            succ = R.nodes.setdefault(node, set())
            new = []; stalls = []
            for m, st in zip(models, sts):
                stall, (qw, pw, qs, ps), nst = m.step(pc, d, st)
                stalls.append(stall); new.append(nst)
                if d.qcons: R.qobs.setdefault(pc, set()).add((m.name, qw == 0, qs, qw))
                if d.pcons: R.pobs.setdefault(pc, set()).add((m.name, pw == 0, ps, pw))
            if stalls[0] != stalls[1]: R.stall_diff.add(pc)
            nsts = tuple(new)
            nxt = bp if bp >= 0 else (pc + 1) & 2047
            if eb:
                succ.add(('end', nxt)); R.tpcs.add(nxt)
                if nxt not in done_e: todo.append(nxt)
                continue
            if d.lk in ('JR', 'JALR'):
                R.dynamic.add(pc); succ.add(('dyn', pc)); continue
            outs = []
            if d.lk in ('B', 'BAL'): outs = [(pc + 1 + d.imm) & 2047]
            elif d.lk.startswith('IB'): outs = [-1, (pc + 1 + d.imm) & 2047]
            else: outs = [-1]
            for t in outs:
                if nxt >= n:
                    succ.add(('out', nxt, t)); R.leaves.add(nxt); continue
                s = (nxt, t, d.ebit)
                succ.add(s)
                add(s, nsts, d.wvi)
    return R

def classify(obs, model='both'):
    """SAFE / OLD / AMBIGUOUS over the observations of one model (or both)"""
    arr = {a for m, a, s, w in obs if model == 'both' or m == model}
    if arr == {True}: return 'SAFE'
    if arr == {False}: return 'OLD'
    return 'AMBIGUOUS'

def elapsed(obs, model='core'):
    """min and max cycles since the latest issue over the states (None: no issue in this run on some path)"""
    v = [s for m, a, s, w in obs if m == model]
    carried = any(s is None for s in v); v = [s for s in v if s is not None]
    return (min(v) if v else None, max(v) if v else None, carried)

def program_entries(p, extra):
    return sorted({0} | {pc for q, pc in extra if q == p})

# ---------------------------------------------------------------- report
def report(progs, extra=(), verbose=False):
    tot = {'SAFE': 0, 'OLD': 0, 'AMBIGUOUS': 0}; totp = dict(tot)
    out = []
    out.append('prog pairs entries          states  Q-cons SAFE OLD AMB | P-cons SAFE OLD AMB | DIV EFU WAITQ WAITP | JR/JALR leaves stall-diff')
    allres = []
    for p, code in enumerate(progs):
        R = analyse(code, program_entries(p, extra)); allres.append(R)
        cq = {pc: classify(o) for pc, o in R.qobs.items()}; cp = {pc: classify(o) for pc, o in R.pobs.items()}
        for c in cq.values(): tot[c] += 1
        for c in cp.values(): totp[c] += 1
        # consumers on which the two models disagree
        dis = [pc for pc, o in list(R.qobs.items()) + list(R.pobs.items()) if classify(o, 'core') != classify(o, 'ref')]
        ndiv = sum(1 for d in R.dec if d.qlat); nefu = sum(1 for d in R.dec if d.plat)
        nwq = sum(1 for d in R.dec if d.lk == 'WAITQ'); nwp = sum(1 for d in R.dec if d.lk == 'WAITP')
        out.append('%4d %5d %-16s %7d  %6d %4d %3d %3d | %6d %4d %3d %3d | %3d %3d %5d %5d | %7d %6d %d%s' % (
            p, R.n, ','.join('%x' % e for e in sorted(R.entries)), R.nstates, len(cq), sum(c == 'SAFE' for c in cq.values()),
            sum(c == 'OLD' for c in cq.values()), sum(c == 'AMBIGUOUS' for c in cq.values()), len(cp),
            sum(c == 'SAFE' for c in cp.values()), sum(c == 'OLD' for c in cp.values()), sum(c == 'AMBIGUOUS' for c in cp.values()),
            ndiv, nefu, nwq, nwp, len(R.dynamic), len(R.leaves), len(R.stall_diff), '' if not dis else '  MODELS DISAGREE at ' + ' '.join('%x' % x for x in dis)))
        if verbose:
            for kind, obs in (('Q', R.qobs), ('P', R.pobs)):
                for pc in sorted(obs):
                    o = obs[pc]; c = classify(o); lo_, hi_, car = elapsed(o)
                    out.append('     %s %03x %-6s %-9s cycles since issue %s..%s%s' % (kind, pc, 'MFP' if kind == 'P' else 'q-op', c,
                               lo_, hi_, ' (or Q/P of a previous run)' if car else ''))
    out.append('consumers of Q: %d SAFE, %d OLD, %d AMBIGUOUS; of P: %d SAFE, %d OLD, %d AMBIGUOUS' % (
        tot['SAFE'], tot['OLD'], tot['AMBIGUOUS'], totp['SAFE'], totp['OLD'], totp['AMBIGUOUS']))
    # the prev_vi branch rule: is the previous pair known at every branch?
    nb = nstatic = 0
    for R in allres:
        for node, pvs in R.prev_vi.items():
            d = R.dec[node[0]]
            if d.lk in BRANCHES or d.lk in ('JR', 'JALR'):
                if not d.bvi: continue
                nb += 1
                if len({pv if pv in d.bvi else -1 for pv in pvs}) == 1: nstatic += 1
    out.append('branch rule (VI written by the previous pair): %d of %d branch nodes static' % (nstatic, nb))
    return '\n'.join(out), allres

if __name__ == '__main__':
    import vu1gen
    args = sys.argv[1:]; verbose = '-v' in args
    extra = []
    if '--entries' in args:
        for t in args[args.index('--entries') + 1].split(','):
            a, b = t.split(':'); extra.append((int(a), int(b, 16)))
    game = os.environ.get('HG_GAME_DIR', os.path.join(HERE, '..', 'game'))
    text, _ = report(vu1gen.programs(os.path.join(game, 'SLUS_210.75')), extra, verbose)
    print(text)
