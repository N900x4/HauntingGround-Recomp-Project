/* VU1 micro-mode interpreter.
   Scope: the 19 VU1 programs Haunting Ground uploads (docs/hardware.md 1.1): FMAC upper ops, integer/load-store/
   branch lower ops, DIV/SQRT/RSQRT with Q latency, CLIP + clip-flag ops, MAC/status flags, XTOP/XITOP, XGKICK.
   Timing model: one instruction per cycle; FMAC results are visible at once (the hardware interlocks on VF reads),
   Q becomes visible 7 (DIV/SQRT) or 13 (RSQRT) instructions after issue or at WAITQ, and a conditional branch
   reads the old value of a VI register written by the instruction right before it.
   EFU ops are decoded but only ESUM/ERSQRT-style results are approximated (the game uses none). */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <float.h>
#include "ee_runtime.h"
#include "hle.h"

void gif_qword(int path, const uint64_t q[2]);
int gif_path_done(int path);

uint8_t vu1_mem[16384];          /* data memory, 1024 qwords */
uint8_t vu1_code[16384];         /* micro memory, 2048 instructions */
uint32_t vu1_tpc;                /* next instruction (index) for MSCNT */
uint32_t vu1_top, vu1_itop;

typedef union { float f[4]; uint32_t u[4]; int32_t s[4]; } Vec;
static Vec vf[32], acc;
static uint16_t vi[16];
static float q_reg, p_reg, i_reg, q_pend, p_pend; static int q_wait, p_wait;
static uint32_t r_reg = 0x3F800000;
static uint32_t mac, status, clip;
static uint64_t n_runs, n_insns, n_kicks;

static inline float clampf(float x) {
    if (isnan(x)) return signbit(x) ? -FLT_MAX : FLT_MAX;
    if (isinf(x)) return x > 0 ? FLT_MAX : -FLT_MAX;
    return x;
}
static inline float rdf(uint32_t u) {           /* VU has no Inf/NaN: treat exponent 255 as max */
    if ((u & 0x7F800000u) == 0x7F800000u) u = (u & 0x80000000u) | 0x7F7FFFFFu;
    float f; memcpy(&f, &u, 4); return f;
}
static inline Vec *vmem(uint32_t qaddr) { return (Vec *)(vu1_mem + ((qaddr & 0x3FF) << 4)); }

/* MAC flags: bit 3-i = zero(field i), 7-i = sign, 11-i = underflow, 15-i = overflow (field 0 = x) */
static uint32_t new_mac;
static void flag_field(int i, float v) {
    if (v == 0.0f) new_mac |= 1u << (3 - i);
    if (signbit(v) && v != 0.0f) new_mac |= 1u << (7 - i);
    if (fabsf(v) == FLT_MAX) new_mac |= 1u << (15 - i);
}
static void flags_commit(void) {
    mac = new_mac;
    uint32_t s = 0;
    if (mac & 0x000F) s |= 1; if (mac & 0x00F0) s |= 2; if (mac & 0x0F00) s |= 4; if (mac & 0xF000) s |= 8;
    status = (status & 0xFF0) | s | (s << 6);
}

