/* bootgate EE kernel HLE: syscalls, cooperative priority threads, semaphores, interrupt handlers,
   hardware-register defaults. Everything is logged so the stop point says what the game waited on. */
#define _GNU_SOURCE
#include <stdio.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>
#include "compat.h"
#include "ee_runtime.h"
#include "hle.h"
extern uint32_t cop0_status;

void rec_syscall(Ctx *ctx, uint32_t pc);
static uint32_t R32(uint32_t a) { uint32_t v; memcpy(&v, gptr(a), 4); return v; }
static void W32(uint32_t a, uint32_t v) { memcpy(gptr(a), &v, 4); }
static void W64(uint32_t a, uint64_t v) { memcpy(gptr(a), &v, 8); }
static const char *gstr(uint32_t a) { static char b[256]; if (!a) return "(null)"; strncpy(b, (char *)gptr(a), 255); b[255] = 0; return b; }

/* ------------------------------------------------------------------ threads */
enum { THS_RUN = 1, THS_READY = 2, THS_WAIT = 4, THS_SUSPEND = 8, THS_DORMANT = 16 };
enum { WT_NONE = 0, WT_SLEEP = 1, WT_SEMA = 2 };
#define MAXT 64
typedef struct {
    int used, status, prio, init_prio, wait_type, wait_id, wakeup, suspended;
    uint32_t entry, stack, stack_size, gp, attr, option, arg;
    uint64_t seq;
    Ctx ctx; HostFiber fib;
} Thread;
static Thread th[MAXT];
static int cur = 1;
static uint64_t seqno = 1;
#define MAXS 256
typedef struct { int used, count, max, init, attr; uint32_t option; int nwait; } Sema;
static Sema sem[MAXS];
typedef struct { int used, cause, enabled; uint32_t handler, arg, gp; } Handler;
static Handler intc[64], dmac[64];
static uint32_t intc_mask, dmac_mask;
static uint64_t vblank_count, idle_ticks;
/* HG_FRAMEPROF=FILE: where emulated time goes (logging only): cycles skipped while no EE thread could run, and each
   wait of a thread on a semaphore or in SleepThread longer than a quarter VBlank ("WAIT vb thread kind id cycles").
   The frame hooks in overrides.c add the frame's start and end. */
uint64_t hle_idle_cycles;
FILE *hle_prof(void) { static FILE *f; static int init; if (!init) { init = 1; const char *e = getenv("HG_FRAMEPROF"); if (e && *e) f = fopen(e, "w"); } return f; }
int hle_cur_thread(void);
static void prof_wait(int kind, int id, uint64_t c0, uint32_t ra) {
    extern uint64_t ee_cycles, ee_vb_period; FILE *f = hle_prof();
    if (f && ee_cycles - c0 > ee_vb_period / 4) fprintf(f, "WAIT %llu %d %s %d %llu ra %x\n", (unsigned long long)vblank_count, cur, kind ? "sleep" : "sema", id, (unsigned long long)(ee_cycles - c0), ra);
}
static uint32_t vsync_flag_addr, vsync_data_addr;
static uint32_t heap_start, heap_end = 0x01F00000;
static uint32_t sif_reg[256];
static uint32_t syscall_vec[256];
static int in_intr;
static int dma_pending[16];

#define SYSTAB 0x00010000u
static const char *stname(int s) { switch (s) { case THS_RUN: return "RUN"; case THS_READY: return "READY"; case THS_WAIT: return "WAIT"; case THS_DORMANT: return "DORMANT"; default: return "SUSP"; } }
static void dump_threads(void) {
    for (int i = 1; i < MAXT; i++) if (th[i].used)
        L("  thread %d: entry 0x%x prio %d status %s wait %s id %d wakeup %d (sp 0x%x)\n", i, th[i].entry, th[i].prio, stname(th[i].status),
          th[i].wait_type == WT_SEMA ? "sema" : th[i].wait_type == WT_SLEEP ? "sleep" : "-", th[i].wait_id, th[i].wakeup, th[i].ctx.r[29].u32[0]);
    for (int i = 1; i < MAXS; i++) if (sem[i].used) L("  sema %d: count %d max %d init %d waiters %d\n", i, sem[i].count, sem[i].max, sem[i].init, sem[i].nwait);
    for (int i = 0; i < 64; i++) if (intc[i].used) L("  intc handler %d: cause %d fn 0x%x arg 0x%x %s\n", i, intc[i].cause, intc[i].handler, intc[i].arg, (intc_mask >> intc[i].cause) & 1 ? "enabled" : "masked");
    for (int i = 0; i < 64; i++) if (dmac[i].used) L("  dmac handler %d: ch %d fn 0x%x arg 0x%x %s\n", i, dmac[i].cause, dmac[i].handler, dmac[i].arg, (dmac_mask >> dmac[i].cause) & 1 ? "enabled" : "masked");
}

static void make_ready(int id) { th[id].status = THS_READY; th[id].wait_type = WT_NONE; th[id].seq = seqno++; }
static int pick(void) {
    int best = 0;
    for (int i = 1; i < MAXT; i++) {
        if (!th[i].used || th[i].suspended) continue;
        if (th[i].status != THS_READY && th[i].status != THS_RUN) continue;
        if (!best || th[i].prio < th[best].prio || (th[i].prio == th[best].prio && th[i].seq < th[best].seq)) best = i;
    }
    return best;
}
static void fire_vblank(void);
static void idle_advance(void);
static void iop_flush_idle(void);
static void thread_trampoline(void);
static uint64_t nswitch;
/* switch to the best runnable thread; when none is runnable, advance virtual time (VBlank) */
static void schedule(void) {
    if (in_intr) return;           /* no switching inside interrupt handlers; done when the handler returns */
    int n, spins = 0;
    while (!(n = pick())) {
        idle_advance(); idle_ticks++;
        if (++spins > 3000) { L("deadlock: all threads blocked after %d idle VBlanks\n", spins); dump_threads(); stop("deadlock: no runnable EE thread (thread %d blocked, wait %d/%d)", cur, th[cur].wait_type, th[cur].wait_id); }
    }
    if (n == cur) { th[cur].status = THS_RUN; return; }
    int prev = cur; cur = n;
    if (th[prev].status == THS_RUN) th[prev].status = THS_READY;
    th[n].status = THS_RUN; nswitch++;
    if (bg_verbose > 1) L("switch %d -> %d\n", prev, n);
    host_fiber_switch(&th[prev].fib, &th[n].fib);
}
static void thread_exit(int id) {
    th[id].status = THS_DORMANT; L("thread %d exited\n", id);
    schedule();
    stop("dormant thread %d resumed", id);
}
static void thread_trampoline(void) {
    int id = cur; Thread *t = &th[id];
    rec_call(&t->ctx, t->entry);
    thread_exit(id);
}
static void start_thread_ctx(int id, uint32_t arg) {
    Thread *t = &th[id];
    memset(&t->ctx, 0, sizeof t->ctx);
    t->ctx.r[4].u64[0] = arg; t->ctx.r[28].u64[0] = t->gp;
    t->ctx.r[29].u64[0] = (t->stack + t->stack_size - 0x40) & ~15u;
    t->ctx.r[31].u64[0] = 0xDEAD0000u; t->ctx.vf[0][3] = 1.0f;
    host_fiber_make(&t->fib, thread_trampoline, 64 << 20);
    t->wakeup = 0; t->suspended = 0; t->prio = t->init_prio;
    make_ready(id);
}

