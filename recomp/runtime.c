/* bootgate native runtime for the bulk-recompiled SLUS_210.75.
   - 32 MB EE RAM, 16 KB scratchpad, trapped EE/GS/VU register space
   - ELF load, entry at 0x100008 (crt0 does SetupThread/SetupHeap itself)
   - dispatcher for indirect jumps (address -> host function), unknown target = stop
   - EE kernel HLE: syscalls, cooperative priority threads (ucontext fibers), semaphores,
     INTC/DMAC handlers fired from a virtual VBlank clock when all threads block / on CSR polls
   - instrumentation: unique functions entered, total calls, first-entry order, syscall and HW-register counts */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <signal.h>
#include <ucontext.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <execinfo.h>
#include "ee_runtime.h"
#include "hle.h"

uint8_t *ee_ram;
static uint8_t spr[0x4000];
static uint8_t hwreg[0x10000];         /* 0x10000000-0x1000FFFF */
uint8_t *hwreg_base(void) { return hwreg; }
static uint8_t gspriv[0x2000];         /* 0x12000000-0x12001FFF */
static uint8_t vumem[0x10000];         /* 0x11000000-0x1100FFFF */

#define MAXF 16384
uint64_t rec_cnt[MAXF];
RecHook rec_hook[MAXF];
uint32_t rec_cur;
extern const int rec_nfuncs;
extern const uint32_t rec_func_end[];
static uint64_t prof_hist[MAXF];
static FILE *logf_;
static int stopping_flag;
static int first_order[MAXF], n_first;
static double t_start;
int bg_verbose = 1;

const char *game_path(const char *name) {
    static char buf[4][1024]; static int k; char *b = buf[k++ & 3];
    snprintf(b, 1024, "%s/%s", getenv("HG_GAME_DIR") ? getenv("HG_GAME_DIR") : "../game", name); return b;
}
double tprof[8];
double host_now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }
static long log_bytes; static int log_capped;
#define LOG_CAP (8L << 20)            /* cap per run; the stop summary bypasses it */
void L(const char *fmt, ...) {
    if (log_bytes > LOG_CAP && !stopping_flag) { if (!log_capped++) fprintf(logf_, "... [log capped at %ld bytes] ...\n", LOG_CAP); return; }
    va_list ap; va_start(ap, fmt); log_bytes += vfprintf(logf_, fmt, ap); va_end(ap); fflush(logf_); }

int func_index(uint32_t a) {               /* exact match, -1 if none */
    int lo = 0, hi = rec_nfuncs - 1;
    while (lo <= hi) { int m = (lo + hi) / 2; if (rec_table[m].addr == a) return m; if (rec_table[m].addr < a) lo = m + 1; else hi = m - 1; }
    return -1;
}
int func_containing(uint32_t a) {
    int lo = 0, hi = rec_nfuncs - 1, r = -1;
    while (lo <= hi) { int m = (lo + hi) / 2; if (rec_table[m].addr <= a) { r = m; lo = m + 1; } else hi = m - 1; }
    if (r >= 0 && a < rec_func_end[r]) return r;
    return -1;
}
void set_hook(uint32_t a, RecHook h) { int i = func_index(a); if (i < 0) { fprintf(stderr, "hook: no func %x\n", a); exit(1); } rec_hook[i] = h; }