/* ---------------------------------------------------------------- upper (FMAC) */
typedef struct { int to_acc, reg, dest, valid; Vec v; int flags; } UpRes;
static float bcast(const Vec *t, int bc) { return rdf(t->u[bc]); }
static void upper(uint32_t u, UpRes *r) {
    int dest = (u >> 21) & 15, ft = (u >> 16) & 31, fs = (u >> 11) & 31, fd = (u >> 6) & 31, op = u & 63;
    r->valid = 0; r->flags = 0; r->dest = dest;
    Vec S = vf[fs], T = vf[ft], out = {{0}};
    float bc = 0; int have_bc = 0;
#define FIELD(i) (dest & (8 >> (i)))
    if ((op & 0x3C) != 0x3C) {
        /* standard: result to fd */
        int kind = -1; /* 0 add 1 sub 2 madd 3 msub 4 max 5 mini 6 mul */
        if (op < 0x1C) { kind = op >> 2; bc = bcast(&T, op & 3); have_bc = 1; }
        else switch (op) {
            case 0x1C: kind = 6; bc = q_reg; have_bc = 1; break;  case 0x1D: kind = 4; bc = i_reg; have_bc = 1; break;
            case 0x1E: kind = 6; bc = i_reg; have_bc = 1; break;  case 0x1F: kind = 5; bc = i_reg; have_bc = 1; break;
            case 0x20: kind = 0; bc = q_reg; have_bc = 1; break;  case 0x21: kind = 2; bc = q_reg; have_bc = 1; break;
            case 0x22: kind = 0; bc = i_reg; have_bc = 1; break;  case 0x23: kind = 2; bc = i_reg; have_bc = 1; break;
            case 0x24: kind = 1; bc = q_reg; have_bc = 1; break;  case 0x25: kind = 3; bc = q_reg; have_bc = 1; break;
            case 0x26: kind = 1; bc = i_reg; have_bc = 1; break;  case 0x27: kind = 3; bc = i_reg; have_bc = 1; break;
            case 0x28: kind = 0; break; case 0x29: kind = 2; break; case 0x2A: kind = 6; break; case 0x2B: kind = 4; break;
            case 0x2C: kind = 1; break; case 0x2D: kind = 3; break; case 0x2F: kind = 5; break;
            case 0x2E: /* OPMSUB: fd = acc - fs x ft (cross) */
                out.f[0] = clampf(rdf(acc.u[0]) - rdf(S.u[1]) * rdf(T.u[2])); out.f[1] = clampf(rdf(acc.u[1]) - rdf(S.u[2]) * rdf(T.u[0]));
                out.f[2] = clampf(rdf(acc.u[2]) - rdf(S.u[0]) * rdf(T.u[1])); out.f[3] = 0;
                r->valid = 1; r->to_acc = 0; r->reg = fd; r->dest = 0xE; r->v = out; r->flags = 1; return;
        }
        for (int i = 0; i < 4; i++) {
            float a = rdf(S.u[i]), b = have_bc ? bc : rdf(T.u[i]), x;
            switch (kind) {
            case 0: x = a + b; break; case 1: x = a - b; break;
            case 2: x = rdf(acc.u[i]) + a * b; break; case 3: x = rdf(acc.u[i]) - a * b; break;
            case 4: x = a > b ? a : b; break; case 5: x = a < b ? a : b; break;
            default: x = a * b;
            }
            out.f[i] = clampf(x);
        }
        r->valid = 1; r->to_acc = 0; r->reg = fd; r->v = out; r->flags = kind != 4 && kind != 5; return;
    }
    int sop = ((u >> 6) & 31) << 2 | (u & 3);
    int kind = -1;
    if (sop < 0x10) { kind = (sop >> 2) == 0 ? 0 : (sop >> 2) == 1 ? 1 : (sop >> 2) == 2 ? 2 : 3; bc = bcast(&T, sop & 3); have_bc = 1; }
    else if (sop >= 0x18 && sop < 0x1C) { kind = 6; bc = bcast(&T, sop & 3); have_bc = 1; }
    else switch (sop) {
        case 0x10: case 0x11: case 0x12: case 0x13: {   /* ITOF0/4/12/15 */
            static const float sc[4] = { 1.0f, 1.0f / 16, 1.0f / 4096, 1.0f / 32768 };
            for (int i = 0; i < 4; i++) out.f[i] = (float)S.s[i] * sc[sop & 3];
            r->valid = 1; r->to_acc = 0; r->reg = ft; r->v = out; return; }
        case 0x14: case 0x15: case 0x16: case 0x17: {   /* FTOI0/4/12/15 */
            static const float sc[4] = { 1.0f, 16.0f, 4096.0f, 32768.0f };
            for (int i = 0; i < 4; i++) { double x = (double)rdf(S.u[i]) * sc[sop & 3];
                out.s[i] = x >= 2147483647.0 ? 0x7FFFFFFF : x <= -2147483648.0 ? (int32_t)0x80000000 : (int32_t)x; }
            r->valid = 1; r->to_acc = 0; r->reg = ft; r->v = out; return; }
        case 0x1C: kind = 6; bc = q_reg; have_bc = 1; break; case 0x1E: kind = 6; bc = i_reg; have_bc = 1; break;
        case 0x1D: for (int i = 0; i < 4; i++) out.f[i] = fabsf(rdf(S.u[i])); r->valid = 1; r->to_acc = 0; r->reg = ft; r->v = out; return;
        case 0x1F: {   /* CLIP fs.xyz against ft.w */
            float w = fabsf(rdf(T.u[3])); uint32_t c = 0;
            float x = rdf(S.u[0]), y = rdf(S.u[1]), z = rdf(S.u[2]);
            if (x > w) c |= 1; if (x < -w) c |= 2; if (y > w) c |= 4; if (y < -w) c |= 8; if (z > w) c |= 16; if (z < -w) c |= 32;
            clip = ((clip << 6) | c) & 0xFFFFFF; return; }
        case 0x20: kind = 0; bc = q_reg; have_bc = 1; break; case 0x21: kind = 2; bc = q_reg; have_bc = 1; break;
        case 0x22: kind = 0; bc = i_reg; have_bc = 1; break; case 0x23: kind = 2; bc = i_reg; have_bc = 1; break;
        case 0x24: kind = 1; bc = q_reg; have_bc = 1; break; case 0x25: kind = 3; bc = q_reg; have_bc = 1; break;
        case 0x26: kind = 1; bc = i_reg; have_bc = 1; break; case 0x27: kind = 3; bc = i_reg; have_bc = 1; break;
        case 0x28: kind = 0; break; case 0x29: kind = 2; break; case 0x2A: kind = 6; break; case 0x2C: kind = 1; break; case 0x2D: kind = 3; break;
        case 0x2E:   /* OPMULA: acc = fs x ft (cross, positive half) */
            out.f[0] = clampf(rdf(S.u[1]) * rdf(T.u[2])); out.f[1] = clampf(rdf(S.u[2]) * rdf(T.u[0])); out.f[2] = clampf(rdf(S.u[0]) * rdf(T.u[1]));
            r->valid = 1; r->to_acc = 1; r->dest = 0xE; r->v = out; r->flags = 1; return;
        case 0x2F: return;   /* NOP */
        default: return;
    }
    for (int i = 0; i < 4; i++) {
        float a = rdf(S.u[i]), b = have_bc ? bc : rdf(T.u[i]), x;
        switch (kind) { case 0: x = a + b; break; case 1: x = a - b; break; case 2: x = rdf(acc.u[i]) + a * b; break; case 3: x = rdf(acc.u[i]) - a * b; break; default: x = a * b; }
        out.f[i] = clampf(x);
    }
    r->valid = 1; r->to_acc = 1; r->v = out; r->flags = 1;
#undef FIELD
}
static void upper_commit(const UpRes *r, int lower_wrote_reg) {
    if (!r->valid) return;
    if (r->flags) new_mac = 0;
    if (r->to_acc) { for (int i = 0; i < 4; i++) if (r->dest & (8 >> i)) { acc.u[i] = r->v.u[i]; if (r->flags) flag_field(i, r->v.f[i]); } }
    else if (r->reg) {   /* if the lower op wrote the same VF, the upper result wins */ for (int i = 0; i < 4; i++) if (r->dest & (8 >> i)) { vf[r->reg].u[i] = r->v.u[i]; if (r->flags) flag_field(i, r->v.f[i]); } }
    else if (r->flags) for (int i = 0; i < 4; i++) if (r->dest & (8 >> i)) flag_field(i, r->v.f[i]);
    if (r->flags) flags_commit();
}