/* ------------------------------------------------------------------ interrupts */
static Ctx ictx;
static void call_handler(Handler *h, int cause) {
    memset(&ictx, 0, sizeof ictx);
    ictx.r[4].u64[0] = cause; ictx.r[5].u64[0] = h->arg; ictx.r[28].u64[0] = h->gp;
    ictx.r[29].u64[0] = 0x000FFF00; ictx.r[31].u64[0] = 0xDEAD0001u; ictx.vf[0][3] = 1.0f;
    uint32_t saved = rec_cur; in_intr++;
    rec_call(&ictx, h->handler);
    in_intr--; rec_cur = saved;
}
static void raise_intc(int cause) {
    if (!((intc_mask >> cause) & 1)) return;
    for (int i = 0; i < 64; i++) if (intc[i].used && intc[i].cause == cause) call_handler(&intc[i], cause);
}
static void raise_dmac(int ch) {
    if (!((dmac_mask >> ch) & 1)) return;
    for (int i = 0; i < 64; i++) if (dmac[i].used && dmac[i].cause == ch) call_handler(&dmac[i], ch);
}
extern uint64_t ee_cycles, ee_next_vb, ee_vb_period; extern int timing_wall;
/* ------------------------------------------------------------------ EE timers 0-3 (0x10000000 + 0x800 n)
   COUNT follows emulated time (guest instructions at HG_EE_MIPS); clock BUSCLK 147.456 MHz / 1, 16, 256 or HBLANK.
   Compare (CMPE) and overflow (OVFE) events set EQUF/OVFF and raise INTC causes 9-12. Gate modes are not modelled.
   ee_next_vb is the next emulated-time event of any kind (VBlank or timer); vb_deadline is the next VBlank. */
typedef struct { uint32_t mode, comp, base_count; uint64_t base_cyc, next; int pend; } Tmr;
static Tmr tmr[4] = { { .next = ~0ull }, { .next = ~0ull }, { .next = ~0ull }, { .next = ~0ull } };
static uint64_t vb_deadline; static int vb_init;
static double tmr_rate(int n) {   /* timer ticks per guest instruction */
    static const double div[3] = { 1, 16, 256 }; const int clks = tmr[n].mode & 3;
    const double hz = clks == 3 ? 15734.264 : 147456000.0 / div[clks];
    return hz / ((double)ee_vb_period * 59.94);
}
static uint32_t tmr_count(int n) {
    Tmr *t = &tmr[n];
    if (!(t->mode & 0x80) || timing_wall) return t->base_count;
    const uint64_t ticks = (uint64_t)((double)(ee_cycles - t->base_cyc) * tmr_rate(n));
    if ((t->mode & 0x40) && t->comp && t->base_count <= t->comp) return (uint32_t)((t->base_count + ticks) % ((uint64_t)t->comp + 1));
    return (uint32_t)((t->base_count + ticks) & 0xFFFF);
}
static void tmr_rebase(int n) { tmr[n].base_count = tmr_count(n); tmr[n].base_cyc = ee_cycles; }
static void tmr_plan(int n) {   /* next compare/overflow event in emulated time */
    Tmr *t = &tmr[n]; t->next = ~0ull;
    if (timing_wall || !(t->mode & 0x80) || !(t->mode & 0x300)) return;
    const uint32_t c = tmr_count(n); uint64_t dt = ~0ull;
    if (t->mode & 0x100) { uint64_t d = (t->comp - c) & 0xFFFF; if (!d) d = (t->mode & 0x40) ? (uint64_t)t->comp + 1 : 0x10000; dt = d; }
    if ((t->mode & 0x200) && !((t->mode & 0x40) && t->comp < 0xFFFF)) { uint64_t d = 0x10000 - c; if (d < dt) dt = d; }
    if (dt == ~0ull) return;
    t->next = ee_cycles + (uint64_t)ceil((double)dt / tmr_rate(n));
}
static void plan_events(void) {
    if (!vb_init) { vb_init = 1; vb_deadline = ee_next_vb; for (int n = 0; n < 4; n++) if (!(tmr[n].mode & 0x80)) tmr[n].next = ~0ull; }
    if (timing_wall) return;
    uint64_t nx = vb_deadline;
    for (int n = 0; n < 4; n++) if (tmr[n].next < nx) nx = tmr[n].next;
    ee_next_vb = nx;
    if (ee_cycles >= nx) vblank_due = 1;
}
static uint64_t n_timer_irq[4];
static void deliver_timers(void) {
    for (int n = 0; n < 4; n++) if (tmr[n].pend && !in_intr) { tmr[n].pend = 0; n_timer_irq[n]++; raise_intc(9 + n); }
}
static void run_timers(void) {   /* fire every timer event that is due */
    for (int n = 0; n < 4; n++) {
        Tmr *t = &tmr[n];
        if (t->next == ~0ull || ee_cycles < t->next) continue;
        const uint64_t now = ee_cycles; ee_cycles = t->next;           /* evaluate at the event time */
        const uint32_t c = tmr_count(n);
        if ((t->mode & 0x100) && c == ((t->mode & 0x40) ? 0 : t->comp)) t->mode |= 0x400;
        else if ((t->mode & 0x100) && c == t->comp) t->mode |= 0x400;
        else t->mode |= 0x800;
        tmr_rebase(n); ee_cycles = now;
        t->pend = 1;
        tmr_plan(n);
        if (t->next <= t->base_cyc) t->next = t->base_cyc + 1;
    }
}
uint64_t hle_timer_read(uint32_t pa, uint64_t v) {
    const int n = (pa >> 11) & 3;
    switch (pa & 0x7F0) {
    case 0x00: return tmr_count(n);
    case 0x10: return tmr[n].mode;
    case 0x20: return tmr[n].comp;
    }
    return v;
}
void hle_timer_write(uint32_t pa, uint64_t v) {
    const int n = (pa >> 11) & 3;
    tmr_rebase(n);
    switch (pa & 0x7F0) {
    case 0x00: tmr[n].base_count = (uint32_t)v & 0xFFFF; break;
    case 0x10: tmr[n].mode = ((uint32_t)v & 0x3FF) | (tmr[n].mode & 0xC00 & ~((uint32_t)v & 0xC00)); break;   /* EQUF/OVFF: write 1 to clear */
    case 0x20: tmr[n].comp = (uint32_t)v & 0xFFFF; break;
    }
    tmr_plan(n); plan_events();
}
static void fire_vblank(void) {
    if (in_intr) return;
    vblank_count++;
    if (!timing_wall) { if (!vb_init) { vb_init = 1; vb_deadline = ee_next_vb; } if (ee_cycles < vb_deadline) ee_cycles = vb_deadline; vb_deadline += ee_vb_period; }
    { static long maxvb = -1; if (maxvb < 0) maxvb = getenv("HG_MAXVB") ? atol(getenv("HG_MAXVB")) : 0;
      if (maxvb && (long)vblank_count >= maxvb) stop("reached HG_MAXVB=%ld", maxvb); }
    { static long crash = -1; if (crash < 0) crash = getenv("HG_CRASHTEST") ? atol(getenv("HG_CRASHTEST")) : 0;   /* tests the crash report */
      if (crash && (long)vblank_count == crash) { volatile int *p = 0; *p = 1; } }
    {   /* HG_SPEEDLOG=n: every n VBlanks, emulated speed (real time = 59.94 VB/s) and host seconds per subsystem.
           VU1 and IOP+SPU2 run on this thread; GS drawing runs on the GS thread when HG_MTGS is on (busy % of wall). */
        static long every = -1; static double t0, p0[8], idle0, lt, lidle, worst; static uint64_t v0;
        if (every < 0) { every = getenv("HG_SPEEDLOG") ? atol(getenv("HG_SPEEDLOG")) : 0; t0 = lt = host_now(); v0 = vblank_count; memcpy(p0, tprof, sizeof p0); idle0 = lidle = host_idle; }
        if (every > 0) {
            const double now = host_now(), work = (now - lt) - (host_idle - lidle);   /* this VBlank's host time, pacing sleep left out */
            if (work > worst) worst = work;
            lt = now; lidle = host_idle;
        }
        if (every > 0 && vblank_count - v0 >= (uint64_t)every) {
            extern double gs_producer_wait_s(void); static double pw0; const double pw = gs_producer_wait_s();
            L("SPEED vb %llu: waiting for the GS thread %.2f ms per VB\n", (unsigned long long)vblank_count, (pw - pw0) / (double)(vblank_count - v0) * 1e3); pw0 = pw;
            double t = host_now(), dt = t - t0, n = (double)(vblank_count - v0), busy = dt - (host_idle - idle0);
            L("SPEED vb %llu: %.1f VB/s = %.2fx real time; per VB %.2f ms (busy %.2f, worst %.1f): VU1 %.2f, IOP+SPU2 %.2f, DMA/VIF (less VU1, GS) %.2f, other EE-thread %.2f; GS thread busy %.0f%%\n",
              (unsigned long long)vblank_count, n / dt, n / dt / 59.94, dt / n * 1e3, busy / n * 1e3, worst * 1e3, (tprof[0] - p0[0]) / n * 1e3, (tprof[2] - p0[2]) / n * 1e3,
              (tprof[4] - p0[4]) / n * 1e3, (busy - (tprof[0] - p0[0]) - (tprof[2] - p0[2]) - (tprof[4] - p0[4])) / n * 1e3, (tprof[1] - p0[1]) / dt * 100);
            t0 = t; v0 = vblank_count; memcpy(p0, tprof, sizeof p0); idle0 = host_idle; worst = 0;
        }
    }
    for (int c = 0; c < 16; c++) if (dma_pending[c] && c != 5) { dma_pending[c] = 0; raise_dmac(c); }
    iop_flush_idle();
    host_vblank(vblank_count);
    iop_vblank();
    pad_vblank(vblank_count);
    gs_vblank(vblank_count);
    raise_intc(2);   /* VBLANK start */
    if (vsync_flag_addr) { W32(vsync_flag_addr, 1); if (vsync_data_addr) W64(vsync_data_addr, (vblank_count & 1) << 13); }
    raise_intc(3);   /* VBLANK end */
    run_timers(); deliver_timers(); plan_events();
    {   /* HG_RAMDUMP=dir:vb,vb,... writes EE RAM at those VBlanks (debugging) */
        static const char *rd; static int init; if (!init) { init = 1; rd = getenv("HG_RAMDUMP"); }
        if (rd) { const char *c = strrchr(rd, ':');   /* the last colon: Windows paths have a drive letter */ if (c) { for (const char *p = c + 1; *p; ) { long v = strtol(p, (char **)&p, 10);
            if ((uint64_t)v == vblank_count) { char path[512]; snprintf(path, sizeof path, "%.*s/ram_%06ld.bin", (int)(c - rd), rd, v); FILE *f = fopen(path, "wb"); if (f) { fwrite(gptr(0), 1, 32 << 20, f); fclose(f); } }
            while (*p == ',') p++; if (!v && *p && (*p < '0' || *p > '9')) break; } } }
    }
}
/* the next emulated-time event is due: timers and/or VBlank */
static void service_events(void) {
    if (timing_wall) { fire_vblank(); return; }
    if (!vb_init) { vb_init = 1; vb_deadline = ee_next_vb; }
    run_timers(); deliver_timers();
    if (ee_cycles >= vb_deadline) fire_vblank(); else plan_events();
}
/* idle: advance emulated time to the next event and service it */
static void idle_advance(void) {
    if (timing_wall) { fire_vblank(); return; }
    if (!vb_init) { vb_init = 1; vb_deadline = ee_next_vb; }
    plan_events();
    {   /* HG_IOPSYNC (iop_kernel.c): the IOP's events before the EE's next one, one at a time, until an EE thread can run */
        extern int iop_sync_on(void); extern uint64_t iop_next_event_ee(uint64_t); extern void iop_sync_to_ee(uint64_t);
        if (iop_sync_on()) for (int k = 0; k < 256; k++) {
            const uint64_t e = iop_next_event_ee(ee_next_vb);
            if (!e) break;
            if (e > ee_cycles) { hle_idle_cycles += e - ee_cycles; ee_cycles = e; }
            iop_sync_to_ee(ee_cycles);
            iop_flush_idle();
            if (pick()) return;
        }
    }
    if (ee_cycles < ee_next_vb) { hle_idle_cycles += ee_next_vb - ee_cycles; ee_cycles = ee_next_vb; }
    service_events();
}


