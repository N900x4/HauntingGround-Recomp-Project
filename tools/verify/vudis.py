# vudis.py DUMP [from to]: disassemble VU micro memory (first 16 KB of an HG_VU1DUMP file)
import struct, sys

D = "xyzw"
def dest(w): s = "".join(c for i, c in enumerate(D) if w >> (24 - i) & 1); return "." + s if s else ""
BC = "xyzw"
UP = {}
for i, n in enumerate(["ADD", "SUB", "MADD", "MSUB", "MAX", "MINI", "MUL"]):
    for b in range(4): UP[i * 4 + b] = (n, b)
UP.update({28: ("MULq", None), 29: ("MAXi", None), 30: ("MULi", None), 31: ("MINIi", None), 32: ("ADDq", None), 33: ("MADDq", None),
           34: ("ADDi", None), 35: ("MADDi", None), 36: ("SUBq", None), 37: ("MSUBq", None), 38: ("SUBi", None), 39: ("MSUBi", None),
           40: ("ADD", None), 41: ("MADD", None), 42: ("MUL", None), 43: ("MAX", None), 44: ("SUB", None), 45: ("MSUB", None),
           46: ("OPMSUB", None), 47: ("MINI", None)})
UP2 = {}
for i, n in enumerate(["ADDA", "SUBA", "MADDA", "MSUBA"]):
    for b in range(4): UP2[i * 4 + b] = (n, b)
for b in range(4): UP2[24 + b] = ("MULA", b)
UP2.update({16: ("ITOF0", None), 17: ("ITOF4", None), 18: ("ITOF12", None), 19: ("ITOF15", None), 20: ("FTOI0", None), 21: ("FTOI4", None),
            22: ("FTOI12", None), 23: ("FTOI15", None), 28: ("MULAq", None), 29: ("ABS", None), 30: ("MULAi", None), 31: ("CLIPw", None),
            32: ("ADDAq", None), 33: ("MADDAq", None), 34: ("ADDAi", None), 35: ("MADDAi", None), 36: ("SUBAq", None), 37: ("MSUBAq", None),
            38: ("SUBAi", None), 39: ("MSUBAi", None), 40: ("ADDA", None), 41: ("MADDA", None), 42: ("MULA", None), 44: ("SUBA", None),
            45: ("MSUBA", None), 46: ("OPMULA", None), 47: ("NOP", None)})

def upper(w):
    op = w & 63; ft, fs, fd = (w >> 16) & 31, (w >> 11) & 31, (w >> 6) & 31
    flags = "".join(f for b, f in ((31, "[I]"), (30, "[E]"), (29, "[M]"), (28, "[D]"), (27, "[T]")) if w >> b & 1)
    if op < 60:
        n, b = UP.get(op, ("?%d" % op, None))
        if b is not None: return f"{n}{BC[b]}{dest(w)} vf{fd}, vf{fs}, vf{ft}{BC[b]}{flags}"
        if n.endswith(("q", "i")): return f"{n}{dest(w)} vf{fd}, vf{fs}{flags}"
        return f"{n}{dest(w)} vf{fd}, vf{fs}, vf{ft}{flags}"
    n, b = UP2.get((fd << 2) | (op & 3), ("?u2", None))
    if n == "NOP": return "NOP" + flags
    if n == "CLIPw": return f"CLIPw.xyz vf{fs}, vf{ft}w{flags}"
    if n.startswith(("ITOF", "FTOI", "ABS")): return f"{n}{dest(w)} vf{ft}, vf{fs}{flags}"
    if b is not None: return f"{n}{BC[b]}{dest(w)} ACC, vf{fs}, vf{ft}{BC[b]}{flags}"
    return f"{n}{dest(w)} ACC, vf{fs}, vf{ft}{flags}"