/* ---------------------------------------------------------------- lower */
static void q_commit(void) { if (q_wait) { q_reg = q_pend; q_wait = 0; } }
static void p_commit(void) { if (p_wait) { p_reg = p_pend; p_wait = 0; } }
static void p_issue(float v, int lat) { p_commit(); p_pend = clampf(v); p_wait = lat; }
static void q_issue(float v, int lat) { q_commit(); q_pend = clampf(v); q_wait = lat; }
static void kick(uint32_t addr) {
    n_kicks++;
    for (int guard = 0; guard < 1024; guard++) {
        uint64_t q[2]; memcpy(q, vu1_mem + ((addr & 0x3FF) << 4), 16); addr++;
        gif_qword(1, q);
        if (gif_path_done(1)) break;
    }
}
/* returns: 0 normal, 1 branch taken (target in *tgt), 2 halt request (not used) */
static int lower(uint32_t l, uint32_t pc, int *wrote_vf, int *wrote_vi, uint16_t vi_before_prev[16], int prev_vi, uint32_t *tgt) {
    int op = l >> 25;
    int dest = (l >> 21) & 15, ft = (l >> 16) & 31, fs = (l >> 11) & 31, it = ft & 15, is = fs & 15;
    int32_t imm11 = (int32_t)(l << 21) >> 21;
    *wrote_vf = 0; *wrote_vi = -1;
#define SETVI(r, v) do { if ((r) & 15) { vi[(r) & 15] = (uint16_t)(v); *wrote_vi = (r) & 15; } } while (0)
#define VIB(r) ((r) == prev_vi ? vi_before_prev[r] : vi[r])     /* branch operand: old value if written by the previous instruction */
    switch (op) {
    case 0x00: { Vec *m = vmem(vi[is] + imm11); for (int i = 0; i < 4; i++) if (dest & (8 >> i)) if (ft) vf[ft].u[i] = m->u[i]; *wrote_vf = ft; return 0; }   /* LQ */
    case 0x01: { Vec *m = vmem(vi[it] + imm11); for (int i = 0; i < 4; i++) if (dest & (8 >> i)) m->u[i] = vf[fs].u[i]; return 0; }                       /* SQ */
    case 0x04: { Vec *m = vmem(vi[is] + imm11); for (int i = 0; i < 4; i++) if (dest & (8 >> i)) { SETVI(it, m->u[i] & 0xFFFF); break; } return 0; }      /* ILW */
    case 0x05: { Vec *m = vmem(vi[is] + imm11); for (int i = 0; i < 4; i++) if (dest & (8 >> i)) m->u[i] = vi[it]; return 0; }                           /* ISW */
    case 0x08: SETVI(it, vi[is] + (((l >> 10) & 0x7800) | (l & 0x7FF))); return 0;                                                                           /* IADDIU */
    case 0x09: SETVI(it, vi[is] - (((l >> 10) & 0x7800) | (l & 0x7FF))); return 0;                                                                           /* ISUBIU */
    case 0x10: SETVI(1, (clip & 0xFFFFFF) == (l & 0xFFFFFF)); return 0;                                                                                      /* FCEQ */
    case 0x11: clip = l & 0xFFFFFF; return 0;                                                                                                                /* FCSET */
    case 0x12: SETVI(1, (clip & l & 0xFFFFFF) != 0); return 0;                                                                                               /* FCAND */
    case 0x13: SETVI(1, ((clip | l) & 0xFFFFFF) == 0xFFFFFF); return 0;                                                                                     /* FCOR */
    case 0x14: SETVI(it, (status & 0xFFF) == (((l >> 10) & 0x800) | (l & 0x7FF))); return 0;                                                                 /* FSEQ */
    case 0x15: status = (status & 0x3F) | ((((l >> 10) & 0x800) | (l & 0x7FF)) & 0xFC0); return 0;                                                          /* FSSET */
    case 0x16: SETVI(it, status & (((l >> 10) & 0x800) | (l & 0x7FF))); return 0;                                                                           /* FSAND */
    case 0x17: SETVI(it, status | (((l >> 10) & 0x800) | (l & 0x7FF))); return 0;                                                                           /* FSOR */
    case 0x18: SETVI(it, (mac & 0xFFFF) == vi[is]); return 0;                                                                                                /* FMEQ */
    case 0x1A: SETVI(it, mac & vi[is]); return 0;                                                                                                            /* FMAND */
    case 0x1B: SETVI(it, mac | vi[is]); return 0;                                                                                                            /* FMOR */
    case 0x1C: SETVI(it, clip & 0xFFF); return 0;                                                                                                            /* FCGET */
    case 0x20: *tgt = pc + 1 + imm11; return 1;                                                                                                              /* B */
    case 0x21: SETVI(it, pc + 2); *tgt = pc + 1 + imm11; return 1;                                                                                           /* BAL */
    case 0x24: *tgt = VIB(is); return 1;                                                                                                                     /* JR */
    case 0x25: { uint32_t t = VIB(is); SETVI(it, pc + 2); *tgt = t; return 1; }                                                                              /* JALR */
    case 0x28: if (VIB(it) == VIB(is)) { *tgt = pc + 1 + imm11; return 1; } return 0;                                                                        /* IBEQ */
    case 0x29: if (VIB(it) != VIB(is)) { *tgt = pc + 1 + imm11; return 1; } return 0;                                                                        /* IBNE */
    case 0x2C: if ((int16_t)VIB(is) < 0) { *tgt = pc + 1 + imm11; return 1; } return 0;                                                                      /* IBLTZ */
    case 0x2D: if ((int16_t)VIB(is) > 0) { *tgt = pc + 1 + imm11; return 1; } return 0;                                                                      /* IBGTZ */
    case 0x2E: if ((int16_t)VIB(is) <= 0) { *tgt = pc + 1 + imm11; return 1; } return 0;                                                                     /* IBLEZ */
    case 0x2F: if ((int16_t)VIB(is) >= 0) { *tgt = pc + 1 + imm11; return 1; } return 0;                                                                     /* IBGEZ */
    case 0x40: break;
    default: L("VU1: unknown lower op 0x%02x (0x%08x) at %u\n", op, l, pc); return 0;
    }
    /* op 0x40: special */
    int f6 = l & 63, id = (l >> 6) & 15;
    switch (f6) {
    case 0x30: SETVI(id, vi[is] + vi[it]); return 0;                         /* IADD */
    case 0x31: SETVI(id, vi[is] - vi[it]); return 0;                         /* ISUB */
    case 0x32: SETVI(it, vi[is] + ((int32_t)(l << 21) >> 27)); return 0;     /* IADDI: imm5 in bits 10-6 */
    case 0x34: SETVI(id, vi[is] & vi[it]); return 0;                         /* IAND */
    case 0x35: SETVI(id, vi[is] | vi[it]); return 0;                         /* IOR */
    }
    if ((f6 & 0x3C) != 0x3C) { L("VU1: unknown special op 0x%02x (0x%08x) at %u\n", f6, l, pc); return 0; }
    int sop = ((l >> 6) & 31) << 2 | (l & 3);
    int fsf = (l >> 21) & 3, ftf = (l >> 23) & 3;
    switch (sop) {
    case 0x30: if (ft) { for (int i = 0; i < 4; i++) if (dest & (8 >> i)) vf[ft].u[i] = vf[fs].u[i]; *wrote_vf = ft; } return 0;   /* MOVE */
    case 0x31: if (ft) { Vec s = vf[fs]; for (int i = 0; i < 4; i++) if (dest & (8 >> i)) vf[ft].u[i] = s.u[(i + 1) & 3]; *wrote_vf = ft; } return 0;   /* MR32 */
    case 0x34: { Vec *m = vmem(vi[is]); if (ft) for (int i = 0; i < 4; i++) if (dest & (8 >> i)) vf[ft].u[i] = m->u[i]; *wrote_vf = ft; SETVI(is, vi[is] + 1); return 0; }   /* LQI */
    case 0x35: { Vec *m = vmem(vi[it]); for (int i = 0; i < 4; i++) if (dest & (8 >> i)) m->u[i] = vf[fs].u[i]; SETVI(it, vi[it] + 1); return 0; }   /* SQI */
    case 0x36: { SETVI(is, vi[is] - 1); Vec *m = vmem(vi[is]); if (ft) for (int i = 0; i < 4; i++) if (dest & (8 >> i)) vf[ft].u[i] = m->u[i]; *wrote_vf = ft; return 0; }   /* LQD */
    case 0x37: { SETVI(it, vi[it] - 1); Vec *m = vmem(vi[it]); for (int i = 0; i < 4; i++) if (dest & (8 >> i)) m->u[i] = vf[fs].u[i]; return 0; }   /* SQD */
    case 0x38: { float a = rdf(vf[fs].u[fsf]), b = rdf(vf[ft].u[ftf]);                                                       /* DIV */
        status &= ~0x30u;
        if (b == 0.0f) { status |= a == 0.0f ? 0x10 : 0x20; q_issue((signbit(a) ^ signbit(b)) ? -FLT_MAX : FLT_MAX, 7); } else q_issue(a / b, 7); return 0; }
    case 0x39: { float b = rdf(vf[ft].u[ftf]); status &= ~0x30u; if (b < 0) status |= 0x10; q_issue(sqrtf(fabsf(b)), 7); return 0; }   /* SQRT */
    case 0x3A: { float a = rdf(vf[fs].u[fsf]), b = rdf(vf[ft].u[ftf]); status &= ~0x30u;                                      /* RSQRT */
        if (b == 0.0f) { status |= 0x20; q_issue(FLT_MAX, 13); } else { if (b < 0) status |= 0x10; q_issue(a / sqrtf(fabsf(b)), 13); } return 0; }
    case 0x3B: q_commit(); return 0;                                                                                          /* WAITQ */
    case 0x3C: SETVI(it, vf[fs].u[fsf] & 0xFFFF); return 0;                                                                   /* MTIR */
    case 0x3D: if (ft) { for (int i = 0; i < 4; i++) if (dest & (8 >> i)) vf[ft].s[i] = (int16_t)vi[is]; *wrote_vf = ft; } return 0;   /* MFIR */
    case 0x3E: { Vec *m = vmem(vi[is]); for (int i = 0; i < 4; i++) if (dest & (8 >> i)) { SETVI(it, m->u[i] & 0xFFFF); break; } return 0; }   /* ILWR */
    case 0x3F: { Vec *m = vmem(vi[is]); for (int i = 0; i < 4; i++) if (dest & (8 >> i)) m->u[i] = vi[it]; return 0; }      /* ISWR */
    case 0x40: { r_reg = (r_reg ^ (r_reg << 1 | ((r_reg >> 4 ^ r_reg >> 22) & 1))) & 0x7FFFFF; r_reg |= 0x3F800000; }        /* RNEXT */
        /* fall through */
    case 0x41: if (ft) { for (int i = 0; i < 4; i++) if (dest & (8 >> i)) vf[ft].u[i] = r_reg; *wrote_vf = ft; } return 0;   /* RGET */
    case 0x42: r_reg = 0x3F800000 | (vf[fs].u[fsf] & 0x7FFFFF); return 0;                                                       /* RINIT */
    case 0x43: r_reg = 0x3F800000 | ((r_reg ^ vf[fs].u[fsf]) & 0x7FFFFF); return 0;                                            /* RXOR */
    case 0x64: if (ft) { for (int i = 0; i < 4; i++) if (dest & (8 >> i)) vf[ft].f[i] = p_reg; *wrote_vf = ft; } return 0;    /* MFP */
    case 0x68: SETVI(it, vu1_top); return 0;                                                                                  /* XTOP */
    case 0x69: SETVI(it, vu1_itop); return 0;                                                                                 /* XITOP */
    case 0x6C: kick(vi[is]); return 0;                                                                                        /* XGKICK */
    case 0x7B: p_commit(); return 0;                                                                                          /* WAITP */
    case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75: case 0x76: case 0x78: case 0x79: case 0x7A: case 0x7C: case 0x7D: case 0x7E: {
        /* EFU: result to P after the documented latency */
        float x = rdf(vf[fs].u[0]), y = rdf(vf[fs].u[1]), z = rdf(vf[fs].u[2]), w = rdf(vf[fs].u[3]), f = rdf(vf[fs].u[fsf]);
        float sq = x * x + y * y + z * z;
        switch (sop) {
        case 0x70: p_issue(sq, 11); break;                                          /* ESADD */
        case 0x71: p_issue(sq != 0 ? 1.0f / sq : FLT_MAX, 18); break;               /* ERSADD */
        case 0x72: p_issue(sqrtf(sq), 18); break;                                   /* ELENG */
        case 0x73: p_issue(sq != 0 ? 1.0f / sqrtf(sq) : FLT_MAX, 24); break;        /* ERLENG */
        case 0x74: p_issue(atan2f(y, x), 54); break;                                /* EATANxy */
        case 0x75: p_issue(atan2f(z, x), 54); break;                                /* EATANxz */
        case 0x76: p_issue(x + y + z + w, 12); break;                               /* ESUM */
        case 0x78: p_issue(sqrtf(fabsf(f)), 12); break;                             /* ESQRT */
        case 0x79: p_issue(f != 0 ? 1.0f / sqrtf(fabsf(f)) : FLT_MAX, 18); break;   /* ERSQRT */
        case 0x7A: p_issue(f != 0 ? 1.0f / f : FLT_MAX, 12); break;                 /* ERCPR */
        case 0x7C: p_issue(sinf(f), 29); break;                                     /* ESIN */
        case 0x7D: p_issue(atanf(f), 54); break;                                    /* EATAN */
        case 0x7E: p_issue(expf(-f), 44); break;                                    /* EEXP */
        }
        return 0; }
    default:
        L("VU1: unknown special2 op 0x%02x (0x%08x) at %u\n", sop, l, pc); return 0;
    }
#undef SETVI
#undef VIB
}