/* ------------------------------------------------------------------ disc: only DATA.CVM is present, mapped at a virtual LSN */
#define CVM_LSN 0x1000u
static FILE *cvm_f; static long long cvm_sz = -1;
static long long cvm_size(void) {
    if (cvm_sz < 0) { const char *p = getenv("HG_CVM") ? getenv("HG_CVM") : game_path("DATA.CVM"); cvm_f = fopen(p, "rb");
        if (cvm_f) { fseeko(cvm_f, 0, SEEK_END); cvm_sz = ftello(cvm_f); } else cvm_sz = 0; }
    return cvm_sz;
}
static uint64_t cd_sectors_read, n_cdreads;
static int cd_read(uint32_t lsn, uint32_t n, uint32_t ee) {   /* 2048-byte sectors */
    if (lsn < CVM_LSN || !cvm_size()) return -1;
    if (n > 4096) n = 4096;
    fseeko(cvm_f, (long long)(lsn - CVM_LSN) * 2048, SEEK_SET);
    size_t got = fread(gptr(ee), 1, (size_t)n * 2048, cvm_f); cd_sectors_read += n;
    return got == (size_t)n * 2048 ? 0 : -1;
}
/* ------------------------------------------------------------------ IOP HLE (SIF command layer only) */
#define IOP_CMDBUF 0x0001F000u
uint8_t iop_ram[2 << 20];
static uint32_t sifdma_id, ee_sbuf;
typedef struct { int len; uint32_t w[32]; } IopPkt;
static IopPkt iopq[256]; static int iopq_n;
typedef struct { uint32_t sid, server, buf, cbuf; int real; uint64_t calls; } Bind;
static Bind binds[64]; static int nbinds;
static uint64_t n_rpc_calls;
static void iop_reply(uint32_t cid, const uint32_t *pl, int n) {
    if (iopq_n >= 256) stop("IOP reply queue overflow");
    IopPkt *p = &iopq[iopq_n++]; p->len = 4 + n;
    p->w[0] = (uint32_t)(16 + 4 * n); p->w[1] = 0; p->w[2] = cid; p->w[3] = 0;
    for (int i = 0; i < n; i++) p->w[4 + i] = pl[i];
}
static const char *sidname(uint32_t sid) {
    switch (sid) { case 0x80000001: return "FILEIO"; case 0x80000003: return "IOP_HEAP"; case 0x80000006: return "LOADFILE";
        case 0x80000592: return "CDVD_INIT"; case 0x80000593: return "CDVD_S_CMD"; case 0x80000595: return "CDVD_N_CMD"; case 0x80000596: return "CDVD_SEARCH";
        case 0x80000597: return "CDVD_SEARCHFILE"; case 0x8000059A: return "CDVD_POFF"; case 0x80000400: return "MCSERV"; case 0x80000100: return "PADMAN";
        case 0x80000101: return "PADMAN_EXT"; case 0x80000901: return "MTAP";
        case 0x80001300: return "DBCMAN"; case 0x8000131c: case 0x8000131e: case 0x8000131f: return "DBCMAN_SUB"; case 0x90000200: return "CRI_ADX"; case 0x77777777: case 0x77777778: return "SNDDRV"; } return "?";
}
/* per-server RPC behaviour: fill the EE receive buffer the way the real IOP module would (minimal) */
static void iop_rpc(Bind *b, uint32_t fn, uint32_t sbuf, uint32_t ssize, uint32_t recv, uint32_t rsize) {
    b->calls++; n_rpc_calls++;
    if (b->calls <= 8 || bg_verbose > 1)
        L("RPC call sid 0x%08x (%s) fn 0x%x send %u -> recv 0x%x size %u  first words %08x %08x %08x %08x\n", b->sid, sidname(b->sid), fn, ssize, recv, rsize,
          *(uint32_t *)(iop_ram + (sbuf & 0x1FFFFF)), *(uint32_t *)(iop_ram + ((sbuf + 4) & 0x1FFFFF)), *(uint32_t *)(iop_ram + ((sbuf + 8) & 0x1FFFFF)), *(uint32_t *)(iop_ram + ((sbuf + 12) & 0x1FFFFF)));
    uint8_t *S = iop_ram + (sbuf & 0x1FFFFF);
    if (recv && rsize && rsize < 0x100000) {
        /* default: result word 0 = 1 (success / module id); everything else zero */
        memset(gptr(recv), 0, rsize);
        if (rsize >= 4) W32(recv, 1);
    }
    if (GETENV_ONCE("HG_DUMPSID") && strtoul(GETENV_ONCE("HG_DUMPSID"), 0, 16) == b->sid) {
        L("  send dump:"); for (uint32_t k = 0; k < ssize && k < 320; k += 4) L("%s%08x", k % 32 ? " " : "\n   ", *(uint32_t *)(S + k)); L("\n");
    }
    if (pad_rpc(b->sid, fn, S, ssize, recv, rsize) || mc_rpc(b->sid, fn, S, ssize, recv, rsize)) return;
    switch (b->sid) {
    case 0x80000597: { /* sceCdSearchFile: cdlFILE result -> EE dest at +0x124; recv[0] = 1 found / 0 not */
        const char *name = (const char *)S + 0x24; uint32_t dest = *(uint32_t *)(S + 0x124);
        uint32_t lsn = 0, size = 0; int found = 0;
        if (strstr(name, "DATA.CVM")) { lsn = CVM_LSN; size = (uint32_t)cvm_size(); found = size != 0; }
        L("IOP: sceCdSearchFile(\"%s\") -> %s lsn 0x%x size %u (dest 0x%x)\n", name, found ? "found" : "NOT FOUND", lsn, size, dest);
        if (found && dest) { W32(dest, lsn); W32(dest + 4, size); strncpy((char *)gptr(dest + 8), "DATA.CVM;1", 16); }
        if (recv) W32(recv, found);
        break; }
    case 0x80000595: { /* CDVD N_CMD */
        if (fn == 0xe && recv) W32(recv, 2);                          /* DiskReady: SCECdComplete */
        else if (fn == 1 || fn == 3) {                                /* sceCdRead / DVD read: lsn, sectors, buf, mode, eedata */
            uint32_t lsn = *(uint32_t *)S, n = *(uint32_t *)(S + 4), buf = *(uint32_t *)(S + 8), eed = *(uint32_t *)(S + 16);
            int r = cd_read(lsn, n, buf);
            if (n_cdreads++ < 20000) L("IOP: sceCdRead(lsn 0x%x, %u sectors -> 0x%x) %s (CVM offset 0x%llx)\n", lsn, n, buf, r ? "FAILED" : "ok", (long long)(lsn - CVM_LSN) * 2048);
            if (eed) for (int k = 0; k < 4; k++) W32(eed + 4 * k, 0);   /* no unaligned head/tail fixups needed */
            if (recv) W32(recv, r ? 0 : 1);
        } else if (recv) W32(recv, 1);
        break; }
    case 0x80000593: { /* CDVD S_CMD */
        uint32_t v = 1;
        if (fn == 0x01 && recv) {          /* ReadClock: result, then sceCdCLOCK {stat, sec, min, hour, pad, day, month, year} in BCD, JST like the console's RTC */
            time_t t = rr_clock() + 9 * 3600; struct tm tm; host_gmtime(t, &tm);
            #define BCD(x) (uint8_t)((((x) / 10) << 4) | ((x) % 10))
            uint8_t c[8] = { 0, BCD(tm.tm_sec), BCD(tm.tm_min), BCD(tm.tm_hour), 0, BCD(tm.tm_mday), BCD(tm.tm_mon + 1), BCD(tm.tm_year % 100) };
            #undef BCD
            memcpy(gptr(recv + 4), c, 8);
        }
        if (fn == 0x03) v = 0x14;          /* GetDiskType: PS2 DVD */
        else if (fn == 0x04) v = 0;        /* GetError: none */
        if (recv) W32(recv, v);
        break; }
    case 0x80000003:   /* IOP heap: fn 1 alloc(size) -> addr, fn 2 free(addr) */
        if (fn == 1 && recv) { uint32_t a = iop_heap_alloc(*(uint32_t *)S); L("IOP: AllocIopHeap(0x%x) -> 0x%x\n", *(uint32_t *)S, a); W32(recv, a); }
        else if (fn == 2 && recv) W32(recv, iop_heap_free(*(uint32_t *)S));
        break;
    case 0x80000006:   /* LOADFILE */
        if (fn == 0xff && recv) memcpy(gptr(recv), "3000", 4);                     /* version */
        else if (fn == 0 || fn == 1) {
            uint32_t alen = *(uint32_t *)S;   /* struct _lf_module_load_arg: { arg_len/result, modres, path[252], args[252] } */ int id = iop_load_module((char *)S + 8, (char *)S + 0x104, alen < 252 ? (int)alen : 0);
            L("IOP: LoadModule \"%s\" (%u arg bytes)%s\n", (char *)S + 8, alen, id > 0 ? " (emulated)" : id < 0 ? " (FAILED in emulator)" : "");
            if (recv) { W32(recv, id > 0 ? (uint32_t)id : 0x100 + (uint32_t)b->calls); if (rsize >= 8) W32(recv + 4, 0); } }
        break;
    }
}
void hle_iop_rpc_end(const uint32_t endpkt[8]) { iop_reply(0x80000008, endpkt, 8); }
void hle_iop_send_cmd(uint32_t cid, const uint32_t *payload, int n) { iop_reply(cid, payload, n); }
int hle_cd_read_iop(uint32_t lsn, uint32_t n, uint32_t iop_addr) {
    if (lsn < CVM_LSN || !cvm_size() || (iop_addr & 0x1FFFFF) + (uint64_t)n * 2048 > sizeof iop_ram) return -1;
    fseeko(cvm_f, (long long)(lsn - CVM_LSN) * 2048, SEEK_SET); cd_sectors_read += n;
    return fread(iop_ram + (iop_addr & 0x1FFFFF), 1, (size_t)n * 2048, cvm_f) == (size_t)n * 2048 ? 0 : -1;
}
static void iop_sifdma(uint32_t src, uint32_t dst, uint32_t size, uint32_t attr) {
    if (size > 0x100000) size = 0x100000;
    if (dst != IOP_CMDBUF) {           /* plain data (RPC send buffer etc): keep a copy in IOP RAM */
        memcpy(iop_ram + (dst & 0x1FFFFF), gptr(src), (dst & 0x1FFFFF) + size <= sizeof iop_ram ? size : 0);
        static int nb; if (GETENV_ONCE("HG_BLKLOG") && size == 0x880 && R32(src) && vblank_count > (uint64_t)atoi(GETENV_ONCE("HG_BLKLOG")) && nb++ < 800) {
            L("EE->IOP blk dst 0x%x vb %llu count %u:", dst, (unsigned long long)vblank_count, R32(src)); for (int k = 4; k < 16; k++) L(" %08x", R32(src + 4 * k)); L("\n"); }
        static int nl; if (bg_verbose > 1 || (GETENV_ONCE("HG_SIFLOG") && nl++ < 400000 && (size > 0x880 || nl < 400))) L("EE SIF DMA -> IOP: src 0x%x dst 0x%x size 0x%x first %08x %08x\n", src, dst, size, R32(src), R32(src + 4));
        return;
    }
    uint32_t w[16]; for (int i = 0; i < 16; i++) w[i] = R32(src + 4 * i);
    uint32_t cid = w[2], opt = w[3];
    switch (cid) {
    case 0x80000002:                   /* INIT_CMD; opt 1 = init RPC -> IOP answers SET_SREG(0, 1) */
        L("IOP: INIT_CMD opt %d (EE buf 0x%x)\n", opt, w[4]);
        if (opt == 1) { uint32_t pl[2] = {0, 1}; iop_reply(0x80000001, pl, 2); }
        break;
    case 0x80000009: {                 /* RPC BIND: rec_id pkt_addr rpc_id client sid */
        uint32_t sid = w[8]; Bind *b = 0;
        for (int i = 0; i < nbinds; i++) if (binds[i].sid == sid) b = &binds[i];
        if (!b) { b = &binds[nbinds++]; b->sid = sid; b->server = 0x001C0000u + 0x1000u * nbinds; b->buf = b->server + 0x100; b->cbuf = b->server + 0x800; }
        uint32_t sd, sb, scb;
        if (!b->real && iop_rpc_find(sid, &sd, &sb, &scb)) { b->real = 1; b->server = sd; b->buf = sb; b->cbuf = scb; }
        if (!b->real && iop_serves_sid(sid)) {          /* module loaded in the IOP emulator but its server is not up yet: EE retries */
            L("IOP: RPC bind sid 0x%08x: server not registered yet\n", sid);
            uint32_t pl0[8] = {w[4], w[5], w[6], w[7], 0x80000009, 0, 0, 0}; iop_reply(0x80000008, pl0, 8); break; }
        L("IOP: RPC bind sid 0x%08x (%s) client 0x%x -> server 0x%x%s\n", sid, sidname(sid), w[7], b->server, b->real ? " (emulated IOP module)" : "");
        uint32_t pl[8] = {w[4], w[5], w[6], w[7], 0x80000009, b->server, b->buf, b->cbuf};
        iop_reply(0x80000008, pl, 8); break; }
    case 0x8000000A: {                 /* RPC CALL: rec_id pkt_addr rpc_id client fn ssize recv rsize rmode server */
        Bind *b = 0; for (int i = 0; i < nbinds; i++) if (binds[i].server == w[13]) b = &binds[i];
        if (!b) { L("IOP: RPC call to unknown server 0x%x\n", w[13]); static Bind dummy; b = &dummy; }
        uint32_t pl[8] = {w[4], w[5], w[6], w[7], 0x8000000A, w[13], 0, 0};
        if (b->real) { b->calls++; n_rpc_calls++; if (iop_rpc_call(b->server, w[8], w[9], w[10], w[11], pl)) break; }
        iop_rpc(b, w[8], b->buf, w[9], w[10], w[11]);
        iop_reply(0x80000008, pl, 8); break; }
    case 0x8000000C: {                 /* GETOTHERDATA: copy IOP memory to EE */
        uint32_t s = w[8], d = w[9], n = w[10];
        if (n < 0x100000) memcpy(gptr(d), iop_ram + (s & 0x1FFFFF), n);
        uint32_t pl[8] = {w[4], w[5], w[6], w[7], 0x8000000C, 0, 0, 0};
        iop_reply(0x80000008, pl, 8); break; }
    case 0x80000008: iop_ee_rpc_end(w); break;   /* RPC END from an EE server to an emulated IOP client */
    case 0x80000000:                   /* CHANGE_SADDR: EE tells the IOP where its command buffer is */
        ee_sbuf = w[4]; L("IOP: CHANGE_SADDR EE buffer 0x%x (word there 0x%x)\n", w[4], R32(w[4])); break;
    default:
        L("IOP: unhandled SIF cmd 0x%08x opt 0x%x size %u\n", cid, opt, size);
    }
}
/* deliver queued IOP->EE packets through the EE's own SIF0 DMAC handler (channel 5) */
static void iop_flush(void) {
    if (in_intr || !(cop0_status & 0x10000u) || !iopq_n) return;
    uint32_t pkt = ee_sbuf ? ee_sbuf : sif_reg[0x81];   /* EE packet buffer (CHANGE_SADDR / SIF_SYSREG_MAINADDR) */
    if (!pkt) return;
    int woke = 0;
    for (int k = 0; k < iopq_n; k++) {
        uint32_t pktbuf = pkt;
        for (int i = 0; i < iopq[k].len; i++) W32(pktbuf + 4 * i, iopq[k].w[i]);
        raise_dmac(5); woke = 1;
    }
    iopq_n = 0;
    if (woke) schedule();
}
extern volatile int vblank_due;
static uint64_t async_vblanks;
/* called at safe points (indirect-call dispatch, ei, syscalls) when the 60 Hz host timer has ticked */
void hle_async_tick(void) {
    if (host_stop_req) host_stop_now();   /* Windows time limit, requested by the timer thread */
    if (in_intr || !(cop0_status & 0x10000u)) return;
    vblank_due = 0; async_vblanks++;
    service_events();
    schedule();     /* preempt if a handler readied a higher-priority thread */
}
void ee_ei_hook(void) { iop_flush(); if (vblank_due) hle_async_tick(); }
static void iop_flush_idle(void) { uint32_t s = cop0_status; cop0_status |= 0x10000u; int n = iopq_n; if (n) { uint32_t pkt = ee_sbuf ? ee_sbuf : sif_reg[0x81]; if (pkt) { for (int k = 0; k < iopq_n; k++) { uint32_t pktbuf = pkt; for (int i = 0; i < iopq[k].len; i++) W32(pktbuf + 4 * i, iopq[k].w[i]); raise_dmac(5); } iopq_n = 0; } } cop0_status = s; }

