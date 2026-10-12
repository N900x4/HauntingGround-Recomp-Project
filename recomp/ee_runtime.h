/* Minimal EE runtime for the rec.py PoC: context, memory accessors, EE-specific arithmetic helpers. */
#ifndef EE_RUNTIME_H
#define EE_RUNTIME_H
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <float.h>

typedef union { uint64_t u64[2]; int64_t s64[2]; uint32_t u32[4]; int32_t s32[4]; uint16_t u16[8]; uint8_t u8[16]; } GPR;
typedef struct {
    GPR r[32];
    uint64_t hi, lo, hi1, lo1;   /* pipeline 0 and pipeline 1 HI/LO (lower 64 bits; MMI uses the full 128) */
    uint32_t sa;
    union { float f[32]; uint32_t fi[32]; };
    float acc; int fcc; uint32_t fcr31;
    uint32_t ret;
    float vf[32][4]; uint32_t vi[32]; float vacc[4]; float vq;   /* bootgate: VU0 macro-mode state */                /* bootgate: guest return address seen at the last jr ra */
} Ctx;
typedef void (*RecFn)(Ctx *);
typedef struct { uint32_t addr; RecFn fn; } RecEntry;

extern uint8_t *ee_ram;                  /* 32 MB main RAM */
/* bootgate: fast path for user-segment RAM (0x00000000-0x01FFFFFF); everything else (KSEG/uncached mirrors,
   scratchpad 0x70000000, EE/GS/VU registers, unmapped) goes through the host slow path in runtime.c */
uint64_t hw_read(uint32_t a, int size);
void hw_write(uint32_t a, uint64_t v, int size);
void hw_read128(uint32_t a, GPR *r);
void hw_write128(uint32_t a, const GPR *r);
#define RAMOK(a) __builtin_expect((a) < 0x02000000u, 1)
static inline uint8_t  rd8 (uint32_t a) { if (RAMOK(a)) return ee_ram[a]; return (uint8_t)hw_read(a, 1); }
static inline uint16_t rd16(uint32_t a) { if (RAMOK(a)) { uint16_t v; memcpy(&v, ee_ram + a, 2); return v; } return (uint16_t)hw_read(a, 2); }
static inline uint32_t rd32(uint32_t a) { if (RAMOK(a)) { uint32_t v; memcpy(&v, ee_ram + a, 4); return v; } return (uint32_t)hw_read(a, 4); }
static inline uint64_t rd64(uint32_t a) { if (RAMOK(a)) { uint64_t v; memcpy(&v, ee_ram + a, 8); return v; } return hw_read(a, 8); }
static inline void rd128(uint32_t a, GPR *r) { a &= ~15u; if (RAMOK(a)) memcpy(r, ee_ram + a, 16); else hw_read128(a, r); }
static inline void wr8 (uint32_t a, uint8_t v)  { if (RAMOK(a)) ee_ram[a] = v; else hw_write(a, v, 1); }
static inline void wr16(uint32_t a, uint16_t v) { if (RAMOK(a)) memcpy(ee_ram + a, &v, 2); else hw_write(a, v, 2); }
static inline void wr32(uint32_t a, uint32_t v) { if (RAMOK(a)) memcpy(ee_ram + a, &v, 4); else hw_write(a, v, 4); }
static inline void wr64(uint32_t a, uint64_t v) { if (RAMOK(a)) memcpy(ee_ram + a, &v, 8); else hw_write(a, v, 8); }
static inline void wr128(uint32_t a, const GPR *r) { a &= ~15u; if (RAMOK(a)) memcpy(ee_ram + a, r, 16); else hw_write128(a, r); }

/* EE FPU is not IEEE: no Inf/NaN/denormals; overflow saturates to +-FLT_MAX, denormals flush to 0,
   rounding is toward zero (not modelled here: would need fesetround(FE_TOWARDZERO) or soft-float). */
