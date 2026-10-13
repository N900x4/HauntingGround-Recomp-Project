/* VU1 micro-mode interpreter.
   Scope: the 19 VU1 programs Haunting Ground uploads (docs/hardware.md 1.1): FMAC upper ops, integer/load-store/
   branch lower ops, DIV/SQRT/RSQRT with Q latency, CLIP + clip-flag ops, MAC/status flags, XTOP/XITOP, XGKICK.
   Timing model: one instruction per cycle; FMAC results are visible at once (the hardware interlocks on VF reads),
   Q becomes visible 7 (DIV/SQRT) or 13 (RSQRT) instructions after issue or at WAITQ, and a conditional branch
   reads the old value of a VI register written by the instruction right before it.
   Float model: IEEE single precision per lane, with VU clamping (an exponent of 255 on input or output becomes
   +-FLT_MAX). EFU ops are decoded but only ESUM/ERSQRT-style results are approximated (the game uses none).

   Implementation: each 64-bit instruction is decoded once into a Dec record (decoded again after MPG uploads change
   micro memory), and the FMAC ops run on all four lanes with SSE, doing per lane exactly the operations of the
   straightforward per-lane interpreter in vu1_ref.c, which HG_VU1CHECK=1 runs in lockstep to verify this one. */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <float.h>
#include <stdlib.h>
#include <emmintrin.h>
#include "ee_runtime.h"
#include "hle.h"
#include "vu1_state.h"

void gif_qword(int path, const uint64_t q[2]);
int gif_path_done(int path);

uint8_t vu1_mem[16384];          /* data memory, 1024 qwords */
uint8_t vu1_code[16384];         /* micro memory, 2048 instructions */
uint32_t vu1_tpc;                /* next instruction (index) for MSCNT */
uint32_t vu1_top, vu1_itop;
uint32_t vu1_code_gen = 1;       /* bumped by MPG uploads: decoded instructions are revalidated */

typedef union { float f[4]; uint32_t u[4]; int32_t s[4]; __m128 v; __m128i vi; } Vec;
static Vec vf[32], acc;
static uint16_t vi[16];
static float q_reg, p_reg, i_reg, q_pend, p_pend; static int q_wait, p_wait;
static uint32_t r_reg = 0x3F800000;
static uint32_t mac, status, clip;
static uint64_t n_runs, n_insns, n_kicks, kick_hash;

static inline float clampf(float x) {
    if (isnan(x)) return signbit(x) ? -FLT_MAX : FLT_MAX;
    if (isinf(x)) return x > 0 ? FLT_MAX : -FLT_MAX;
    return x;
}
static inline float rdf(uint32_t u) {           /* VU has no Inf/NaN: treat exponent 255 as max */
    if ((u & 0x7F800000u) == 0x7F800000u) u = (u & 0x80000000u) | 0x7F7FFFFFu;
    float f; memcpy(&f, &u, 4); return f;
}
/* rdf() and clampf() on four lanes: both turn an exponent of 255 (Inf or NaN) into sign | FLT_MAX */
static inline __m128 vclamp(__m128 x) {
    const __m128i b = _mm_castps_si128(x), e = _mm_set1_epi32(0x7F800000);
    const __m128i m = _mm_cmpeq_epi32(_mm_and_si128(b, e), e);
    const __m128i r = _mm_or_si128(_mm_and_si128(b, _mm_set1_epi32((int)0x80000000u)), _mm_set1_epi32(0x7F7FFFFF));
    return _mm_castsi128_ps(_mm_or_si128(_mm_and_si128(m, r), _mm_andnot_si128(m, b)));
}
static inline Vec *vmem(uint32_t qaddr) { return (Vec *)(vu1_mem + ((qaddr & 0x3FF) << 4)); }
static __m128i lane_mask[16];   /* dest field (x = bit 3) -> per-lane all-ones mask */
static uint8_t rev4[16];        /* bit i <-> bit 3-i */
static inline void put_lanes(Vec *d, __m128 v, int dest) {
    const __m128i m = lane_mask[dest];
    d->vi = _mm_or_si128(_mm_and_si128(m, _mm_castps_si128(v)), _mm_andnot_si128(m, d->vi));
}
static inline void lanes_copy(Vec *dst, const Vec *src, int dest) {
    dst->vi = _mm_or_si128(_mm_and_si128(lane_mask[dest], src->vi), _mm_andnot_si128(lane_mask[dest], dst->vi));
}

/* Flag pipeline: an FMAC instruction's MAC/status and CLIP results reach the flag registers flag_delay
   instructions later (the FMAC pipeline; flags do not interlock like VF registers do), so FMxxx/FSxxx/FCxxx read the
   flags of an older instruction. HG_VUFLAGDELAY=4 enables this; the default 0 keeps flags immediate, because the
   interpreter does not model stalls (the game only reads clip flags, and stage 1 renders the same either way). */