/* ---------------------------------------------------------------- run */
void vu1_run(uint32_t start) {
    n_runs++;
    uint32_t pc = start & 2047;
    int ebit_pending = 0, branch_pending = 0; uint32_t branch_tgt = 0;
    uint16_t vi_before_prev[16]; int prev_vi = -1;
    vf[0].f[0] = vf[0].f[1] = vf[0].f[2] = 0; vf[0].f[3] = 1.0f; vi[0] = 0;
    for (long n = 0; n < 4000000; n++) {
        uint32_t lo, up; memcpy(&lo, vu1_code + pc * 8, 4); memcpy(&up, vu1_code + pc * 8 + 4, 4);
        n_insns++;
        if (q_wait && --q_wait == 0) q_reg = q_pend;
        if (p_wait && --p_wait == 0) p_reg = p_pend;
        uint16_t vi_snap[16]; memcpy(vi_snap, vi, sizeof vi);
        UpRes ur; upper(up, &ur);
        int wrote_vf = 0, wrote_vi = -1, taken = 0; uint32_t tgt = 0;
        if (up & 0x80000000u) { memcpy(&i_reg, &lo, 4); }           /* I bit: lower word is a float immediate */
        else taken = lower(lo, pc, &wrote_vf, &wrote_vi, vi_before_prev, prev_vi, &tgt);
        upper_commit(&ur, wrote_vf);
        vi[0] = 0; vf[0].f[0] = vf[0].f[1] = vf[0].f[2] = 0; vf[0].f[3] = 1.0f;
        memcpy(vi_before_prev, vi_snap, sizeof vi_snap); prev_vi = wrote_vi;
        uint32_t next = (pc + 1) & 2047;
        if (branch_pending) { next = branch_tgt & 2047; branch_pending = 0; }
        if (taken) { branch_pending = 1; branch_tgt = tgt; }
        if (ebit_pending) { vu1_tpc = next; q_commit(); p_commit(); return; }
        if (up & 0x40000000u) ebit_pending = 1;                      /* E bit: run the next instruction, then stop */
        pc = next;
    }
    L("VU1: program at %u did not end\n", start);
    vu1_tpc = pc;
}
void vu1_dump_stats(void) { L("VU1: %llu runs, %llu instructions, %llu XGKICKs\n", (unsigned long long)n_runs, (unsigned long long)n_insns, (unsigned long long)n_kicks); }
