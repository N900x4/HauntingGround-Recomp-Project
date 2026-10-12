/* bootgate interpreter fallbacks for instructions rec.py does not translate:
   MMI halfword/byte ops, COP0 (mfc0/mtc0), COP2 = VU0 macro mode (lqc2/sqc2/qmtc2/qmfc2/cfc2/ctc2 + vector ops). */
#include <stdio.h>
#include <math.h>
#include "ee_runtime.h"
#include "hle.h"

static int16_t sat16(int v) { return v > 32767 ? 32767 : v < -32768 ? -32768 : v; }
void ee_mmi(Ctx *c, uint32_t pc, uint32_t w) {
    int rs = (w >> 21) & 31, rt = (w >> 16) & 31, rd = (w >> 11) & 31, sa = (w >> 6) & 31, fn = w & 63;
    GPR s = c->r[rs], t = c->r[rt], d = c->r[rd];
    if (rs == 0) memset(&s, 0, 16);
    if (rt == 0) memset(&t, 0, 16);
    int k;
    if (fn == 8) switch (sa) {          /* MMI0 */
        case 4: for (k = 0; k < 8; k++) d.u16[k] = s.u16[k] + t.u16[k]; break;                      /* paddh */
        case 7: for (k = 0; k < 8; k++) d.u16[k] = (int16_t)s.u16[k] > (int16_t)t.u16[k] ? s.u16[k] : t.u16[k]; break; /* pmaxh */
        case 6: for (k = 0; k < 8; k++) d.u16[k] = (int16_t)s.u16[k] > (int16_t)t.u16[k] ? 0xFFFF : 0; break; /* pcgth */
        case 9: for (k = 0; k < 16; k++) d.u8[k] = s.u8[k] - t.u8[k]; break;                        /* psubb */
        case 8: for (k = 0; k < 16; k++) d.u8[k] = s.u8[k] + t.u8[k]; break;                        /* paddb */
        case 5: for (k = 0; k < 8; k++) d.u16[k] = s.u16[k] - t.u16[k]; break;                      /* psubh */
        case 20: for (k = 0; k < 8; k++) d.u16[k] = sat16((int16_t)s.u16[k] + (int16_t)t.u16[k]); break; /* paddsh */
        case 21: for (k = 0; k < 8; k++) d.u16[k] = sat16((int16_t)s.u16[k] - (int16_t)t.u16[k]); break; /* psubsh */
        case 26: for (k = 0; k < 8; k++) { d.u8[2 * k] = t.u8[k]; d.u8[2 * k + 1] = s.u8[k]; } break; /* pextlb */
        case 22: for (k = 0; k < 4; k++) { d.u16[2 * k] = t.u16[k]; d.u16[2 * k + 1] = s.u16[k]; } break; /* pextlh */
        case 27: for (k = 0; k < 8; k++) { d.u8[k] = t.u8[2 * k]; d.u8[k + 8] = s.u8[2 * k]; } break; /* ppacb */
        case 23: for (k = 0; k < 4; k++) { d.u16[k] = t.u16[2 * k]; d.u16[k + 4] = s.u16[2 * k]; } break; /* ppach */
        default: goto bad;
    } else if (fn == 40) switch (sa) {  /* MMI1 */
        case 7: for (k = 0; k < 8; k++) d.u16[k] = (int16_t)s.u16[k] < (int16_t)t.u16[k] ? s.u16[k] : t.u16[k]; break; /* pminh */
        case 26: for (k = 0; k < 8; k++) { d.u8[2 * k] = t.u8[k + 8]; d.u8[2 * k + 1] = s.u8[k + 8]; } break; /* pextub */
        case 22: for (k = 0; k < 4; k++) { d.u16[2 * k] = t.u16[k + 4]; d.u16[2 * k + 1] = s.u16[k + 4]; } break; /* pextuh */
        case 6: for (k = 0; k < 8; k++) d.u16[k] = s.u16[k] == t.u16[k] ? 0xFFFF : 0; break;          /* pceqh */
        case 10: for (k = 0; k < 16; k++) d.u8[k] = s.u8[k] == t.u8[k] ? 0xFF : 0; break;            /* pceqb */
        case 27: { /* qfsrv: rd = low 128 of (rs:rt) >> (SA bytes * 8) */
            uint8_t b[32]; memcpy(b, t.u8, 16); memcpy(b + 16, s.u8, 16); int sh = c->sa & 15;
            for (k = 0; k < 16; k++) d.u8[k] = b[k + sh]; break; }
        default: goto bad;
    } else if (fn == 41) switch (sa) {  /* MMI3 */
        case 27: for (k = 0; k < 4; k++) { d.u16[k] = t.u16[0]; d.u16[k + 4] = t.u16[4]; } break;    /* pcpyh */
        default: goto bad;
    } else if (fn == 9) switch (sa) {   /* MMI2 */
        case 27: for (k = 0; k < 4; k++) { d.u16[k] = t.u16[3 - k]; d.u16[k + 4] = t.u16[7 - k]; } break; /* prevh */
        default: goto bad;
    } else switch (fn) {
        case 55: for (k = 0; k < 8; k++) d.u16[k] = (uint16_t)((int16_t)t.u16[k] >> (sa & 15)); break; /* psrah */
        case 54: for (k = 0; k < 8; k++) d.u16[k] = t.u16[k] >> (sa & 15); break;                      /* psrlh */
        case 52: for (k = 0; k < 8; k++) d.u16[k] = t.u16[k] << (sa & 15); break;                      /* psllh */
        default: goto bad;
    }
    if (rd) c->r[rd] = d;
    return;
bad:
    stop("interp: unimplemented MMI 0x%08x at 0x%x", w, pc);
}