typedef struct { uint64_t due; int clip; uint32_t val; } FlagUpd;
static FlagUpd fq[16]; static int fq_n; static uint64_t vcyc; static int flag_delay = -1;
static inline void apply_mac(uint32_t m) {
    mac = m;
    uint32_t s = 0;
    if (mac & 0x000F) s |= 1; if (mac & 0x00F0) s |= 2; if (mac & 0x0F00) s |= 4; if (mac & 0xF000) s |= 8;
    status = (status & 0xFF0) | s | (s << 6);
}
static inline void flag_push(int is_clip, uint32_t val) {
    if (!flag_delay) { if (is_clip) clip = ((clip << 6) | val) & 0xFFFFFF; else apply_mac(val); return; }
    if (fq_n == 16) { if (fq[0].clip) clip = ((clip << 6) | fq[0].val) & 0xFFFFFF; else apply_mac(fq[0].val); memmove(fq, fq + 1, sizeof fq[0] * 15); fq_n--; }
    fq[fq_n].due = vcyc + (uint64_t)flag_delay; fq[fq_n].clip = is_clip; fq[fq_n].val = val; fq_n++;
}
static inline void flag_commit(int all) {
    int k = 0;
    while (k < fq_n && (all || fq[k].due <= vcyc)) { if (fq[k].clip) clip = ((clip << 6) | fq[k].val) & 0xFFFFFF; else apply_mac(fq[k].val); k++; }
    if (k) { memmove(fq, fq + k, sizeof fq[0] * (size_t)(fq_n - k)); fq_n -= k; }
}
/* MAC flags: bit 3-i = zero(field i), 7-i = sign, 11-i = underflow, 15-i = overflow (field 0 = x); only dest fields */
static inline void set_flags(__m128 v, int dest) {
    const __m128i b = _mm_castps_si128(v);
    const int z = _mm_movemask_ps(_mm_cmpeq_ps(v, _mm_setzero_ps()));
    const int sg = _mm_movemask_ps(v) & ~z;
    const int ov = _mm_movemask_ps(_mm_castsi128_ps(_mm_cmpeq_epi32(_mm_and_si128(b, _mm_set1_epi32(0x7FFFFFFF)), _mm_set1_epi32(0x7F7FFFFF))));
    flag_push(0, (rev4[z] & dest) | (rev4[sg] & dest) << 4 | (rev4[ov] & dest) << 12);
}

/* ---------------------------------------------------------------- decode */
enum { U_NOP, U_ARITH, U_OPMULA, U_OPMSUB, U_ITOF, U_FTOI, U_ABS, U_CLIP };
enum { K_ADD, K_SUB, K_MADD, K_MSUB, K_MAX, K_MINI, K_MUL };
enum { B_VEC, B_BC, B_Q, B_I };
enum { L_NONE, L_LQ, L_SQ, L_ILW, L_ISW, L_IADDIU, L_ISUBIU, L_FCEQ, L_FCSET, L_FCAND, L_FCOR, L_FSEQ, L_FSSET, L_FSAND, L_FSOR,
       L_FMEQ, L_FMAND, L_FMOR, L_FCGET, L_B, L_BAL, L_JR, L_JALR, L_IBEQ, L_IBNE, L_IBLTZ, L_IBGTZ, L_IBLEZ, L_IBGEZ,
       L_IADD, L_ISUB, L_IADDI, L_IAND, L_IOR, L_MOVE, L_MR32, L_LQI, L_SQI, L_LQD, L_SQD, L_DIV, L_SQRT, L_RSQRT, L_WAITQ,
       L_MTIR, L_MFIR, L_ILWR, L_ISWR, L_RNEXT, L_RGET, L_RINIT, L_RXOR, L_MFP, L_XTOP, L_XITOP, L_XGKICK, L_WAITP, L_EFU,
       L_BADSPECIAL, L_UNKNOWN };
typedef struct {
    uint8_t uk, kind, bsrc, bc, dest, fs, ft, fd, to_acc, flags, sc;                 /* upper */
    uint8_t lk, ldest, lft, lfs, lit, lis, lid, fsf, ftf, sop, ibit, ebit;           /* lower */
    int32_t imm;
    uint32_t lraw, iimm, gen;
} Dec;
static Dec dec[2048];