/* ------------------------------------------------------------------ syscalls */
static uint64_t sys_cnt[512];
static int sema_alloc(void) { for (int i = 1; i < MAXS; i++) if (!sem[i].used) return i; stop("out of semaphores"); }
static int thread_alloc(void) { for (int i = 2; i < MAXT; i++) if (!th[i].used) return i; stop("out of threads"); }
static int tid(int id) { return id == 0 ? cur : id; }
static int nlog_sys;

void rec_syscall(Ctx *ctx, uint32_t pc) {
    int num = (int)ctx->r[3].s32[0];
    uint32_t a0 = ctx->r[4].u32[0], a1 = ctx->r[5].u32[0], a2 = ctx->r[6].u32[0], a3 = ctx->r[7].u32[0], t0 = ctx->r[8].u32[0];
    sys_cnt[num >= 0 ? (num & 255) : 256 + ((-num) & 255)]++;
    if (iopq_n && num > 0) iop_flush();
    if (vblank_due && num > 0) hle_async_tick();
    int64_t ret = 0; int quiet = 0;
    if (num >= 0 && num < 256 && syscall_vec[num] && func_index(syscall_vec[num]) >= 0) {
        /* the program installed its own (user-space, translated) handler with SetSyscall: run it like the kernel would */
        rec_call(ctx, syscall_vec[num]); return;
    }
    switch (num) {
    case 0x3c: { /* SetupThread(gp, stack, stack_size, args, root) */
        uint32_t top = a1 == 0xFFFFFFFFu ? 0x02000000u : a1 + a2;
        th[1].stack = top - a2; th[1].stack_size = a2; th[1].gp = a0;
        if (a3) { /* struct { int argc; char *argv[16]; char payload[256]; } */
            const char *arg0 = "cdrom0:\\SLUS_210.75;1";
            W32(a3, 1); W32(a3 + 4, a3 + 0x44); strcpy((char *)gptr(a3 + 0x44), arg0);
        }
        ret = top - 0x2a0; break; }
    case 0x3d: heap_start = a0; heap_end = a1 == 0xFFFFFFFFu ? th[1].stack : a0 + a1; ret = heap_end; break; /* SetupHeap */
    case 0x3e: ret = heap_end; break;                                   /* EndOfHeap */
    case 0x64: quiet = 1; ret = 0; break;                               /* FlushCache */
    case 0x7f: ret = 32 << 20; break;                                   /* GetMemorySize */
    case 0x82: ret = 0; break;                                          /* _InitTLB */
    case 0x5a: memcpy(gptr(a0 & 0x1FFFFFFF), gptr(a1), a2); ret = 0; break; /* copy into kernel RAM */
    case 0x5b: ret = R32(SYSTAB + 4 * (a0 & 511)); break;              /* GetEntryAddress(n): read the fake table */
    case 0x74: if (a0 < 512) W32(SYSTAB + 4 * a0, a1); if (a0 < 256) syscall_vec[a0] = a1; L("SetSyscall(0x%x, 0x%x)\n", a0, a1); ret = 0; break;
    case 0x02: L("SetGsCrt(interlace=%d, mode=0x%x, field=%d)\n", a0, a1, a2); break;
    case 0x71: L("GsPutIMR(0x%x)\n", a0); quiet = 1; break;
    case 0x73: vsync_flag_addr = a0; vsync_data_addr = a1; break;       /* SetVSyncFlag */
    case 0x4b: { uint32_t v = 1u << 16; W32(a0, v); break; }            /* GetOsdConfigParam: English, 4:3, RGB */
    case 0x4a: break;                                                   /* SetOsdConfigParam */
    case 0x7c: ret = 0; break;                                          /* Deci2Call */
    /* --- interrupts */
    case 0x10: case -0x10: { int i; for (i = 0; i < 64 && intc[i].used; i++); intc[i] = (Handler){1, (int)a0, 1, a1, a3, ctx->r[28].u32[0]}; ret = i + 1;
        L("AddIntcHandler(cause %d, fn 0x%x, next %d, arg 0x%x) -> %d\n", a0, a1, a2, a3, i + 1); break; }
    case 0x11: case -0x11: if (a1 >= 1 && a1 <= 64) intc[a1 - 1].used = 0; break;
    case 0x12: case -0x12: { int i; for (i = 0; i < 64 && dmac[i].used; i++); dmac[i] = (Handler){1, (int)a0, 1, a1, a3, ctx->r[28].u32[0]}; ret = i + 1;
        L("AddDmacHandler(ch %d, fn 0x%x, next %d, arg 0x%x) -> %d\n", a0, a1, a2, a3, i + 1); break; }
    case 0x13: case -0x13: if (a1 >= 1 && a1 <= 64) dmac[a1 - 1].used = 0; break;
    case 0x14: case -0x1a: ret = !((intc_mask >> a0) & 1); intc_mask |= 1u << a0; break;
    case 0x15: case -0x1b: ret = (intc_mask >> a0) & 1; intc_mask &= ~(1u << a0); break;
    case 0x16: case -0x1c: ret = !((dmac_mask >> a0) & 1); dmac_mask |= 1u << a0; break;
    case 0x17: case -0x1d: ret = (dmac_mask >> a0) & 1; dmac_mask &= ~(1u << a0); break;
    /* --- threads */
    case 0x20: { int id = thread_alloc(); Thread *t = &th[id]; memset(t, 0, sizeof *t) ; t->used = 1;
        t->entry = R32(a0 + 4); t->stack = R32(a0 + 8); t->stack_size = R32(a0 + 12); t->gp = R32(a0 + 16);
        t->init_prio = t->prio = R32(a0 + 20); t->attr = R32(a0 + 28); t->option = R32(a0 + 32); t->status = THS_DORMANT;
        L("CreateThread(entry 0x%x, stack 0x%x+0x%x, gp 0x%x, prio %d) -> %d  [caller 0x%x]\n", t->entry, t->stack, t->stack_size, t->gp, t->prio, id, ctx->r[31].u32[0]);
        ret = id; break; }
    case 0x21: if (th[a0].used && th[a0].status == THS_DORMANT) th[a0].used = 0; break;
    case 0x22: { int id = a0; if (!th[id].used) { ret = -1; break; }
        L("StartThread(%d, arg 0x%x) entry 0x%x prio %d\n", id, a1, th[id].entry, th[id].prio);
        start_thread_ctx(id, a1); ctx->r[2].s64[0] = 0; schedule(); return; }
    case 0x23: case 0x24: thread_exit(cur); break;
    case 0x25: case -0x25: th[a0].status = THS_DORMANT; break;
    case 0x29: case -0x2a: { int id = tid(a0); ret = th[id].prio; th[id].prio = a1;
        if (th[id].status == THS_READY || th[id].status == THS_RUN) th[id].seq = seqno++;
        if (bg_verbose) L("ChangeThreadPriority(%d, %d) [old %d]\n", id, a1, (int)ret); ctx->r[2].s64[0] = ret; if (num > 0) schedule(); return; }
    case 0x2b: case -0x2c: { /* RotateThreadReadyQueue(prio) */
        if (th[cur].prio == (int)a0) th[cur].seq = seqno++;
        else { int b = 0; for (int i = 1; i < MAXT; i++) if (th[i].used && th[i].status == THS_READY && th[i].prio == (int)a0 && (!b || th[i].seq < th[b].seq)) b = i; if (b) th[b].seq = seqno++; }
        quiet = 1; ctx->r[2].s64[0] = 0; if (num > 0) schedule(); return; }
    case 0x2f: case -0x2f: ret = cur; quiet = 1; break;                      /* GetThreadId / iGetThreadId */
    case 0x30: case -0x31: { int id = tid(a0); Thread *t = &th[id]; if (!t->used) { ret = -1; break; }
        /* kernel status codes: RUN 1, READY 2, WAIT 4, SUSPEND 8, WAITSUSPEND 0xC, DORMANT 0x10; suspension is reported in the status */
        uint32_t st = t->status;
        if (t->suspended && st != THS_DORMANT) st = (st == THS_WAIT) ? 0xC : 8;
        if (a1) { W32(a1, st); W32(a1 + 4, t->entry); W32(a1 + 8, t->stack); W32(a1 + 12, t->stack_size); W32(a1 + 16, t->gp);
                  W32(a1 + 20, t->init_prio); W32(a1 + 24, t->prio); W32(a1 + 28, t->attr); W32(a1 + 32, t->option);
                  W32(a1 + 36, t->wait_type); W32(a1 + 40, t->wait_id); W32(a1 + 44, t->wakeup); }
        ret = st; quiet = 1; break; }
    case 0x32: /* SleepThread */
        if (th[cur].wakeup > 0) { th[cur].wakeup--; ret = cur; break; }
        th[cur].status = THS_WAIT; th[cur].wait_type = WT_SLEEP; { const uint64_t c0 = ee_cycles; const uint32_t ra = ctx->r[31].u32[0]; schedule(); prof_wait(1, 0, c0, ra); } ctx->r[2].s64[0] = cur; return;
    case 0x33: case -0x34: { int id = a0; if (!th[id].used) { ret = -1; break; }
        if (th[id].status == THS_WAIT && th[id].wait_type == WT_SLEEP) make_ready(id); else th[id].wakeup++;
        ctx->r[2].s64[0] = id; if (num > 0) schedule(); return; }
    case 0x37: case -0x38: th[tid(a0)].suspended = 1; if (num > 0) schedule(); break;
    case 0x39: case -0x3a: th[a0].suspended = 0; ctx->r[2].s64[0] = a0; if (num > 0) schedule(); return;
    /* --- semaphores */
    case 0x40: { int id = sema_alloc(); sem[id] = (Sema){1, (int)R32(a0), (int)R32(a0 + 4), (int)R32(a0 + 8), (int)R32(a0 + 16), R32(a0 + 20), 0};
        sem[id].count = sem[id].init;
        if (bg_verbose) L("CreateSema(init %d, max %d) -> %d [caller 0x%x]\n", sem[id].init, sem[id].max, id, ctx->r[31].u32[0]);
        ret = id; quiet = 1; break; }
    case 0x41: if (a0 < MAXS) sem[a0].used = 0; ret = a0; quiet = 1; break;
    case 0x42: case -0x43: { int id = a0; if (id <= 0 || id >= MAXS || !sem[id].used) { ret = -1; break; }
        int w = 0; for (int i = 1; i < MAXT; i++) if (th[i].used && th[i].status == THS_WAIT && th[i].wait_type == WT_SEMA && th[i].wait_id == id && (!w || th[i].seq < th[w].seq)) w = i;
        if (w) { sem[id].nwait--; make_ready(w); } else sem[id].count++;
        ctx->r[2].s64[0] = id; if (num > 0 && w) schedule(); return; }
    case 0x44: { int id = a0; if (id <= 0 || id >= MAXS || !sem[id].used) { ret = -1; break; }  /* WaitSema */
        if (sem[id].count > 0) { sem[id].count--; ret = id; quiet = 1; break; }
        th[cur].status = THS_WAIT; th[cur].wait_type = WT_SEMA; th[cur].wait_id = id; th[cur].seq = seqno++; sem[id].nwait++;
        { const uint64_t c0 = ee_cycles; const uint32_t ra = ctx->r[31].u32[0];
          if (hle_prof() && getenv("HG_RPCWAIT")) { extern int iop_rpc_open(void); fprintf(hle_prof(), "BLOCK %llu %d sema %d replies queued %d, IOP open %d, int %s\n", (unsigned long long)vblank_count, cur, id, iopq_n, iop_rpc_open(), (cop0_status & 0x10000u) ? "on" : "off"); }
          schedule(); prof_wait(0, id, c0, ra); }
        ctx->r[2].s64[0] = id; return; }
    case 0x45: case -0x46: { int id = a0; if (id <= 0 || id >= MAXS || !sem[id].used) { ret = -1; break; }
        if (sem[id].count > 0) { sem[id].count--; ret = id; } else ret = -1; quiet = 1; break; }
    case 0x47: case -0x48: { int id = a0; if (!sem[id].used) { ret = -1; break; }
        W32(a1, sem[id].count); W32(a1 + 4, sem[id].max); W32(a1 + 8, sem[id].init); W32(a1 + 12, sem[id].nwait); W32(a1 + 16, sem[id].attr); W32(a1 + 20, sem[id].option);
        ret = id; quiet = 1; break; }
    /* --- SIF */
    case 0x79: if (a0 < 256) sif_reg[a0 & 255] = a1; else sif_reg[0x80 + (a0 & 0x7f)] = a1; L("sceSifSetReg(0x%x, 0x%x)\n", a0, a1); break;
    case 0x7a: { uint32_t r = a0 < 256 ? sif_reg[a0] : sif_reg[0x80 + (a0 & 0x7f)];
        if (a0 == 4) r |= 0x70000;   /* SMFLG: IOP says SIFINIT|CMDINIT|BOOTEND */
        if (a0 == 0x80000000u) r = IOP_CMDBUF;   /* IOP-side SIF command buffer */
        ret = r; L("sceSifGetReg(0x%x) -> 0x%x\n", a0, r); break; }
    case 0x78: case -0x78: L("sceSifSetDChain()\n"); ret = 0; break;
    case 0x77: case -0x77: { /* sceSifSetDma(desc*, count): EE->IOP transfers, handed to the IOP HLE */
        for (uint32_t i = 0; i < a1 && i < 32; i++) { uint32_t d = a0 + i * 16; iop_sifdma(R32(d), R32(d + 4), R32(d + 8), R32(d + 12)); }
        ret = ++sifdma_id; ctx->r[2].s64[0] = ret; if (num > 0) iop_flush(); return; }
    case 0x76: case -0x76: ret = -1; quiet = 1; break;              /* sceSifDmaStat: done */
    case 0x6b: ret = 0; break;
    default:
        L("syscall 0x%x (%d) UNHANDLED at 0x%x a0=%x a1=%x a2=%x a3=%x\n", num & 0xffff, num, pc, a0, a1, a2, a3);
        if (syscall_vec[num & 255]) L("  (patched by SetSyscall to 0x%x)\n", syscall_vec[num & 255]);
        ret = 0;
    }
    if (!quiet && nlog_sys++ < 2000 && bg_verbose) L("syscall %d(0x%x, 0x%x, 0x%x, 0x%x) -> 0x%llx  [pc 0x%x ra 0x%x]\n", num, a0, a1, a2, a3, (long long)ret, pc, ctx->r[31].u32[0]);
    ctx->r[2].s64[0] = ret;
}