/* ------------------------------------------------------------------ stats / stop */
static uint64_t sys_cnt[512];
typedef struct { uint32_t addr; uint64_t r, w; uint64_t lastv; } HwStat;
static HwStat hwst[4096]; static int nhw;
static HwStat *hwstat(uint32_t a) {
    for (int i = 0; i < nhw; i++) if (hwst[i].addr == a) return &hwst[i];
    if (nhw < 4096) { hwst[nhw].addr = a; return &hwst[nhw++]; }
    return &hwst[4095];
}
static int cmp_hw(const void *a, const void *b) { const HwStat *x = a, *y = b; return x->addr < y->addr ? -1 : x->addr > y->addr; }
static int stopping;
void dump_stats(const char *why) {
    if (stopping++) return;
    stopping_flag = 1;
    uint64_t tot = 0; int uniq = 0;
    for (int i = 0; i < rec_nfuncs; i++) { tot += rec_cnt[i]; if (rec_cnt[i]) uniq++; }
    L("\n==== STOP: %s\n", why);
    L("cur func 0x%08x, wall %.2fs\n", rec_cur, now() - t_start);
    L("unique functions entered: %d / %d, total calls: %llu\n", uniq, rec_nfuncs, (unsigned long long)tot);
    qsort(hwst, nhw, sizeof hwst[0], cmp_hw);
    L("hw registers (addr reads writes last):\n"); for (int i = 0; i < nhw; i++) L("  %08x r=%llu w=%llu last=0x%llx\n", hwst[i].addr, (unsigned long long)hwst[i].r, (unsigned long long)hwst[i].w, (unsigned long long)hwst[i].lastv);
    FILE *f = fopen("funcs_entered.txt", "w");
    if (f) { for (int k = 0; k < n_first; k++) { int i = first_order[k]; fprintf(f, "%d 0x%08x %llu\n", k, rec_table[i].addr, (unsigned long long)rec_cnt[i]); } fclose(f); }
    /* profile */
    uint64_t pt = 0; for (int i = 0; i < rec_nfuncs; i++) pt += prof_hist[i];
    if (pt) { L("profile samples %llu, top:\n", (unsigned long long)pt);
        for (int k = 0; k < 15; k++) { int b = -1; for (int i = 0; i < rec_nfuncs; i++) if (prof_hist[i] && (b < 0 || prof_hist[i] > prof_hist[b])) b = i;
            if (b < 0) break; L("  0x%08x %llu\n", rec_table[b].addr, (unsigned long long)prof_hist[b]); prof_hist[b] = 0; } }
    { double tot = now() - t_start; L("host time: total %.1fs, VU1 %.1fs, GS draw %.1fs, IOP+SPU2 %.1fs (SPU2 mix %.1fs), DMA/VIF %.1fs, rest (EE code, HLE, output) %.1fs\n",
        tot, tprof[0], tprof[1], tprof[2], tprof[3], tprof[4], tot - tprof[0] - tprof[1] - tprof[2] - tprof[4]); }
    hle_dump();
    if (getenv("HG_PEEK")) { char *p = getenv("HG_PEEK"); while (*p) { uint32_t a = strtoul(p, &p, 16); uint32_t v; memcpy(&v, ee_ram + (a & 0x1FFFFFF), 4); L("peek %08x = %08x\n", a, v); while (*p == ',') p++; } }
}
void stop(const char *fmt, ...) {
    char buf[512]; va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    fprintf(stderr, "STOP: %s\n", buf);
    dump_stats(buf); exit(0);
}
void rec_first(int i) { if (n_first < MAXF) first_order[n_first++] = i; }

/* ------------------------------------------------------------------ dispatcher */
volatile int vblank_due;
uint64_t ee_cycles, ee_next_vb = 1ull << 62, ee_vb_period;
int timing_wall;   /* HG_TIMING=wall: VBlank from the host clock (old behaviour) */
extern void hle_async_tick(void);
void rec_call(Ctx *ctx, uint32_t addr) {
    if (__builtin_expect(vblank_due, 0)) hle_async_tick();
    int i = func_index(addr);
    if (i < 0) {
        int c = func_containing(addr);
        if (c >= 0) stop("indirect/out-of-unit jump into the middle of function 0x%x (target 0x%x) from 0x%x", rec_table[c].addr, addr, rec_cur);
        stop("indirect call to unknown target 0x%08x from func 0x%x (ra=0x%x)", addr, rec_cur, ctx->r[31].u32[0]);
    }
    rec_table[i].fn(ctx);
}
void rec_bad_jump(Ctx *ctx, uint32_t pc, uint32_t t) { (void)ctx; stop("bad jump table target 0x%x at 0x%x", t, pc); }
void rec_unimpl(Ctx *ctx, uint32_t pc, uint32_t insn) { (void)ctx; stop("unimplemented instruction 0x%08x at 0x%x", insn, pc); }
void rec_break(Ctx *ctx, uint32_t pc) { (void)ctx; stop("break (divide by zero trap) at 0x%x", pc); }
void rec_ret_mismatch(Ctx *ctx, uint32_t expect) { stop("return-address mismatch: expected 0x%x, callee returned via jr ra=0x%x (in 0x%x)", expect, ctx->ret, rec_cur); }
static uint64_t n_fall;
void rec_fallthrough(Ctx *ctx, uint32_t from, uint32_t to) {
    if (n_fall++ < 50) L("fallthrough: function 0x%x ran into 0x%x\n", from, to);
    rec_call(ctx, to);
}