static void decode_upper(uint32_t u, Dec *d) {
    const int op = u & 63;
    d->dest = (u >> 21) & 15; d->ft = (u >> 16) & 31; d->fs = (u >> 11) & 31; d->fd = (u >> 6) & 31;
    d->uk = U_NOP; d->to_acc = 0; d->flags = 0; d->bsrc = B_VEC; d->bc = 0; d->kind = K_ADD; d->sc = 0;
    if ((op & 0x3C) != 0x3C) {                       /* result to fd */
        d->uk = U_ARITH;
        if (op < 0x1C) { d->kind = op >> 2; d->bsrc = B_BC; d->bc = op & 3; }
        else switch (op) {
            case 0x1C: d->kind = K_MUL; d->bsrc = B_Q; break;  case 0x1D: d->kind = K_MAX; d->bsrc = B_I; break;
            case 0x1E: d->kind = K_MUL; d->bsrc = B_I; break;  case 0x1F: d->kind = K_MINI; d->bsrc = B_I; break;
            case 0x20: d->kind = K_ADD; d->bsrc = B_Q; break;  case 0x21: d->kind = K_MADD; d->bsrc = B_Q; break;
            case 0x22: d->kind = K_ADD; d->bsrc = B_I; break;  case 0x23: d->kind = K_MADD; d->bsrc = B_I; break;
            case 0x24: d->kind = K_SUB; d->bsrc = B_Q; break;  case 0x25: d->kind = K_MSUB; d->bsrc = B_Q; break;
            case 0x26: d->kind = K_SUB; d->bsrc = B_I; break;  case 0x27: d->kind = K_MSUB; d->bsrc = B_I; break;
            case 0x28: d->kind = K_ADD; break; case 0x29: d->kind = K_MADD; break; case 0x2A: d->kind = K_MUL; break; case 0x2B: d->kind = K_MAX; break;
            case 0x2C: d->kind = K_SUB; break; case 0x2D: d->kind = K_MSUB; break; case 0x2F: d->kind = K_MINI; break;
            case 0x2E: d->uk = U_OPMSUB; d->flags = 1; return;
            default: d->kind = K_MUL; break;         /* 0x30-0x3B: as the per-lane interpreter (multiply by ft) */
        }
        d->flags = d->kind != K_MAX && d->kind != K_MINI;
        return;
    }
    const int sop = ((u >> 6) & 31) << 2 | (u & 3);     /* result to ACC unless noted */
    if (sop < 0x10) { d->uk = U_ARITH; d->to_acc = 1; d->flags = 1; d->kind = sop >> 2; d->bsrc = B_BC; d->bc = sop & 3; return; }
    if (sop >= 0x18 && sop < 0x1C) { d->uk = U_ARITH; d->to_acc = 1; d->flags = 1; d->kind = K_MUL; d->bsrc = B_BC; d->bc = sop & 3; return; }
    switch (sop) {
    case 0x10: case 0x11: case 0x12: case 0x13: d->uk = U_ITOF; d->sc = sop & 3; return;
    case 0x14: case 0x15: case 0x16: case 0x17: d->uk = U_FTOI; d->sc = sop & 3; return;
    case 0x1D: d->uk = U_ABS; return;
    case 0x1F: d->uk = U_CLIP; return;
    case 0x2E: d->uk = U_OPMULA; d->to_acc = 1; d->flags = 1; return;
    case 0x2F: return;   /* NOP */
    }
    d->uk = U_ARITH; d->to_acc = 1; d->flags = 1;
    switch (sop) {
    case 0x1C: d->kind = K_MUL; d->bsrc = B_Q; return; case 0x1E: d->kind = K_MUL; d->bsrc = B_I; return;
    case 0x20: d->kind = K_ADD; d->bsrc = B_Q; return; case 0x21: d->kind = K_MADD; d->bsrc = B_Q; return;
    case 0x22: d->kind = K_ADD; d->bsrc = B_I; return; case 0x23: d->kind = K_MADD; d->bsrc = B_I; return;
    case 0x24: d->kind = K_SUB; d->bsrc = B_Q; return; case 0x25: d->kind = K_MSUB; d->bsrc = B_Q; return;
    case 0x26: d->kind = K_SUB; d->bsrc = B_I; return; case 0x27: d->kind = K_MSUB; d->bsrc = B_I; return;
    case 0x28: d->kind = K_ADD; return; case 0x29: d->kind = K_MADD; return; case 0x2A: d->kind = K_MUL; return;
    case 0x2C: d->kind = K_SUB; return; case 0x2D: d->kind = K_MSUB; return;
    }
    d->uk = U_NOP; d->to_acc = 0; d->flags = 0;      /* unknown: no effect, as the per-lane interpreter */
}
static void decode_lower(uint32_t l, Dec *d) {
    const int op = l >> 25;
    d->lraw = l; d->ldest = (l >> 21) & 15; d->lft = (l >> 16) & 31; d->lfs = (l >> 11) & 31; d->lit = d->lft & 15; d->lis = d->lfs & 15;
    d->lid = (l >> 6) & 15; d->fsf = (l >> 21) & 3; d->ftf = (l >> 23) & 3; d->sop = 0;
    d->imm = (int32_t)(l << 21) >> 21; d->lk = L_UNKNOWN;
    const int32_t imm15 = ((l >> 10) & 0x7800) | (l & 0x7FF), imm12 = ((l >> 10) & 0x800) | (l & 0x7FF);
    switch (op) {
    case 0x00: d->lk = L_LQ; return; case 0x01: d->lk = L_SQ; return; case 0x04: d->lk = L_ILW; return; case 0x05: d->lk = L_ISW; return;
    case 0x08: d->lk = L_IADDIU; d->imm = imm15; return; case 0x09: d->lk = L_ISUBIU; d->imm = imm15; return;
    case 0x10: d->lk = L_FCEQ; return; case 0x11: d->lk = L_FCSET; return; case 0x12: d->lk = L_FCAND; return; case 0x13: d->lk = L_FCOR; return;
    case 0x14: d->lk = L_FSEQ; d->imm = imm12; return; case 0x15: d->lk = L_FSSET; d->imm = imm12; return;
    case 0x16: d->lk = L_FSAND; d->imm = imm12; return; case 0x17: d->lk = L_FSOR; d->imm = imm12; return;
    case 0x18: d->lk = L_FMEQ; return; case 0x1A: d->lk = L_FMAND; return; case 0x1B: d->lk = L_FMOR; return; case 0x1C: d->lk = L_FCGET; return;
    case 0x20: d->lk = L_B; return; case 0x21: d->lk = L_BAL; return; case 0x24: d->lk = L_JR; return; case 0x25: d->lk = L_JALR; return;
    case 0x28: d->lk = L_IBEQ; return; case 0x29: d->lk = L_IBNE; return; case 0x2C: d->lk = L_IBLTZ; return; case 0x2D: d->lk = L_IBGTZ; return;
    case 0x2E: d->lk = L_IBLEZ; return; case 0x2F: d->lk = L_IBGEZ; return;
    case 0x40: break;
    default: return;
    }
    const int f6 = l & 63;
    switch (f6) {
    case 0x30: d->lk = L_IADD; return; case 0x31: d->lk = L_ISUB; return; case 0x32: d->lk = L_IADDI; d->imm = (int32_t)(l << 21) >> 27; return;
    case 0x34: d->lk = L_IAND; return; case 0x35: d->lk = L_IOR; return;
    }
    if ((f6 & 0x3C) != 0x3C) { d->lk = L_BADSPECIAL; return; }
    const int sop = ((l >> 6) & 31) << 2 | (l & 3);
    d->sop = sop;
    switch (sop) {
    case 0x30: d->lk = L_MOVE; return; case 0x31: d->lk = L_MR32; return; case 0x34: d->lk = L_LQI; return; case 0x35: d->lk = L_SQI; return;
    case 0x36: d->lk = L_LQD; return; case 0x37: d->lk = L_SQD; return; case 0x38: d->lk = L_DIV; return; case 0x39: d->lk = L_SQRT; return;
    case 0x3A: d->lk = L_RSQRT; return; case 0x3B: d->lk = L_WAITQ; return; case 0x3C: d->lk = L_MTIR; return; case 0x3D: d->lk = L_MFIR; return;
    case 0x3E: d->lk = L_ILWR; return; case 0x3F: d->lk = L_ISWR; return; case 0x40: d->lk = L_RNEXT; return; case 0x41: d->lk = L_RGET; return;
    case 0x42: d->lk = L_RINIT; return; case 0x43: d->lk = L_RXOR; return; case 0x64: d->lk = L_MFP; return; case 0x68: d->lk = L_XTOP; return;
    case 0x69: d->lk = L_XITOP; return; case 0x6C: d->lk = L_XGKICK; return; case 0x7B: d->lk = L_WAITP; return;
    case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75: case 0x76: case 0x78: case 0x79: case 0x7A: case 0x7C: case 0x7D: case 0x7E:
        d->lk = L_EFU; return;
    }
}
static inline const Dec *decoded(uint32_t pc) {
    Dec *d = &dec[pc];
    if (__builtin_expect(d->gen != vu1_code_gen, 0)) {
        uint32_t lo, up; memcpy(&lo, vu1_code + pc * 8, 4); memcpy(&up, vu1_code + pc * 8 + 4, 4);
        decode_upper(up, d);
        d->ibit = up >> 31; d->ebit = (up >> 30) & 1; d->iimm = lo;
        if (d->ibit) d->lk = L_NONE; else decode_lower(lo, d);
        d->gen = vu1_code_gen;
    }
    return d;
}