def s11(w): v = w & 0x7FF; return v - 0x800 if v & 0x400 else v
def lower(w, pc):
    hi = w >> 25; it, is_, idr = (w >> 16) & 31, (w >> 11) & 31, (w >> 6) & 31
    if hi == 0x40:
        op = w & 63
        if op < 0x3C:
            return {0x30: f"IADD vi{idr}, vi{is_}, vi{it}", 0x31: f"ISUB vi{idr}, vi{is_}, vi{it}", 0x32: f"IADDI vi{it}, vi{is_}, {((w >> 6) & 31) - 32 if (w >> 6) & 16 else (w >> 6) & 31}",
                    0x34: f"IAND vi{idr}, vi{is_}, vi{it}", 0x35: f"IOR vi{idr}, vi{is_}, vi{it}"}.get(op, "?l1 %x" % op)
        o2 = ((w >> 6) & 31) << 2 | (op & 3); fsf, ftf = (w >> 21) & 3, (w >> 23) & 3
        m = {0x30: f"MOVE{dest(w)} vf{it}, vf{is_}", 0x31: f"MR32{dest(w)} vf{it}, vf{is_}", 0x34: f"LQI{dest(w)} vf{it}, (vi{is_}++)",
             0x35: f"SQI{dest(w)} vf{is_}, (vi{it}++)", 0x36: f"LQD{dest(w)} vf{it}, (--vi{is_})", 0x37: f"SQD{dest(w)} vf{is_}, (--vi{it})",
             0x38: f"DIV Q, vf{is_}{D[fsf]}, vf{it}{D[ftf]}", 0x39: f"SQRT Q, vf{it}{D[ftf]}", 0x3A: f"RSQRT Q, vf{is_}{D[fsf]}, vf{it}{D[ftf]}",
             0x3B: "WAITQ", 0x3C: f"MTIR vi{it}, vf{is_}{D[fsf]}", 0x3D: f"MFIR{dest(w)} vf{it}, vi{is_}", 0x3E: f"ILWR{dest(w)} vi{it}, (vi{is_})",
             0x3F: f"ISWR{dest(w)} vi{it}, (vi{is_})", 0x64: f"MFP{dest(w)} vf{it}, P", 0x68: f"XTOP vi{it}", 0x69: f"XITOP vi{it}",
             0x6C: f"XGKICK vi{is_}", 0x2F: "NOP"}
        if o2 in m: return m[o2]
        if 0x70 <= o2 <= 0x7F: return "EFU %x" % o2
        return "?l2 %x" % o2
    imm24 = w & 0xFFFFFF
    tgt = lambda: "0x%03x" % ((pc + 1 + s11(w)) & 0x7FF)
    m = {0x00: lambda: f"LQ{dest(w)} vf{it}, {s11(w)}(vi{is_})", 0x01: lambda: f"SQ{dest(w)} vf{is_}, {s11(w)}(vi{it})",
         0x04: lambda: f"ILW{dest(w)} vi{it}, {s11(w)}(vi{is_})", 0x05: lambda: f"ISW{dest(w)} vi{it}, {s11(w)}(vi{is_})",
         0x08: lambda: f"IADDIU vi{it}, vi{is_}, {((w >> 10) & 0x7800) | (w & 0x7FF)}", 0x09: lambda: f"ISUBIU vi{it}, vi{is_}, {((w >> 10) & 0x7800) | (w & 0x7FF)}",
         0x10: lambda: f"FCEQ vi1, 0x{imm24:06x}", 0x11: lambda: f"FCSET 0x{imm24:06x}", 0x12: lambda: f"FCAND vi1, 0x{imm24:06x}",
         0x13: lambda: f"FCOR vi1, 0x{imm24:06x}", 0x14: lambda: f"FSEQ vi{it}, 0x{((w >> 10) & 0x800) | (w & 0x7FF):03x}",
         0x15: lambda: f"FSSET 0x{((w >> 10) & 0x800) | (w & 0x7FF):03x}", 0x16: lambda: f"FSAND vi{it}, 0x{((w >> 10) & 0x800) | (w & 0x7FF):03x}",
         0x17: lambda: f"FSOR vi{it}, 0x{((w >> 10) & 0x800) | (w & 0x7FF):03x}", 0x18: lambda: f"FMEQ vi{it}, vi{is_}",
         0x1A: lambda: f"FMAND vi{it}, vi{is_}", 0x1B: lambda: f"FMOR vi{it}, vi{is_}", 0x1C: lambda: f"FCGET vi{it}",
         0x20: lambda: f"B {tgt()}", 0x21: lambda: f"BAL vi{it}, {tgt()}", 0x24: lambda: f"JR vi{is_}", 0x25: lambda: f"JALR vi{it}, vi{is_}",
         0x28: lambda: f"IBEQ vi{it}, vi{is_}, {tgt()}", 0x29: lambda: f"IBNE vi{it}, vi{is_}, {tgt()}", 0x2C: lambda: f"IBLTZ vi{is_}, {tgt()}",
         0x2D: lambda: f"IBGTZ vi{is_}, {tgt()}", 0x2E: lambda: f"IBLEZ vi{is_}, {tgt()}", 0x2F: lambda: f"IBGEZ vi{is_}, {tgt()}"}
    return m[hi]() if hi in m else "?l %02x" % hi

if __name__ == "__main__":
    b = open(sys.argv[1], "rb").read()[:16384]
    a0 = int(sys.argv[2], 16) if len(sys.argv) > 2 else 0; a1 = int(sys.argv[3], 16) if len(sys.argv) > 3 else 2048
    for pc in range(a0, a1):
        lo, up = struct.unpack_from("<II", b, pc * 8)
        if lo == 0 and up == 0: continue
        l = "LOI %g" % struct.unpack("<f", struct.pack("<I", lo))[0] if up >> 31 & 1 else lower(lo, pc)
        print("%03x  %-44s %s" % (pc, upper(up), l))