static inline float fpu_clamp(float x) {
    uint32_t u; memcpy(&u, &x, 4);
    uint32_t e = u & 0x7F800000u;
    if (e == 0x7F800000u) { u = (u & 0x80000000u) | 0x7F7FFFFFu; memcpy(&x, &u, 4); }
    else if (e == 0) { u &= 0x80000000u; memcpy(&x, &u, 4); }
    return x;
}
static inline float ee_divs(float a, float b) {
    if (b == 0.0f) { uint32_t ua, ub; memcpy(&ua, &a, 4); memcpy(&ub, &b, 4); uint32_t u = ((ua ^ ub) & 0x80000000u) | 0x7F7FFFFFu; float r; memcpy(&r, &u, 4); return r; }
    return fpu_clamp(a / b);
}
static inline int32_t ee_cvtw(float f) {           /* EE cvt.w.s truncates and saturates */
    if (f >= 2147483648.0f) return 0x7FFFFFFF;
    if (f <= -2147483648.0f) return (int32_t)0x80000000;
    return (int32_t)f;
}
static inline void ee_mult(Ctx *c, int p, int rd, int32_t a, int32_t b) {
    int64_t r = (int64_t)a * b; uint64_t lo = (uint64_t)(int64_t)(int32_t)r, hi = (uint64_t)(int64_t)(int32_t)(r >> 32);
    if (p) { c->lo1 = lo; c->hi1 = hi; } else { c->lo = lo; c->hi = hi; }
    if (rd) c->r[rd].u64[0] = lo;
}
static inline void ee_multu(Ctx *c, int p, int rd, uint32_t a, uint32_t b) {
    uint64_t r = (uint64_t)a * b; uint64_t lo = (uint64_t)(int64_t)(int32_t)r, hi = (uint64_t)(int64_t)(int32_t)(r >> 32);
    if (p) { c->lo1 = lo; c->hi1 = hi; } else { c->lo = lo; c->hi = hi; }
    if (rd) c->r[rd].u64[0] = lo;
}
static inline void ee_madd(Ctx *c, int p, int rd, int32_t a, int32_t b) {
    uint64_t *L = p ? &c->lo1 : &c->lo, *H = p ? &c->hi1 : &c->hi;
    int64_t acc = (int64_t)(((uint64_t)(uint32_t)*H << 32) | (uint32_t)*L) + (int64_t)a * b;
    *L = (uint64_t)(int64_t)(int32_t)acc; *H = (uint64_t)(int64_t)(int32_t)(acc >> 32);
    if (rd) c->r[rd].u64[0] = *L;
}
static inline void ee_maddu(Ctx *c, int p, int rd, uint32_t a, uint32_t b) {
    uint64_t *L = p ? &c->lo1 : &c->lo, *H = p ? &c->hi1 : &c->hi;
    uint64_t acc = (((uint64_t)(uint32_t)*H << 32) | (uint32_t)*L) + (uint64_t)a * b;
    *L = (uint64_t)(int64_t)(int32_t)acc; *H = (uint64_t)(int64_t)(int32_t)(acc >> 32);
    if (rd) c->r[rd].u64[0] = *L;
}
static inline void ee_div(Ctx *c, int p, int32_t a, int32_t b) {
    int32_t q, r;
    if (b == 0) { q = a < 0 ? 1 : -1; r = a; } else if (a == INT32_MIN && b == -1) { q = INT32_MIN; r = 0; } else { q = a / b; r = a % b; }
    if (p) { c->lo1 = (uint64_t)(int64_t)q; c->hi1 = (uint64_t)(int64_t)r; } else { c->lo = (uint64_t)(int64_t)q; c->hi = (uint64_t)(int64_t)r; }
}
static inline void ee_divu(Ctx *c, int p, uint32_t a, uint32_t b) {
    uint32_t q = b ? a / b : 0xFFFFFFFFu, r = b ? a % b : a;
    if (p) { c->lo1 = (uint64_t)(int64_t)(int32_t)q; c->hi1 = (uint64_t)(int64_t)(int32_t)r; } else { c->lo = (uint64_t)(int64_t)(int32_t)q; c->hi = (uint64_t)(int64_t)(int32_t)r; }
}
static inline void ee_pmfhl(Ctx *c, int rd, int fmt) {  /* only .lw (fmt 0) implemented */
    if (fmt == 0) { GPR t; t.u32[0] = (uint32_t)c->lo; t.u32[1] = (uint32_t)c->hi; t.u32[2] = (uint32_t)c->lo1; t.u32[3] = (uint32_t)c->hi1; c->r[rd] = t; }
}
static inline void ee_plzcw(Ctx *c, int rd, int rs) {
    for (int i = 0; i < 2; i++) { uint32_t v = c->r[rs].u32[i]; if (v & 0x80000000u) v = ~v; int n = 0; while (n < 32 && !(v & (0x80000000u >> n))) n++; c->r[rd].u32[i] = n - 1; }
}
/* unaligned loads/stores (little endian) */
static inline void ee_lwl(Ctx *c, int rt, uint32_t a) { int sh = a & 3; uint32_t m = rd32(a & ~3u); uint32_t v = (uint32_t)c->r[rt].u64[0];
    v = (v & (0x00FFFFFFu >> (sh * 8))) | (m << (24 - sh * 8)); c->r[rt].s64[0] = (int32_t)v; }
static inline void ee_lwr(Ctx *c, int rt, uint32_t a) { int sh = a & 3; uint32_t m = rd32(a & ~3u); uint32_t v = (uint32_t)c->r[rt].u64[0];
    v = sh ? (v & (0xFFFFFF00u << (24 - sh * 8))) | (m >> (sh * 8)) : m; c->r[rt].s64[0] = (int32_t)v; }