/* ---------------------------------------------------------------- upper (FMAC) */
typedef struct { __m128 v; int valid, to_acc, reg, dest, flags; } UpRes;
static inline void upper(const Dec *d, UpRes *r) {
    r->valid = 0;
    switch (d->uk) {
    case U_NOP: return;
    case U_ARITH: {
        const __m128 a = vclamp(vf[d->fs].v);
        __m128 b;
        switch (d->bsrc) {
        case B_VEC: b = vclamp(vf[d->ft].v); break;
        case B_BC: b = _mm_set1_ps(rdf(vf[d->ft].u[d->bc])); break;
        case B_Q: b = _mm_set1_ps(q_reg); break;
        default: b = _mm_set1_ps(i_reg); break;
        }
        __m128 x;
        switch (d->kind) {
        case K_ADD: x = _mm_add_ps(a, b); break;
        case K_SUB: x = _mm_sub_ps(a, b); break;
        case K_MADD: x = _mm_add_ps(vclamp(acc.v), _mm_mul_ps(a, b)); break;
        case K_MSUB: x = _mm_sub_ps(vclamp(acc.v), _mm_mul_ps(a, b)); break;
        case K_MAX: x = _mm_max_ps(a, b); break;     /* a > b ? a : b */
        case K_MINI: x = _mm_min_ps(a, b); break;    /* a < b ? a : b */
        default: x = _mm_mul_ps(a, b); break;
        }
        r->v = vclamp(x); r->valid = 1; r->to_acc = d->to_acc; r->reg = d->fd; r->dest = d->dest; r->flags = d->flags; return; }
    case U_OPMSUB: {   /* fd = acc - fs x ft (cross) */
        const Vec S = vf[d->fs], T = vf[d->ft]; Vec out;
        out.f[0] = clampf(rdf(acc.u[0]) - rdf(S.u[1]) * rdf(T.u[2])); out.f[1] = clampf(rdf(acc.u[1]) - rdf(S.u[2]) * rdf(T.u[0]));
        out.f[2] = clampf(rdf(acc.u[2]) - rdf(S.u[0]) * rdf(T.u[1])); out.f[3] = 0;
        r->v = out.v; r->valid = 1; r->to_acc = 0; r->reg = d->fd; r->dest = 0xE; r->flags = 1; return; }
    case U_OPMULA: {   /* acc = fs x ft (cross, positive half) */
        const Vec S = vf[d->fs], T = vf[d->ft]; Vec out; out.vi = _mm_setzero_si128();
        out.f[0] = clampf(rdf(S.u[1]) * rdf(T.u[2])); out.f[1] = clampf(rdf(S.u[2]) * rdf(T.u[0])); out.f[2] = clampf(rdf(S.u[0]) * rdf(T.u[1]));
        r->v = out.v; r->valid = 1; r->to_acc = 1; r->dest = 0xE; r->flags = 1; return; }
    case U_ITOF: {
        static const float sc[4] = { 1.0f, 1.0f / 16, 1.0f / 4096, 1.0f / 32768 };
        r->v = _mm_mul_ps(_mm_cvtepi32_ps(vf[d->fs].vi), _mm_set1_ps(sc[d->sc]));
        r->valid = 1; r->to_acc = 0; r->reg = d->ft; r->dest = d->dest; r->flags = 0; return; }
    case U_FTOI: {
        static const float sc[4] = { 1.0f, 16.0f, 4096.0f, 32768.0f };
        const Vec S = vf[d->fs]; Vec out;
        for (int i = 0; i < 4; i++) { double x = (double)rdf(S.u[i]) * sc[d->sc];
            out.s[i] = x >= 2147483647.0 ? 0x7FFFFFFF : x <= -2147483648.0 ? (int32_t)0x80000000 : (int32_t)x; }
        r->v = out.v; r->valid = 1; r->to_acc = 0; r->reg = d->ft; r->dest = d->dest; r->flags = 0; return; }
    case U_ABS:
        r->v = _mm_castsi128_ps(_mm_and_si128(_mm_castps_si128(vclamp(vf[d->fs].v)), _mm_set1_epi32(0x7FFFFFFF)));
        r->valid = 1; r->to_acc = 0; r->reg = d->ft; r->dest = d->dest; r->flags = 0; return;
    case U_CLIP: {   /* CLIP fs.xyz against ft.w */
        const Vec S = vf[d->fs], T = vf[d->ft];
        float w = fabsf(rdf(T.u[3])); uint32_t c = 0;
        float x = rdf(S.u[0]), y = rdf(S.u[1]), z = rdf(S.u[2]);
        if (x > w) c |= 1; if (x < -w) c |= 2; if (y > w) c |= 4; if (y < -w) c |= 8; if (z > w) c |= 16; if (z < -w) c |= 32;
        if (getenv("HG_CLIPLOG")) { static int n; if (n++ < 40) L("VU1 CLIP @%u fs vf%d (%g %g %g %g) ft vf%d.w %g -> %02x\n", 0, d->fs, x, y, z, rdf(S.u[3]), d->ft, rdf(T.u[3]), c); }
        flag_push(1, c); return; }
    }
}
static inline void upper_commit(const UpRes *r) {
    if (!r->valid) return;
    if (r->to_acc) put_lanes(&acc, r->v, r->dest);
    else if (r->reg) put_lanes(&vf[r->reg], r->v, r->dest);   /* if the lower op wrote the same VF, the upper result wins */
    if (r->flags) set_flags(r->v, r->dest);
}