/* ------------------------------------------------------------------ hardware register defaults */
static int is_chcr(uint32_t pa) {
    switch (pa) { case 0x10008000: case 0x10009000: case 0x1000A000: case 0x1000B000: case 0x1000B400: case 0x1000C000:
        case 0x1000C400: case 0x1000C800: case 0x1000D000: case 0x1000D400: return 1; } return 0;
}
static int chan_of(uint32_t pa) {
    switch (pa & 0xFFFFFF00) { case 0x10008000: return 0; case 0x10009000: return 1; case 0x1000A000: return 2; case 0x1000B000: return 3;
        case 0x1000B400: return 4; case 0x1000C000: return 5; case 0x1000C400: return 6; case 0x1000C800: return 7; case 0x1000D000: return 8; case 0x1000D400: return 9; } return -1;
}
static uint64_t dma_kicks[16], timer_tick, nhwpoll;
/* scratchpad DMA: channel 8 (fromSPR) copies SPR -> memory, channel 9 (toSPR) memory -> SPR; normal mode and
   source-chain mode for toSPR (tags in memory, as for VIF1). SADR is the scratchpad address. */
static uint64_t n_spr_qw[2];
static uint32_t R32hw(const uint8_t *hw, uint32_t off) { uint32_t v; memcpy(&v, hw + off, 4); return v; }
static void spr_dma(int ch, uint8_t *hw) {
    const uint32_t base = ch == 8 ? 0xD000 : 0xD400;
    uint32_t chcr, madr, qwc, tadr, sadr;
    memcpy(&chcr, hw + base, 4); memcpy(&madr, hw + base + 0x10, 4); memcpy(&qwc, hw + base + 0x20, 4); memcpy(&tadr, hw + base + 0x30, 4); memcpy(&sadr, hw + base + 0x80, 4);
    qwc &= 0xFFFF;
    uint8_t *spr = gptr(0x70000000u);
    const int mode = (chcr >> 2) & 3;
    { static int seen[2][4]; if (!seen[ch - 8][mode]++) L("SPR DMA ch%d first use of mode %d (chcr 0x%x qwc %u, D_SQWC 0x%x)\n", ch, mode, chcr, qwc, R32hw(hw, 0xE030)); }
    #define RAMP(a) ((a) & 0x80000000u ? spr + ((a) & 0x3FF0) : gptr((a) & 0x01FFFFF0u))
    for (int guard = 0; guard < 100000; guard++) {
        for (uint32_t i = 0; i < qwc; i++) {
            uint8_t *m = RAMP(madr), *sp = spr + (sadr & 0x3FF0);
            if (ch == 8) memcpy(m, sp, 16); else memcpy(sp, m, 16);
            madr += 16; sadr = (sadr + 16) & 0x3FFF; n_spr_qw[ch - 8]++;
        }
        qwc = 0;
        if (mode != 1 || ch != 9) break;
        uint64_t tag[2]; memcpy(tag, RAMP(tadr), 16);
        uint32_t tq = tag[0] & 0xFFFF, id = (tag[0] >> 28) & 7, addr = (uint32_t)(tag[0] >> 32) & 0x7FFFFFF0u; if ((tag[0] >> 63) & 1) addr |= 0x80000000u;
        int end = 0; qwc = tq;
        switch (id) {
        case 0: madr = addr; tadr += 16; end = 1; break;
        case 1: madr = tadr + 16; tadr = madr + tq * 16; break;
        case 2: madr = tadr + 16; tadr = addr; break;
        case 3: case 4: madr = addr; tadr += 16; break;
        default: madr = tadr + 16; end = 1; break;
        }
        if (end) { for (uint32_t i = 0; i < qwc; i++) { memcpy(spr + (sadr & 0x3FF0), RAMP(madr), 16); madr += 16; sadr = (sadr + 16) & 0x3FFF; n_spr_qw[1]++; } qwc = 0; break; }
    }
    #undef RAMP
    if (mode == 1 && ch == 8) { static int w; if (w++ < 5) L("DMA: fromSPR chain mode not implemented (chcr 0x%x)\n", chcr); }
    chcr &= ~0x100u;
    memcpy(hw + base, &chcr, 4); memcpy(hw + base + 0x10, &madr, 4); memcpy(hw + base + 0x20, &qwc, 4); memcpy(hw + base + 0x30, &tadr, 4); memcpy(hw + base + 0x80, &sadr, 4);
}
uint64_t ipu_read(uint32_t pa, int size); void ipu_write(uint32_t pa, uint64_t v, int size); void ipu_dma_start(int ch, uint8_t *hw); void ipu_dma_stop(int ch);
void ipu_init(uint8_t *hw); void ipu_dump_stats(void);
void hle_dma_complete(int ch) { dma_pending[ch] = 1; }
extern uint8_t *hwreg_base(void);
static void ipu_ensure_init(void) { static int init; if (!init) { init = 1; ipu_init(hwreg_base()); } }
uint64_t hle_hw_read(uint32_t pa, uint64_t v, int size) {
    if ((pa >= 0x10002000 && pa < 0x10002040) || (pa >= 0x10007000 && pa < 0x10007010) || pa == 0x1000B000 || pa == 0x1000B400) ipu_ensure_init();
    if ((pa >= 0x10002000 && pa < 0x10002040) || (pa >= 0x10007000 && pa < 0x10007010)) return ipu_read(pa & ~7u, size);
    if (pa == 0x1000B000 || pa == 0x1000B400) return v;          /* IPU DMA: STR stays set while the transfer is in progress (ipu.c) */
    if (is_chcr(pa)) return v & ~0x100ull;
    switch (pa) {
    case 0x10000000: case 0x10000800: case 0x10001000: case 0x10001800:
    case 0x10000010: case 0x10000810: case 0x10001010: case 0x10001810:
    case 0x10000020: case 0x10000820: case 0x10001020: case 0x10001820: if (timing_wall) return (timer_tick += 0x40) & 0xFFFF; REC_CYC(16); return hle_timer_read(pa, v);
    case 0x1000F000: if (timing_wall) { if ((++nhwpoll & 255) == 0) fire_vblank(); } else REC_CYC(64); return v | 0xC;   /* INTC_STAT: VBlank pending; polling burns time */
    case 0x1000E010: return v | 0x3FF;                          /* D_STAT: all channels done */
    case 0x10003020: case 0x10003800: case 0x10003C00: return 0;  /* GIF/VIF0/VIF1 STAT idle */
    case 0x1000F230: return v | 0x70000;                        /* SIF SMFLG */
    }
    return v;
}
void hle_hw_write(uint32_t pa, uint64_t v, int size, uint8_t *hw) {
    (void)hw; ipu_ensure_init();
    if (pa < 0x10002000 && (pa & 0x7FF) < 0x30 && ((pa & 0x7FF) & 0xF) == 0) hle_timer_write(pa, v);
    if ((pa >= 0x10002000 && pa < 0x10002040) || (pa >= 0x10007010 && pa < 0x10007020)) { ipu_write(pa, v, size); return; }
    if (pa == 0x1000B000 || pa == 0x1000B400) { if (v & 0x100) ipu_dma_start(pa == 0x1000B000 ? 3 : 4, hw); else ipu_dma_stop(pa == 0x1000B000 ? 3 : 4); return; }
    if (is_chcr(pa) && (v & 0x100)) {
        int ch = chan_of(pa); uint32_t base = pa & 0xFFFF;
        uint32_t madr, qwc, tadr; memcpy(&madr, hw + base + 0x10, 4); memcpy(&qwc, hw + base + 0x20, 4); memcpy(&tadr, hw + base + 0x30, 4);
        if (dma_kicks[ch]++ < 20) L("DMA kick ch%d chcr 0x%llx madr 0x%x qwc 0x%x tadr 0x%x [in 0x%x]\n", ch, (unsigned long long)v, madr, qwc & 0xffff, tadr, rec_cur);
        if (ch == 1 || ch == 2) dma_run(ch, hw);   /* VIF1 / GIF: run the transfer now (clears STR) */
        else if (ch == 8 || ch == 9) { spr_dma(ch, hw); dma_pending[ch] = 1; }
        else { uint32_t c = (uint32_t)v & ~0x100u; memcpy(hw + base, &c, 4); }
        dma_pending[ch] = 1;
    }
    if (pa == 0x1000E010) { uint32_t s; memcpy(&s, hw + 0xE010, 4); }
    if (pa >= 0x10006000 && pa < 0x10006010 && size == 8) gif_fifo_write(pa & 15, v);
}
static uint64_t csr_reads;
static int gs_ready;
uint64_t hle_gs_read(uint32_t pa, uint64_t v, int size) {
    if (pa == 0x12001000) { csr_reads++; if (timing_wall) { if ((csr_reads & 63) == 0) fire_vblank(); } else REC_CYC(64); return (v & ~0x200ull) | 0x8 | 0x2 | ((csr_reads & 1) << 13); }
    return v;
}
void hle_gs_write(uint32_t pa, uint64_t v, int size, uint8_t *gs) { (void)pa; (void)v; (void)size; if (!gs_ready++) gs_init(gs); }