static inline void ee_ldl(Ctx *c, int rt, uint32_t a) { int sh = a & 7; uint64_t m = rd64(a & ~7u); uint64_t v = c->r[rt].u64[0];
    c->r[rt].u64[0] = sh == 7 ? m : (v & (0x00FFFFFFFFFFFFFFull >> (sh * 8))) | (m << (56 - sh * 8)); }
static inline void ee_ldr(Ctx *c, int rt, uint32_t a) { int sh = a & 7; uint64_t m = rd64(a & ~7u); uint64_t v = c->r[rt].u64[0];
    c->r[rt].u64[0] = sh ? (v & (0xFFFFFFFFFFFFFF00ull << (56 - sh * 8))) | (m >> (sh * 8)) : m; }
static inline void ee_swl(Ctx *c, int rt, uint32_t a) { int sh = a & 3; uint32_t m = rd32(a & ~3u), v = (uint32_t)c->r[rt].u64[0];
    wr32(a & ~3u, (m & (0xFFFFFF00u << (sh * 8))) | (v >> (24 - sh * 8))); }
static inline void ee_swr(Ctx *c, int rt, uint32_t a) { int sh = a & 3; uint32_t m = rd32(a & ~3u), v = (uint32_t)c->r[rt].u64[0];
    wr32(a & ~3u, (m & (0x00FFFFFFu >> (24 - sh * 8))) | (v << (sh * 8))); }
static inline void ee_sdl(Ctx *c, int rt, uint32_t a) { int sh = a & 7; uint64_t m = rd64(a & ~7u), v = c->r[rt].u64[0];
    wr64(a & ~7u, sh == 7 ? v : (m & (0xFFFFFFFFFFFFFF00ull << (sh * 8))) | (v >> (56 - sh * 8))); }
static inline void ee_sdr(Ctx *c, int rt, uint32_t a) { int sh = a & 7; uint64_t m = rd64(a & ~7u), v = c->r[rt].u64[0];
    wr64(a & ~7u, sh ? (m & (0x00FFFFFFFFFFFFFFull >> (56 - sh * 8))) | (v << (sh * 8)) : v); }

/* provided by the host runtime / harness */
void rec_call(Ctx *ctx, uint32_t addr);                 /* dispatcher for indirect + out-of-unit calls */
void rec_bad_jump(Ctx *ctx, uint32_t pc, uint32_t t);
void rec_unimpl(Ctx *ctx, uint32_t pc, uint32_t insn);
void rec_syscall(Ctx *ctx, uint32_t pc);
void rec_break(Ctx *ctx, uint32_t pc);
extern const RecEntry rec_table[];
extern uint32_t cop0_status;
void ee_ei_hook(void);
static inline void ee_ei(Ctx *c) { (void)c; cop0_status |= 0x10000u; ee_ei_hook(); }
static inline void ee_di(Ctx *c) { (void)c; cop0_status &= ~0x10000u; }
uint32_t ee_eret(Ctx *c);
void ee_mmi(Ctx *c, uint32_t pc, uint32_t w);
void ee_cop0(Ctx *c, uint32_t pc, uint32_t w);
void ee_cop2(Ctx *c, uint32_t pc, uint32_t w);
void ee_lqc2(Ctx *c, int ft, uint32_t a);
void ee_sqc2(Ctx *c, int ft, uint32_t a);
/* bootgate instrumentation */
typedef int (*RecHook)(Ctx *);           /* returns 1 if the hook replaced the function */
extern uint64_t rec_cnt[];
extern RecHook rec_hook[];
extern uint32_t rec_cur;
void rec_ret_mismatch(Ctx *ctx, uint32_t expect);
void rec_fallthrough(Ctx *ctx, uint32_t from, uint32_t to);
void rec_first(int i);
#define REC_ENTER(i, addr) do { if (__builtin_expect(rec_cnt[i]++ == 0, 0)) rec_first(i); rec_cur = (addr); \
    if (__builtin_expect(rec_hook[i] != 0, 0) && rec_hook[i](ctx)) { ctx->ret = ctx->r[31].u32[0]; return; } } while (0)
extern volatile int vblank_due;
void hle_async_tick(void);
#define REC_POLL() do { if (__builtin_expect(vblank_due, 0)) hle_async_tick(); } while (0)
#define REC_CHKRET(e) do { if (__builtin_expect(ctx->ret != (e), 0)) rec_ret_mismatch(ctx, (e)); } while (0)
#define REC_FALLTHROUGH(from, to) do { rec_fallthrough(ctx, from, to); return; } while (0)
#endif