/* ---------------------------------------------------------------- lower */
static void q_commit(void) { if (q_wait) { q_reg = q_pend; q_wait = 0; } }
static void p_commit(void) { if (p_wait) { p_reg = p_pend; p_wait = 0; } }
static void p_issue(float v, int lat) { p_commit(); p_pend = clampf(v); p_wait = lat; }
static void q_issue(float v, int lat) { q_commit(); q_pend = clampf(v); q_wait = lat; }

/* Pipeline stalls (on by default; HG_VUSTALL=0 turns them off): a VF result (upper FMAC or lower-unit write) becomes readable 4 cycles after
   issue, and an instruction pair that reads a field before then waits; a DIV/SQRT/RSQRT waits for a busy FDIV.
   Q, P and flag latencies then elapse in cycles (stalls included), not in instructions. */
static int vu_stall = -1;
static uint64_t vf_rdy[32][4], acc_rdy;
static inline uint64_t rdy(int r, int mask) {
    uint64_t m = 0; if (!r) return 0;
    for (int i = 0; i < 4; i++) if ((mask & (8 >> i)) && vf_rdy[r][i] > m) m = vf_rdy[r][i];
    return m;
}
static inline void set_rdy(int r, int mask, uint64_t c) { if (r) for (int i = 0; i < 4; i++) if (mask & (8 >> i)) vf_rdy[r][i] = c; }
static inline uint64_t mx(uint64_t a, uint64_t b) { return a > b ? a : b; }
static int stall_cycles(const Dec *d) {
    uint64_t need = 0;
    switch (d->uk) {
    case U_ARITH:
        need = rdy(d->fs, d->dest);
        if (d->bsrc == B_VEC) need = mx(need, rdy(d->ft, d->dest)); else if (d->bsrc == B_BC) need = mx(need, rdy(d->ft, 8 >> d->bc));
        if (d->kind == K_MADD || d->kind == K_MSUB) need = mx(need, acc_rdy);
        break;
    case U_OPMULA: need = mx(rdy(d->fs, 0xE), rdy(d->ft, 0xE)); break;
    case U_OPMSUB: need = mx(mx(rdy(d->fs, 0xE), rdy(d->ft, 0xE)), acc_rdy); break;
    case U_ITOF: case U_FTOI: case U_ABS: need = rdy(d->fs, d->dest); break;
    case U_CLIP: need = mx(rdy(d->fs, 0xE), rdy(d->ft, 1)); break;
    }
    if (!d->ibit) switch (d->lk) {
    case L_SQ: case L_SQI: case L_SQD: case L_MOVE: need = mx(need, rdy(d->lfs, d->ldest)); break;
    case L_MR32: case L_EFU: need = mx(need, rdy(d->lfs, 0xF)); break;
    case L_MTIR: case L_RINIT: case L_RXOR: need = mx(need, rdy(d->lfs, 8 >> d->fsf)); break;
    case L_DIV: case L_RSQRT: need = mx(need, mx(rdy(d->lfs, 8 >> d->fsf), rdy(d->lft, 8 >> d->ftf))); /* fall through */
    case L_SQRT: if (d->lk == L_SQRT) need = mx(need, rdy(d->lft, 8 >> d->ftf));
        if (q_wait > 1) need = mx(need, vcyc + (uint64_t)q_wait - 1);
        break;
    }
    return need > vcyc ? (int)(need - vcyc) : 0;
}
static void note_writes(const Dec *d) {
    const uint64_t c = vcyc + 4;
    switch (d->uk) {
    case U_ARITH: if (d->to_acc) acc_rdy = c; else set_rdy(d->fd, d->dest, c); break;
    case U_OPMULA: acc_rdy = c; break;
    case U_OPMSUB: set_rdy(d->fd, 0xE, c); break;
    case U_ITOF: case U_FTOI: case U_ABS: set_rdy(d->ft, d->dest, c); break;
    }
    if (!d->ibit) switch (d->lk) {
    case L_LQ: case L_LQI: case L_LQD: case L_MOVE: case L_MR32: case L_MFIR: case L_MFP: case L_RGET: case L_RNEXT: set_rdy(d->lft, d->ldest, c); break;
    }
}
static int kick_check;
static void kick(uint32_t addr) {
    n_kicks++;
    for (int guard = 0; guard < 1024; guard++) {
        uint64_t q[2]; memcpy(q, vu1_mem + ((addr & 0x3FF) << 4), 16); addr++;
        if (kick_check) { kick_hash = (kick_hash ^ q[0]) * 0x100000001B3ull; kick_hash = (kick_hash ^ q[1]) * 0x100000001B3ull; }
        gif_qword(1, q);
        if (gif_path_done(1)) break;
    }
}
/* returns 1 if a branch is taken (target in *tgt). *wvi = VI register written (-1: none) and *wold its previous value */
static uint64_t lk_count[64];
static inline int lower(const Dec *d, uint32_t pc, int prev_vi, uint16_t prev_old, int *wvi, uint16_t *wold, uint32_t *tgt) {
    lk_count[d->lk]++;
    const int dest = d->ldest, ft = d->lft, fs = d->lfs, it = d->lit, is = d->lis;
#define SETVI(r, v) do { const int r_ = (r) & 15; if (r_) { const uint16_t nv_ = (uint16_t)(v); *wold = vi[r_]; *wvi = r_; vi[r_] = nv_; } } while (0)
#define VIB(r) ((r) == prev_vi ? prev_old : vi[r])     /* branch operand: old value if written by the previous instruction */
    switch (d->lk) {
    case L_NONE: return 0;
    case L_LQ: { const Vec *m = vmem(vi[is] + d->imm); if (ft) lanes_copy(&vf[ft], m, dest); return 0; }
    case L_SQ: { Vec *m = vmem(vi[it] + d->imm); lanes_copy(m, &vf[fs], dest); return 0; }
    case L_ILW: { const Vec *m = vmem(vi[is] + d->imm); for (int i = 0; i < 4; i++) if (dest & (8 >> i)) { SETVI(it, m->u[i] & 0xFFFF); break; } return 0; }
    case L_ISW: { Vec *m = vmem(vi[is] + d->imm); for (int i = 0; i < 4; i++) if (dest & (8 >> i)) m->u[i] = vi[it]; return 0; }
    case L_IADDIU: SETVI(it, vi[is] + d->imm); return 0;
    case L_ISUBIU: SETVI(it, vi[is] - d->imm); return 0;
    case L_FCEQ: SETVI(1, (clip & 0xFFFFFF) == (d->lraw & 0xFFFFFF)); return 0;
    case L_FCSET: clip = d->lraw & 0xFFFFFF; return 0;
    case L_FCAND: SETVI(1, (clip & d->lraw & 0xFFFFFF) != 0); return 0;
    case L_FCOR: SETVI(1, ((clip | d->lraw) & 0xFFFFFF) == 0xFFFFFF); return 0;
    case L_FSEQ: SETVI(it, (status & 0xFFF) == (uint32_t)d->imm); return 0;
    case L_FSSET: status = (status & 0x3F) | (d->imm & 0xFC0); return 0;
    case L_FSAND: SETVI(it, status & d->imm); return 0;
    case L_FSOR: SETVI(it, status | d->imm); return 0;
    case L_FMEQ: SETVI(it, (mac & 0xFFFF) == vi[is]); return 0;
    case L_FMAND: SETVI(it, mac & vi[is]); return 0;
    case L_FMOR: SETVI(it, mac | vi[is]); return 0;
    case L_FCGET: SETVI(it, clip & 0xFFF); return 0;
    case L_B: *tgt = pc + 1 + d->imm; return 1;
    case L_BAL: SETVI(it, pc + 2); *tgt = pc + 1 + d->imm; return 1;
    case L_JR: *tgt = VIB(is); return 1;
    case L_JALR: { const uint32_t t = VIB(is); SETVI(it, pc + 2); *tgt = t; return 1; }
    case L_IBEQ: if (VIB(it) == VIB(is)) { *tgt = pc + 1 + d->imm; return 1; } return 0;
    case L_IBNE: if (VIB(it) != VIB(is)) { *tgt = pc + 1 + d->imm; return 1; } return 0;
    case L_IBLTZ: if ((int16_t)VIB(is) < 0) { *tgt = pc + 1 + d->imm; return 1; } return 0;
    case L_IBGTZ: if ((int16_t)VIB(is) > 0) { *tgt = pc + 1 + d->imm; return 1; } return 0;
    case L_IBLEZ: if ((int16_t)VIB(is) <= 0) { *tgt = pc + 1 + d->imm; return 1; } return 0;
    case L_IBGEZ: if ((int16_t)VIB(is) >= 0) { *tgt = pc + 1 + d->imm; return 1; } return 0;
    case L_IADD: SETVI(d->lid, vi[is] + vi[it]); return 0;
    case L_ISUB: SETVI(d->lid, vi[is] - vi[it]); return 0;
    case L_IADDI: SETVI(it, vi[is] + d->imm); return 0;
    case L_IAND: SETVI(d->lid, vi[is] & vi[it]); return 0;
    case L_IOR: SETVI(d->lid, vi[is] | vi[it]); return 0;
    case L_MOVE: if (ft) lanes_copy(&vf[ft], &vf[fs], dest); return 0;
    case L_MR32: if (ft) { Vec s; s.vi = _mm_shuffle_epi32(vf[fs].vi, _MM_SHUFFLE(0, 3, 2, 1)); lanes_copy(&vf[ft], &s, dest); } return 0;
    case L_LQI: { const Vec *m = vmem(vi[is]); if (ft) lanes_copy(&vf[ft], m, dest); SETVI(is, vi[is] + 1); return 0; }
    case L_SQI: { Vec *m = vmem(vi[it]); lanes_copy(m, &vf[fs], dest); SETVI(it, vi[it] + 1); return 0; }
    case L_LQD: { SETVI(is, vi[is] - 1); const Vec *m = vmem(vi[is]); if (ft) lanes_copy(&vf[ft], m, dest); return 0; }
    case L_SQD: { SETVI(it, vi[it] - 1); Vec *m = vmem(vi[it]); lanes_copy(m, &vf[fs], dest); return 0; }
    case L_DIV: { const float a = rdf(vf[fs].u[d->fsf]), b = rdf(vf[ft].u[d->ftf]);
        status &= ~0x30u;
        if (b == 0.0f) { status |= a == 0.0f ? 0x10 : 0x20; q_issue((signbit(a) ^ signbit(b)) ? -FLT_MAX : FLT_MAX, 7); } else q_issue(a / b, 7); return 0; }
    case L_SQRT: { const float b = rdf(vf[ft].u[d->ftf]); status &= ~0x30u; if (b < 0) status |= 0x10; q_issue(sqrtf(fabsf(b)), 7); return 0; }
    case L_RSQRT: { const float a = rdf(vf[fs].u[d->fsf]), b = rdf(vf[ft].u[d->ftf]); status &= ~0x30u;
        if (b == 0.0f) { status |= 0x20; q_issue(FLT_MAX, 13); } else { if (b < 0) status |= 0x10; q_issue(a / sqrtf(fabsf(b)), 13); } return 0; }
    case L_WAITQ: q_commit(); return 0;
    case L_MTIR: SETVI(it, vf[fs].u[d->fsf] & 0xFFFF); return 0;
    case L_MFIR: if (ft) { for (int i = 0; i < 4; i++) if (dest & (8 >> i)) vf[ft].s[i] = (int16_t)vi[is]; } return 0;
    case L_ILWR: { const Vec *m = vmem(vi[is]); for (int i = 0; i < 4; i++) if (dest & (8 >> i)) { SETVI(it, m->u[i] & 0xFFFF); break; } return 0; }
    case L_ISWR: { Vec *m = vmem(vi[is]); for (int i = 0; i < 4; i++) if (dest & (8 >> i)) m->u[i] = vi[it]; return 0; }
    case L_RNEXT: r_reg = (r_reg ^ (r_reg << 1 | ((r_reg >> 4 ^ r_reg >> 22) & 1))) & 0x7FFFFF; r_reg |= 0x3F800000;
        /* fall through */
    case L_RGET: if (ft) { for (int i = 0; i < 4; i++) if (dest & (8 >> i)) vf[ft].u[i] = r_reg; } return 0;
    case L_RINIT: r_reg = 0x3F800000 | (vf[fs].u[d->fsf] & 0x7FFFFF); return 0;
    case L_RXOR: r_reg = 0x3F800000 | ((r_reg ^ vf[fs].u[d->fsf]) & 0x7FFFFF); return 0;
    case L_MFP: if (ft) { for (int i = 0; i < 4; i++) if (dest & (8 >> i)) vf[ft].f[i] = p_reg; } return 0;
    case L_XTOP: SETVI(it, vu1_top); return 0;
    case L_XITOP: SETVI(it, vu1_itop); return 0;
    case L_XGKICK: kick(vi[is]); return 0;
    case L_WAITP: p_commit(); return 0;
    case L_EFU: {   /* result to P after the documented latency */
        const float x = rdf(vf[fs].u[0]), y = rdf(vf[fs].u[1]), z = rdf(vf[fs].u[2]), w = rdf(vf[fs].u[3]), f = rdf(vf[fs].u[d->fsf]);
        const float sq = x * x + y * y + z * z;
        switch (d->sop) {
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
    case L_BADSPECIAL: L("VU1: unknown special op 0x%02x (0x%08x) at %u\n", d->lraw & 63, d->lraw, pc); return 0;
    default:
        if ((d->lraw >> 25) == 0x40) L("VU1: unknown special2 op 0x%02x (0x%08x) at %u\n", d->sop, d->lraw, pc);
        else L("VU1: unknown lower op 0x%02x (0x%08x) at %u\n", d->lraw >> 25, d->lraw, pc);
        return 0;
    }
#undef SETVI
#undef VIB
}

/* ---------------------------------------------------------------- run */
static void vu1_run_inner(uint32_t start) {
    n_runs++;
    uint32_t pc = start & 2047;
    int ebit_pending = 0, branch_pending = 0; uint32_t branch_tgt = 0;
    int prev_vi = -1; uint16_t prev_old = 0;
    vf[0].f[0] = vf[0].f[1] = vf[0].f[2] = 0; vf[0].f[3] = 1.0f; vi[0] = 0;
    if (vu_stall) { memset(vf_rdy, 0, sizeof vf_rdy); acc_rdy = 0; }   /* the pipeline has drained between program runs */
    for (long n = 0; n < 4000000; n++) {
        const Dec *d = decoded(pc);
        n_insns++; vcyc++;
        int st = vu_stall ? stall_cycles(d) : 0;
        vcyc += (uint64_t)st;
        if (fq_n) flag_commit(0);
        if (q_wait) { q_wait -= 1 + st; if (q_wait <= 0) { q_wait = 0; q_reg = q_pend; } }
        if (p_wait) { p_wait -= 1 + st; if (p_wait <= 0) { p_wait = 0; p_reg = p_pend; } }
        /* WAITQ/WAITP stall the whole instruction pair until the result arrives, so an upper op in the same pair
           (e.g. MULq next to WAITQ in the game's epilogue) already sees the new Q or P. */
        if (!d->ibit) { if (d->lk == L_WAITQ) q_commit(); else if (d->lk == L_WAITP) p_commit(); }
        UpRes ur; upper(d, &ur);
        int wvi = -1, taken = 0; uint16_t wold = 0; uint32_t tgt = 0;
        if (d->ibit) memcpy(&i_reg, &d->iimm, 4);           /* I bit: lower word is a float immediate */
        else taken = lower(d, pc, prev_vi, prev_old, &wvi, &wold, &tgt);
        upper_commit(&ur);
        if (vu_stall) note_writes(d);
        vi[0] = 0; vf[0].f[0] = vf[0].f[1] = vf[0].f[2] = 0; vf[0].f[3] = 1.0f;
        prev_vi = wvi; prev_old = wold;
        uint32_t next = (pc + 1) & 2047;
        if (branch_pending) { next = branch_tgt & 2047; branch_pending = 0; }
        if (taken) { branch_pending = 1; branch_tgt = tgt; }
        if (ebit_pending) { vu1_tpc = next; q_commit(); p_commit(); flag_commit(1); return; }
        if (d->ebit) ebit_pending = 1;                      /* E bit: run the next instruction, then stop */
        pc = next;
    }
    L("VU1: program at %u did not end\n", start);
    vu1_tpc = pc;
}

/* ---------------------------------------------------------------- lockstep check against vu1_ref.c (HG_VU1CHECK=1) */
void vu1ref_run(uint32_t start); void vu1ref_set(const Vu1State *s); void vu1ref_get(Vu1State *s); uint32_t vu1ref_tpc(void);
extern uint64_t vu1ref_kick_hash;
static void get_state(Vu1State *s) {
    memset(s, 0, sizeof *s);
    for (int r = 0; r < 32; r++) memcpy(s->vf[r], vf[r].u, 16);
    memcpy(s->acc, acc.u, 16); memcpy(s->vi, vi, sizeof vi);
    s->q = q_reg; s->p = p_reg; s->i = i_reg; s->q_pend = q_pend; s->p_pend = p_pend; s->q_wait = q_wait; s->p_wait = p_wait;
    s->r = r_reg; s->mac = mac; s->status = status; s->clip = clip;
}
static void check_run(uint32_t start) {
    static Vu1State before, after_new, after_ref; static uint8_t mem_before[16384], mem_new[16384];
    static long mismatches, checked;
    get_state(&before); memcpy(mem_before, vu1_mem, sizeof mem_before);
    kick_check = 1; kick_hash = 0xcbf29ce484222325ull;
    vu1_run_inner(start);
    kick_check = 0;
    get_state(&after_new); memcpy(mem_new, vu1_mem, sizeof mem_new); const uint32_t tpc_new = vu1_tpc;
    memcpy(vu1_mem, mem_before, sizeof mem_before);
    vu1ref_set(&before); vu1ref_run(start); memset(&after_ref, 0, sizeof after_ref); vu1ref_get(&after_ref);
    checked++;
    const int memdiff = memcmp(mem_new, vu1_mem, sizeof mem_new) != 0;
    if ((memcmp(&after_new, &after_ref, sizeof after_new) || memdiff || tpc_new != vu1ref_tpc() || kick_hash != vu1ref_kick_hash) && mismatches++ < 20) {
        L("VU1CHECK: mismatch in run %ld (start %u): tpc %u/%u kick %016llx/%016llx data memory %s\n", checked, start, tpc_new, vu1ref_tpc(),
          (unsigned long long)kick_hash, (unsigned long long)vu1ref_kick_hash, memdiff ? "differs" : "same");
        for (int r = 0; r < 32; r++) if (memcmp(after_new.vf[r], after_ref.vf[r], 16))
            L("  vf%02d new %08x %08x %08x %08x ref %08x %08x %08x %08x\n", r, after_new.vf[r][0], after_new.vf[r][1], after_new.vf[r][2], after_new.vf[r][3],
              after_ref.vf[r][0], after_ref.vf[r][1], after_ref.vf[r][2], after_ref.vf[r][3]);
        for (int r = 0; r < 16; r++) if (after_new.vi[r] != after_ref.vi[r]) L("  vi%02d new %04x ref %04x\n", r, after_new.vi[r], after_ref.vi[r]);
        if (memcmp(after_new.acc, after_ref.acc, 16)) L("  acc differs\n");
        if (after_new.mac != after_ref.mac || after_new.status != after_ref.status || after_new.clip != after_ref.clip)
            L("  mac %x/%x status %x/%x clip %x/%x\n", after_new.mac, after_ref.mac, after_new.status, after_ref.status, after_new.clip, after_ref.clip);
    }
    if (checked % 100000 == 0) L("VU1CHECK: %ld runs checked, %ld mismatches\n", checked, mismatches);
    memcpy(vu1_mem, mem_new, sizeof mem_new);   /* continue with this interpreter's results */
}

void vu1_run(uint32_t start) {
    static int init, check;
    if (!init) {
        init = 1; check = getenv("HG_VU1CHECK") != 0;
        flag_delay = getenv("HG_VUFLAGDELAY") ? atoi(getenv("HG_VUFLAGDELAY")) : 0;
        vu_stall = getenv("HG_VUSTALL") ? atoi(getenv("HG_VUSTALL")) : 1;   /* 4 matches the FMAC pipeline, but stalls are not modelled: off until checked against a reference */
        for (int m = 0; m < 16; m++) {
            rev4[m] = (uint8_t)(((m & 1) << 3) | ((m & 2) << 1) | ((m & 4) >> 1) | ((m & 8) >> 3));
            lane_mask[m] = _mm_setr_epi32(m & 8 ? -1 : 0, m & 4 ? -1 : 0, m & 2 ? -1 : 0, m & 1 ? -1 : 0);
        }
    }
    {   /* HG_VU1DUMP=path:vb writes micro and data memory (code first) at the first run from that VBlank on */
        static long dump_vb = -2; static const char *dump_path;
        if (dump_vb == -2) { const char *e = getenv("HG_VU1DUMP"); dump_vb = -1; if (e) { static char buf[512]; snprintf(buf, sizeof buf, "%s", e); char *c = strrchr(buf, ':'); if (c) { *c = 0; dump_vb = atol(c + 1); dump_path = buf; } } }
        extern uint64_t hle_vblank_count(void);
        if (dump_vb >= 0 && (long)hle_vblank_count() >= dump_vb) {
            FILE *f = fopen(dump_path, "wb"); if (f) { fwrite(vu1_code, 1, sizeof vu1_code, f); fwrite(vu1_mem, 1, sizeof vu1_mem, f); fclose(f); }
            L("VU1DUMP at VB %llu, start pc %u\n", (unsigned long long)hle_vblank_count(), start); dump_vb = -1;
        }
    }
    double t0 = host_now();
    if (check) check_run(start); else vu1_run_inner(start);
    tprof[0] += host_now() - t0;
}
void vu1_dump_stats(void) {
    static const char *nm[] = { [L_FCEQ]="FCEQ", [L_FCSET]="FCSET", [L_FCAND]="FCAND", [L_FCOR]="FCOR", [L_FSEQ]="FSEQ", [L_FSSET]="FSSET", [L_FSAND]="FSAND", [L_FSOR]="FSOR",
        [L_FMEQ]="FMEQ", [L_FMAND]="FMAND", [L_FMOR]="FMOR", [L_FCGET]="FCGET" };
    L("VU1 flag reads:"); for (int i = 0; i < 64; i++) if (i < (int)(sizeof nm / sizeof nm[0]) && nm[i] && lk_count[i]) L(" %s %llu", nm[i], (unsigned long long)lk_count[i]); L("\n"); L("VU1: %llu runs, %llu instructions, %llu XGKICKs\n", (unsigned long long)n_runs, (unsigned long long)n_insns, (unsigned long long)n_kicks); }