static uint64_t cop0_count;
static uint32_t cop0_epc, cop0_errorepc;
uint32_t cop0_status = 0x70030C13;   /* EIE (bit16) toggled by ei/di */
void ee_cop0(Ctx *c, uint32_t pc, uint32_t w) {
    int rs = (w >> 21) & 31, rt = (w >> 16) & 31, rd = (w >> 11) & 31;
    if (rs == 0) {            /* mfc0 */
        uint32_t v = 0;
        if (rd == 9) v = (uint32_t)(cop0_count += 2000);    /* Count */
        else if (rd == 12) v = cop0_status;                 /* Status */
        else if (rd == 15) v = 0x2E20;                      /* PRId R5900 */
        if (rt) c->r[rt].s64[0] = (int32_t)v;
        return;
    }
    if (rs == 4) {            /* mtc0: Status, EPC, ErrorEPC tracked; rest ignored */
        uint32_t v = rt ? c->r[rt].u32[0] : 0;
        if (rd == 12) cop0_status = v; else if (rd == 14) cop0_epc = v; else if (rd == 30) cop0_errorepc = v;
        return;
    }
    stop("interp: unimplemented COP0 0x%08x at 0x%x", w, pc);
}

uint32_t ee_eret(Ctx *c) {
    (void)c; uint32_t t;
    if (cop0_status & 4) { t = cop0_errorepc; cop0_status &= ~4u; } else { t = cop0_epc; cop0_status &= ~2u; }
    return t;
}
/* ---------------- VU0 macro mode */
#define VF(n) c->vf[n]
static float vclamp(float x) { return fpu_clamp(x); }
static void vwrite(Ctx *c, int fd, int dest, const float r[4]) {
    if (fd == 0) return;
    for (int i = 0; i < 4; i++) if (dest & (8 >> i)) c->vf[fd][i] = vclamp(r[i]);
}
void ee_lqc2(Ctx *c, int ft, uint32_t a) { GPR g; rd128(a, &g); if (ft) memcpy(c->vf[ft], &g, 16); }
void ee_sqc2(Ctx *c, int ft, uint32_t a) { GPR g; memcpy(&g, c->vf[ft], 16); wr128(a, &g); }
void ee_cop2(Ctx *c, uint32_t pc, uint32_t w) {
    int rs = (w >> 21) & 31, ft = (w >> 16) & 31, fs = (w >> 11) & 31, fd = (w >> 6) & 31, fn = w & 63;
    int dest = (w >> 21) & 15, bc = w & 3;
    float r[4], *S = c->vf[fs], *T = c->vf[ft], *A = c->vacc; int i;
    if (rs < 16) {
        switch (rs) {
        case 1: { GPR g; memcpy(&g, c->vf[fs], 16); if (ft) c->r[ft] = g; return; }          /* qmfc2 */
        case 5: if (fs) memcpy(c->vf[fs], &c->r[ft], 16); return;                             /* qmtc2 */
        case 2: { uint32_t v = fs < 16 ? c->vi[fs] : fs == 22 ? 0 : fs == 29 ? 0 : c->vi[fs];      /* cfc2 */
                  if (fs == 22) memcpy(&v, &c->vq, 4); if (ft) c->r[ft].s64[0] = (int32_t)v; return; }
        case 6: if (fs) c->vi[fs] = c->r[ft].u32[0]; if (fs == 22) memcpy(&c->vq, &c->r[ft].u32[0], 4); return; /* ctc2 */
        }
        stop("interp: unimplemented COP2 0x%08x at 0x%x", w, pc);
    }
    if (fn < 60) {
        float bv = T[bc];
        switch (fn) {
        case 0: case 1: case 2: case 3: for (i = 0; i < 4; i++) r[i] = S[i] + bv; vwrite(c, fd, dest, r); return;        /* vaddbc */
        case 4: case 5: case 6: case 7: for (i = 0; i < 4; i++) r[i] = S[i] - bv; vwrite(c, fd, dest, r); return;        /* vsubbc */
        case 8: case 9: case 10: case 11: for (i = 0; i < 4; i++) r[i] = A[i] + S[i] * bv; vwrite(c, fd, dest, r); return; /* vmaddbc */
        case 12: case 13: case 14: case 15: for (i = 0; i < 4; i++) r[i] = A[i] - S[i] * bv; vwrite(c, fd, dest, r); return; /* vmsubbc */
        case 16: case 17: case 18: case 19: for (i = 0; i < 4; i++) r[i] = fmaxf(S[i], bv); vwrite(c, fd, dest, r); return; /* vmaxbc */
        case 20: case 21: case 22: case 23: for (i = 0; i < 4; i++) r[i] = fminf(S[i], bv); vwrite(c, fd, dest, r); return; /* vminibc */
        case 24: case 25: case 26: case 27: for (i = 0; i < 4; i++) r[i] = S[i] * bv; vwrite(c, fd, dest, r); return;     /* vmulbc */
        case 28: for (i = 0; i < 4; i++) r[i] = S[i] * c->vq; vwrite(c, fd, dest, r); return;     /* vmulq */
        case 32: for (i = 0; i < 4; i++) r[i] = S[i] + c->vq; vwrite(c, fd, dest, r); return;     /* vaddq */
        case 40: for (i = 0; i < 4; i++) r[i] = S[i] + T[i]; vwrite(c, fd, dest, r); return;      /* vadd */
        case 41: for (i = 0; i < 4; i++) r[i] = A[i] + S[i] * T[i]; vwrite(c, fd, dest, r); return; /* vmadd */
        case 42: for (i = 0; i < 4; i++) r[i] = S[i] * T[i]; vwrite(c, fd, dest, r); return;      /* vmul */
        case 43: for (i = 0; i < 4; i++) r[i] = fmaxf(S[i], T[i]); vwrite(c, fd, dest, r); return;
        case 44: for (i = 0; i < 4; i++) r[i] = S[i] - T[i]; vwrite(c, fd, dest, r); return;      /* vsub */
        case 45: for (i = 0; i < 4; i++) r[i] = A[i] - S[i] * T[i]; vwrite(c, fd, dest, r); return;
        case 46: r[0] = A[0] - S[1] * T[2]; r[1] = A[1] - S[2] * T[0]; r[2] = A[2] - S[0] * T[1]; r[3] = 0; vwrite(c, fd, dest & 0xE, r); return; /* vopmsub */
        case 47: for (i = 0; i < 4; i++) r[i] = fminf(S[i], T[i]); vwrite(c, fd, dest, r); return;
        }
    } else {
        int op = (fn & 3) | (fd << 2);
        float bv = T[bc];
        switch (op) {
        case 0: case 1: case 2: case 3: for (i = 0; i < 4; i++) if (dest & (8 >> i)) A[i] = vclamp(S[i] + bv); return;   /* vaddabc */
        case 4: case 5: case 6: case 7: for (i = 0; i < 4; i++) if (dest & (8 >> i)) A[i] = vclamp(S[i] - bv); return;
        case 8: case 9: case 10: case 11: for (i = 0; i < 4; i++) if (dest & (8 >> i)) A[i] = vclamp(A[i] + S[i] * bv); return; /* vmaddabc */
        case 12: case 13: case 14: case 15: for (i = 0; i < 4; i++) if (dest & (8 >> i)) A[i] = vclamp(A[i] - S[i] * bv); return;
        case 24: case 25: case 26: case 27: for (i = 0; i < 4; i++) if (dest & (8 >> i)) A[i] = vclamp(S[i] * bv); return; /* vmulabc */
        case 16: case 17: case 18: case 19: { float sc = op == 16 ? 1 : op == 17 ? 1.f / 16 : op == 18 ? 1.f / 4096 : 1.f / 32768; /* vitofN */
            for (i = 0; i < 4; i++) { int32_t v; memcpy(&v, &S[i], 4); r[i] = v * sc; } vwrite(c, ft, dest, r); return; }
        case 20: case 21: case 22: case 23: { float sc = op == 20 ? 1 : op == 21 ? 16 : op == 22 ? 4096 : 32768;          /* vftoiN */
            if (!ft) return; for (i = 0; i < 4; i++) if (dest & (8 >> i)) { int32_t v = ee_cvtw(S[i] * sc); memcpy(&c->vf[ft][i], &v, 4); } return; }
        case 40: for (i = 0; i < 4; i++) if (dest & (8 >> i)) A[i] = vclamp(S[i] + T[i]); return;   /* vadda */
        case 41: for (i = 0; i < 4; i++) if (dest & (8 >> i)) A[i] = vclamp(A[i] + S[i] * T[i]); return; /* vmadda */
        case 42: for (i = 0; i < 4; i++) if (dest & (8 >> i)) A[i] = vclamp(S[i] * T[i]); return;   /* vmula */
        case 46: A[0] = S[1] * T[2]; A[1] = S[2] * T[0]; A[2] = S[0] * T[1]; return;                 /* vopmula */
        case 47: return;                                                                               /* vnop */
        case 48: for (i = 0; i < 4; i++) r[i] = S[i]; vwrite(c, ft, dest, r); return;                 /* vmove */
        case 49: r[0] = S[1]; r[1] = S[2]; r[2] = S[3]; r[3] = S[0]; vwrite(c, ft, dest, r); return;  /* vmr32 */
        case 56: { int fsf = (w >> 21) & 3, ftf = (w >> 23) & 3; c->vq = ee_divs(S[fsf], T[ftf]); return; } /* vdiv */
        case 57: { int ftf = (w >> 23) & 3; c->vq = sqrtf(fabsf(T[ftf])); return; }                  /* vsqrt */
        case 58: { int fsf = (w >> 21) & 3, ftf = (w >> 23) & 3; c->vq = ee_divs(S[fsf], sqrtf(fabsf(T[ftf]))); return; } /* vrsqrt */
        case 59: return;                                                                               /* vwaitq */
        }
    }
    stop("interp: unimplemented VU0 macro op 0x%08x at 0x%x", w, pc);
}