/* ------------------------------------------------------------------ memory slow path */
static uint64_t n_unmapped;
static uint8_t *ram_alias(uint32_t a) {
    uint32_t seg = a >> 28;
    if ((seg == 2 || seg == 3) && (a & 0x0FFFFFFF) < 0x02000000) return ee_ram + (a & 0x01FFFFFF);
    if ((seg == 8 || seg == 9 || seg == 0xA || seg == 0xB) && (a & 0x1FFFFFFF) < 0x02000000) return ee_ram + (a & 0x01FFFFFF);
    if (a >= 0x70000000 && a < 0x70004000) return spr + (a - 0x70000000);
    return 0;
}
static uint64_t ld(uint8_t *p, int size) { uint64_t v = 0; memcpy(&v, p, size); return v; }
static void st(uint8_t *p, uint64_t v, int size) { memcpy(p, &v, size); }

uint64_t hw_read(uint32_t a, int size) {
    uint8_t *p = ram_alias(a);
    if (p) return ld(p, size);
    uint32_t pa = a & 0x1FFFFFFF;
    if (pa >= 0x10000000 && pa < 0x10010000) { HwStat *s = hwstat(pa); s->r++; uint64_t v = hle_hw_read(pa, ld(hwreg + (pa & 0xFFFF), size), size); s->lastv = v; return v; }
    if (pa >= 0x12000000 && pa < 0x12002000) { HwStat *s = hwstat(pa); s->r++; uint64_t v = hle_gs_read(pa, ld(gspriv + (pa & 0x1FFF), size), size); s->lastv = v; return v; }
    if (pa >= 0x11000000 && pa < 0x11010000) { HwStat *s = hwstat(pa & ~0xFFF); s->r++; return ld(vumem + (pa & 0xFFFF), size); }
    HwStat *s = hwstat(a); s->r++;
    if (n_unmapped++ < 100) L("unmapped read%d 0x%08x in 0x%x\n", size * 8, a, rec_cur);
    return 0;
}
void hw_write(uint32_t a, uint64_t v, int size) {
    uint8_t *p = ram_alias(a);
    if (p) { st(p, v, size); return; }
    uint32_t pa = a & 0x1FFFFFFF;
    if (pa >= 0x10000000 && pa < 0x10010000) { HwStat *s = hwstat(pa); s->w++; s->lastv = v; st(hwreg + (pa & 0xFFFF), v, size); hle_hw_write(pa, v, size, hwreg); return; }
    if (pa >= 0x12000000 && pa < 0x12002000) { HwStat *s = hwstat(pa); s->w++; s->lastv = v; st(gspriv + (pa & 0x1FFF), v, size); hle_gs_write(pa, v, size, gspriv); return; }
    if (pa >= 0x11000000 && pa < 0x11010000) { HwStat *s = hwstat(pa & ~0xFFF); s->w++; st(vumem + (pa & 0xFFFF), v, size); return; }
    HwStat *s = hwstat(a); s->w++; s->lastv = v;
    if (n_unmapped++ < 100) L("unmapped write%d 0x%08x = 0x%llx in 0x%x\n", size * 8, a, (unsigned long long)v, rec_cur);
}
void hw_read128(uint32_t a, GPR *r) { r->u64[0] = hw_read(a, 8); r->u64[1] = hw_read(a + 8, 8); }
void hw_write128(uint32_t a, const GPR *r) { hw_write(a, r->u64[0], 8); hw_write(a + 8, r->u64[1], 8); }
uint8_t *gptr(uint32_t a) { if (a < 0x02000000) return ee_ram + a; uint8_t *p = ram_alias(a); return p ? p : ee_ram; }

/* ------------------------------------------------------------------ profiling + time limit */
static void on_prof(int sig) { (void)sig; int i = func_containing(rec_cur); if (i < 0) i = func_index(rec_cur); if (i >= 0) prof_hist[i]++; }
static double time_limit = -1;   /* -t seconds; default 60 headless, unlimited with a window */
static void on_alarm(int sig) { (void)sig; if (timing_wall) vblank_due = 1; if (time_limit > 0 && now() - t_start > time_limit) { void *bt[64]; int n = backtrace(bt, 64); fprintf(logf_, "host backtrace (guest call chain, innermost first):\n"); fflush(logf_); backtrace_symbols_fd(bt, n, fileno(logf_)); dump_stats("wall-clock time limit (probably a busy-wait loop; see profile)"); _exit(0); } }