void hle_dump(void) {
    L("EE timers: interrupts %llu/%llu/%llu/%llu; modes %x/%x/%x/%x comp %x/%x/%x/%x next %lld/%lld/%lld/%lld (now %llu)\n", (unsigned long long)n_timer_irq[0], (unsigned long long)n_timer_irq[1],
      (unsigned long long)n_timer_irq[2], (unsigned long long)n_timer_irq[3], tmr[0].mode, tmr[1].mode, tmr[2].mode, tmr[3].mode, tmr[0].comp, tmr[1].comp, tmr[2].comp, tmr[3].comp,
      (long long)tmr[0].next, (long long)tmr[1].next, (long long)tmr[2].next, (long long)tmr[3].next, (unsigned long long)ee_cycles);
    gs_dump_stats(); dma_dump_stats(); vu1_dump_stats(); ipu_dump_stats(); iop_dump();
    L("SPR DMA: %llu qw from scratchpad, %llu qw to scratchpad\n", (unsigned long long)n_spr_qw[0], (unsigned long long)n_spr_qw[1]);
    L("CD: %llu read calls, %llu sectors from DATA.CVM\n", (unsigned long long)n_cdreads, (unsigned long long)cd_sectors_read);
    L("IOP RPC: %d servers bound, %llu calls\n", nbinds, (unsigned long long)n_rpc_calls);
    for (int i = 0; i < nbinds; i++) L("  sid 0x%08x (%s): %llu calls\n", binds[i].sid, sidname(binds[i].sid), (unsigned long long)binds[i].calls);
    L("threads/semas at stop (current thread %d, %llu switches, %llu vblanks (%llu timer-driven), %llu idle ticks):\n", cur, (unsigned long long)nswitch, (unsigned long long)vblank_count, (unsigned long long)async_vblanks, (unsigned long long)idle_ticks);
    dump_threads();
    L("syscalls (number: count):\n");
    for (int i = 0; i < 512; i++) if (sys_cnt[i]) L("  %d (0x%x): %llu\n", i < 256 ? i : -(i - 256), i < 256 ? i : i - 256, (unsigned long long)sys_cnt[i]);
    for (int c = 0; c < 16; c++) if (dma_kicks[c]) L("  DMA channel %d kicks: %llu\n", c, (unsigned long long)dma_kicks[c]);
}

/* ------------------------------------------------------------------ boot */
static void main_trampoline(void) {
    Thread *t = &th[1];
    rec_call(&t->ctx, t->entry);
    stop("crt0 returned (main/exit finished)");
}
uint64_t hle_vblank_count(void) { return vblank_count; }
void hle_init(void) {
    /* fake kernel syscall table in kernel RAM (libkernel searches 0x80000000-0x80080000 for it) */
    for (int i = 0; i < 512; i++) W32(SYSTAB + 4 * i, 0x80001000u + 16 * i);
}
void hle_run_main(uint32_t entry) {
    Thread *t = &th[1];
    memset(t, 0, sizeof *t);
    t->used = 1; t->entry = entry; t->prio = t->init_prio = 1; t->status = THS_RUN; t->seq = seqno++;
    t->ctx.r[29].u64[0] = 0x01FFFF00; t->ctx.r[31].u64[0] = 0xDEAD0000u; t->ctx.vf[0][3] = 1.0f;
    host_fiber_make(&t->fib, main_trampoline, 256 << 20);
    HostFiber boot = {0};
    cur = 1;
    host_fiber_switch(&boot, &t->fib);
    stop("unexpected return to boot context");
}
int hle_cur_thread(void) { return cur; }