/* ------------------------------------------------------------------ main */
extern void hle_init(void);
static uint64_t ntrace;
static const char *maybe_str(uint32_t a) {
    static char b[4][80]; static int k; char *o = b[k++ & 3];
    if (a < 0x100000 || a >= 0x02000000) return "";
    int i; for (i = 0; i < 70; i++) { char ch = ee_ram[a + i]; if (!ch) break; if (ch < 32 || ch > 126) return ""; o[i] = ch; }
    o[i] = 0; return i >= 2 ? o : "";
}
extern uint64_t hle_vblank_count(void);
static long press_at[16]; static int npress;
static int press_hook(Ctx *c) {   /* HG_PRESS: OR pad bit 14 (0x4000) into the game's pad word 0x47e37c right before the logo/notice step reads it */
    (void)c; uint64_t v = hle_vblank_count();
    for (int k = 0; k < npress; k++) if ((long)v >= press_at[k] && (long)v < press_at[k] + 4) {
        uint32_t w; memcpy(&w, ee_ram + 0x47e37c, 4); w |= 0x4000; memcpy(ee_ram + 0x47e37c, &w, 4);
        static long last = -1; if (last != press_at[k]) { last = press_at[k]; L("HG_PRESS: pad bit 14 injected at VBlank %llu (in 0x%x)\n", (unsigned long long)v, rec_cur); } }
    return 0;
}
static int trace_hook(Ctx *c) {
    if (ntrace++ < 3000) L("TRACE 0x%x(a0=%x '%s', a1=%x '%s', a2=%x, a3=%x) ra=%x\n", rec_cur, c->r[4].u32[0], maybe_str(c->r[4].u32[0]),
        c->r[5].u32[0], maybe_str(c->r[5].u32[0]), c->r[6].u32[0], c->r[7].u32[0], c->r[31].u32[0]);
    return 0;
}
int main(int argc, char **argv) {
    logf_ = stdout;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-t") && i + 1 < argc) time_limit = atof(argv[++i]);
        else if (!strcmp(argv[i], "-q")) bg_verbose = 0;
    }
    if (time_limit < 0) time_limit = host_window_wanted() ? 0 : 60;
    timing_wall = getenv("HG_TIMING") && !strcmp(getenv("HG_TIMING"), "wall");
    { double ips = getenv("HG_EE_MIPS") ? atof(getenv("HG_EE_MIPS")) * 1e6 : 300e6;   /* guest instructions per second (EE 294.9 MHz at about one instruction per cycle; 150 starves movie audio) */
      ee_vb_period = (uint64_t)(ips / 59.94); ee_next_vb = timing_wall ? (1ull << 62) : ee_vb_period; }
    const char *exe = getenv("HG_ELF") ? getenv("HG_ELF") : game_path("SLUS_210.75");
    ee_ram = mmap(0, 32 << 20, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    FILE *f = fopen(exe, "rb"); if (!f) { perror(exe); return 1; }
    fseek(f, 0x80, SEEK_SET);
    if (fread(ee_ram + 0x100000, 1, 0x37B200, f) != 0x37B200) { fprintf(stderr, "short read\n"); return 1; }
    fclose(f);
    t_start = now();
    struct sigaction sa = {0}; sa.sa_handler = on_prof; sa.sa_flags = SA_RESTART; sigaction(SIGPROF, &sa, 0);
    struct itimerval it = {{0, 1000}, {0, 1000}}; setitimer(ITIMER_PROF, &it, 0);
    sa.sa_handler = on_alarm; sigaction(SIGALRM, &sa, 0);
    struct itimerval ia = {{0, 16667}, {0, 16667}}; setitimer(ITIMER_REAL, &ia, 0);
    hle_init();
    extern void overrides_init(void); overrides_init();
    if (getenv("HG_PRESS")) { char *p = getenv("HG_PRESS"); while (*p && npress < 16) { press_at[npress++] = strtol(p, &p, 10); while (*p == ',') p++; }
        const char *fl = getenv("HG_PRESSFN") ? getenv("HG_PRESSFN") : "380410,37fef0,380050,37fe50,37fc60,37fac0,37f980,37f7d0";
        char *q = (char *)fl; while (*q) { uint32_t a = strtoul(q, &q, 16); if (func_index(a) >= 0) set_hook(a, press_hook); while (*q == ',') q++; } }
    if (getenv("HG_TRACE")) { char *p = getenv("HG_TRACE"); while (*p) { uint32_t a = strtoul(p, &p, 16); set_hook(a, trace_hook); while (*p == ',') p++; } }
    hle_run_main(0x100008);     /* never returns */
    return 0;
}
