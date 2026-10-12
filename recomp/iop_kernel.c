/* IOP kernel on the host: IRX loader/linker, the kernel libraries the game's sound modules import
   (sysmem, loadcore, intrman, thbase, thsemap, thevent, timrman, sifcmd, sifman, stdio, sysclib, ioman,
   cdvdman, vblank), IOP threads and time, interrupts, and the SIF RPC bridge to the EE side (hle.c).
   Function numbers come from ps2sdk's export tables. Real modules export their own libraries
   (libsd, modhsyn, modmidi, modmsin) through RegisterLibraryEntries and are linked to each other. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>
#include "ee_runtime.h"
#include "hle.h"
#include "iop.h"

#define TRAP(n) (IOP_HLE_BASE + 8u * (n))
enum { T_CALLRET = 0, T_THREADEXIT = 1, T_RPCRET = 2, T_UNRESOLVED = 3, T_FIRST_FN = 8 };
static uint64_t now;                     /* IOP time in 36.864 MHz cycles */
static uint64_t vblanks;
#define R iop.r
static uint32_t RD32(uint32_t a) { return iop_read(a, 4); }
static void WR32(uint32_t a, uint32_t v) { iop_write(a, v, 4); }
static const char *istr(uint32_t a) { static char b[4][512]; static int k; char *o = b[k++ & 3]; int i = 0;
    for (; i < 511; i++) { uint8_t *p = iop_ptr(a + i); if (!p || !*p) break; o[i] = *p; } o[i] = 0; return o; }
static uint32_t arg(int i) { return i < 4 ? R[4 + i] : RD32(R[29] + 16 + 4 * (i - 4)); }

static int fault_count;
void iop_fault(const char *fmt, ...) {
    char b[300]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    extern uint32_t iop_jtrace[32]; extern int iop_jtrace_n;
    if (fault_count++ < 20) { L("%s [ra 0x%08x, sp 0x%08x, gp 0x%08x]\n  recent jr/jalr (from -> to):", b, iop.r[31], iop.r[29], iop.r[28]);
        for (int k = 0; k < 32; k += 2) { int j = (iop_jtrace_n - 32 + k) & 31; L(" %x>%x", iop_jtrace[j], iop_jtrace[(j + 1) & 31]); } L("\n");
        extern uint32_t iop_ptrace[64]; extern int iop_ptrace_n; L("  last PCs:"); for (int k = 40; k > 0; k--) L(" %x", iop_ptrace[(iop_ptrace_n - k) & 63]); L("\n");
        uint32_t c = iop_ptrace[(iop_ptrace_n - 40) & 63] & ~3u; L("  memory at 0x%x:", c); for (int k = 0; k < 44; k++) L(" %08x", iop_read(c + 4 * k, 4)); L("\n"); }
    iop.yield = 1; iop.pc = TRAP(T_THREADEXIT);     /* kill the faulting thread */
}
static int unmapped_count;
void iop_unmapped(uint32_t pa, uint32_t v, int size, int write) {
    if (unmapped_count++ < 50) L("IOP: unmapped %s%d 0x%08x = 0x%x (pc 0x%08x)\n", write ? "write" : "read", size * 8, pa, v, iop.pc);
}

/* ================================================================== sysmem */
typedef struct { uint32_t addr, size; int used; } Blk;
static Blk blk[512]; static int nblk;
#define HEAP_LO 0x00020000u
#define HEAP_HI 0x001C0000u
static void heap_init(void) { if (nblk) return; blk[0] = (Blk){ HEAP_LO, HEAP_HI - HEAP_LO, 0 }; nblk = 1; }
static uint32_t heap_alloc(int mode, uint32_t size, uint32_t want) {
    heap_init(); size = (size + 255) & ~255u; if (!size) size = 256;
    int pick = -1;
    for (int i = 0; i < nblk; i++) {
        if (blk[i].used) continue;
        if (mode == 2) { if (want >= blk[i].addr && want + size <= blk[i].addr + blk[i].size) { pick = i; break; } continue; }
        if (blk[i].size >= size) { pick = i; if (mode == 0) break; }
    }
    if (pick < 0 || nblk >= 510) return 0;
    Blk b = blk[pick]; uint32_t a = mode == 1 ? b.addr + b.size - size : mode == 2 ? (want & ~255u) : b.addr;
    /* split [b.addr, a) free, [a, a+size) used, [a+size, end) free */
    Blk parts[3]; int np = 0;
    if (a > b.addr) parts[np++] = (Blk){ b.addr, a - b.addr, 0 };
    parts[np++] = (Blk){ a, size, 1 };
    if (a + size < b.addr + b.size) parts[np++] = (Blk){ a + size, b.addr + b.size - a - size, 0 };
    memmove(&blk[pick + np], &blk[pick + 1], (nblk - pick - 1) * sizeof(Blk)); memcpy(&blk[pick], parts, np * sizeof(Blk)); nblk += np - 1;
    memset(iop_ram + a, 0, size);
    return a;
}
static int heap_free(uint32_t a) {
    for (int i = 0; i < nblk; i++) if (blk[i].used && blk[i].addr == a) {
        blk[i].used = 0;
        if (i + 1 < nblk && !blk[i + 1].used) { blk[i].size += blk[i + 1].size; memmove(&blk[i + 1], &blk[i + 2], (nblk - i - 2) * sizeof(Blk)); nblk--; }
        if (i > 0 && !blk[i - 1].used) { blk[i - 1].size += blk[i].size; memmove(&blk[i], &blk[i + 1], (nblk - i - 1) * sizeof(Blk)); nblk--; }
        return 0;
    }
    return -1;
}
uint32_t iop_heap_alloc(uint32_t size) { return heap_alloc(0, size, 0); }
int iop_heap_free(uint32_t a) { return heap_free(a); }
static uint32_t heap_free_total(int maxonly) { uint32_t t = 0, m = 0; heap_init(); for (int i = 0; i < nblk; i++) if (!blk[i].used) { t += blk[i].size; if (blk[i].size > m) m = blk[i].size; } return maxonly ? m : t; }

/* ================================================================== threads */
enum { TS_FREE = 0, TS_DORMANT, TS_READY, TS_WAIT };
enum { W_NONE = 0, W_SEMA, W_EVENT, W_DELAY, W_VBLANK, W_RPC, W_SLEEP, W_START, W_SIFC };
typedef struct {
    int status, prio, wait, wait_id; uint32_t entry, stack, stack_size, gp, attr, option;
    uint32_t ev_bits, ev_mode, ev_res; uint64_t wake; uint64_t seq; int wakeups;
    IopCpu ctx; int exit_code; int is_loader; uint32_t rpc_qd; int ie;   /* ie: interrupt enable (part of the thread context) */
} Th;
#define MAXTH 64
static Th th[MAXTH]; static int cur = 0;
static int live;    /* 1 while the CPU registers belong to thread cur (else th[cur].ctx is authoritative) */ static uint64_t seqc = 1; static int in_intr;
static uint64_t nswitch;
static int intr_enabled = 1;
static void th_save(int i) { if (i > 0) { th[i].ctx = iop; th[i].ie = intr_enabled; } }
static void th_load(int i) { uint32_t c0[32]; memcpy(c0, iop.cop0, sizeof c0); iop = th[i].ctx; memcpy(iop.cop0, c0, sizeof c0); iop.yield = 0; intr_enabled = th[i].ie; }
static void make_ready(int i) { th[i].status = TS_READY; th[i].wait = W_NONE; th[i].seq = seqc++; }
static int pick_thread(void) {
    int b = 0;
    for (int i = 1; i < MAXTH; i++) if (th[i].status == TS_READY && (!b || th[i].prio < th[b].prio || (th[i].prio == th[b].prio && th[i].seq < th[b].seq))) b = i;
    return b;
}
static void block(int wait, int id) {       /* current thread waits; leaves iop_exec */
    th[cur].status = TS_WAIT; th[cur].wait = wait; th[cur].wait_id = id; iop.yield = 1;
}
static int th_alloc(void) { for (int i = 1; i < MAXTH; i++) if (th[i].status == TS_FREE) return i; return -1; }
static void th_start(int i, uint32_t a0) {
    Th *t = &th[i]; memset(&t->ctx, 0, sizeof t->ctx);
    t->ctx.pc = t->entry; t->ctx.r[4] = a0; t->ctx.r[28] = t->gp; t->ctx.r[29] = (t->stack + t->stack_size - 0x30) & ~15u; t->ctx.r[31] = TRAP(T_THREADEXIT);
    t->wakeups = 0; t->ie = 1; make_ready(i);
}

/* ---------------------------------------------------------------- semaphores / event flags */
typedef struct { int used, count, max; uint32_t attr; } Sem;
static Sem sem[128];
typedef struct { int used; uint32_t bits, attr; } Ev;
static Ev evf[64];
static void sema_wake(int id) {
    int w = 0; for (int i = 1; i < MAXTH; i++) if (th[i].status == TS_WAIT && th[i].wait == W_SEMA && th[i].wait_id == id && (!w || th[i].seq < th[w].seq)) w = i;
    if (w) { make_ready(w); th[w].ctx.r[2] = 0; } else if (sem[id].count < sem[id].max) sem[id].count++;
}
static int ev_match(int id, uint32_t bits, uint32_t mode) { uint32_t b = evf[id].bits; return (mode & 1) ? (b & bits) != 0 : (b & bits) == bits; }
static void ev_check(int id) {
    for (int i = 1; i < MAXTH; i++) if (th[i].status == TS_WAIT && th[i].wait == W_EVENT && th[i].wait_id == id && ev_match(id, th[i].ev_bits, th[i].ev_mode)) {
        if (th[i].ev_res) WR32(th[i].ev_res, evf[id].bits);
        if (th[i].ev_mode & 0x10) evf[id].bits = 0;
        make_ready(i); th[i].ctx.r[2] = 0;
    }
}

/* ================================================================== interrupts and nested calls */
typedef struct { uint32_t fn, arg; int enabled; } Irq;
static Irq irq[64]; static uint64_t irq_pending;
static int call_done;
#define INTR_STACK 0x0001EF00u
static uint32_t iop_call(uint32_t fn, uint32_t a0, uint32_t a1, uint32_t a2) {     /* run guest code to completion (interrupt-style) */
    IopCpu save = iop;
    memset(iop.r, 0, sizeof iop.r);
    iop.r[4] = a0; iop.r[5] = a1; iop.r[6] = a2; iop.r[28] = save.r[28]; iop.r[29] = INTR_STACK - 0x40 * in_intr; iop.r[31] = TRAP(T_CALLRET);
    iop.pc = fn; iop.in_delay = 0; iop.yield = 0;
    int saved_done = call_done, saved_ie = intr_enabled; call_done = 0; in_intr++;
    long guard = 0;
    while (!call_done && guard < 50000000) { guard += iop_exec(10000); if (iop.yield && !call_done) { iop.yield = 0; } }
    in_intr--; call_done = saved_done; intr_enabled = saved_ie;   /* the kernel restores the interrupt state on handler exit */
    uint32_t ret = iop.r[2];
    uint32_t c0[32]; memcpy(c0, iop.cop0, sizeof c0); iop = save; memcpy(iop.cop0, c0, sizeof c0);
    if (guard >= 50000000) L("IOP: nested call to 0x%08x did not return\n", fn);
    return ret;
}
void iop_raise_irq(int n) { irq_pending |= 1ull << n; }
static void deliver_irqs(void) {
    if (!intr_enabled || in_intr || !irq_pending) return;
    for (int n = 0; n < 64; n++) if ((irq_pending >> n) & 1) {
        irq_pending &= ~(1ull << n);
        if (irq[n].fn && irq[n].enabled) iop_call(irq[n].fn, irq[n].arg, 0, 0);
    }
}

/* ================================================================== timers (timrman) */
typedef struct { int used, running; uint32_t source, prescale, compare, handler, common; uint64_t next; } Tmr;
static Tmr tmr[6];
static double tmr_cycles_per_tick(Tmr *t) {
    double hz = t->source & 1 ? IOP_CLOCK : t->source & 2 ? 13500000.0 : t->source & 4 ? 15734.26573 : IOP_CLOCK;
    return IOP_CLOCK / hz * (t->prescale ? t->prescale : 1);
}
static void tmr_schedule(Tmr *t) { t->next = now + (uint64_t)(tmr_cycles_per_tick(t) * (t->compare ? t->compare : 1)); }
static uint64_t ntimer_calls;
static void run_timers(void) {
    if (!intr_enabled || in_intr) return;
    for (int i = 0; i < 6; i++) { Tmr *t = &tmr[i];
        while (t->running && t->handler && t->next <= now) {
            ntimer_calls++;
            uint32_t r = iop_call(t->handler, t->common, 0, 0);
            if (!r) { t->running = 0; break; }
            t->compare = r; t->next += (uint64_t)(tmr_cycles_per_tick(t) * r);
        }
    }
}

/* ================================================================== libraries */
typedef void (*HleFn)(void);
typedef struct { const char *lib; int idx; HleFn fn; const char *name; } Hle;
typedef struct { char name[9]; uint32_t table; } Exp;
static Exp exps[32]; static int nexps;

/* ---- sysmem */
static void f_AllocSysMemory(void) { R[2] = heap_alloc(R[4], R[5], R[6]); }
static void f_FreeSysMemory(void) { R[2] = heap_free(R[4]); }
static void f_QueryMaxFree(void) { R[2] = heap_free_total(1); }
static void f_QueryTotalFree(void) { R[2] = heap_free_total(0); }
/* ---- loadcore */
static void f_FlushDcache(void) { }
static void f_RegisterLibraryEntries(void) {
    uint32_t t = R[4]; char nm[9]; for (int i = 0; i < 8; i++) nm[i] = iop_read(t + 12 + i, 1); nm[8] = 0;
    for (int i = 0; i < nexps; i++) if (!strcmp(exps[i].name, nm)) { exps[i].table = t; R[2] = 0; return; }
    if (nexps < 32) { strcpy(exps[nexps].name, nm); exps[nexps++].table = t; }
    L("IOP: RegisterLibraryEntries(\"%s\") table 0x%x\n", nm, t); R[2] = 0;
}
/* ---- intrman */
static void f_RegisterIntrHandler(void) { int n = R[4] & 63; irq[n].fn = R[6]; irq[n].arg = R[7]; L("IOP: RegisterIntrHandler(%d, fn 0x%x, arg 0x%x)\n", n, R[6], R[7]); R[2] = 0; }
static void f_ReleaseIntrHandler(void) { irq[R[4] & 63].fn = 0; R[2] = 0; }
static void f_EnableIntr(void) { irq[R[4] & 63].enabled = 1; R[2] = 0; }
static void f_DisableIntr(void) { int n = R[4] & 63; if (R[5]) WR32(R[5], irq[n].enabled ? n : 0); irq[n].enabled = 0; R[2] = 0; }
static void f_CpuSuspendIntr(void) { if (R[4]) WR32(R[4], intr_enabled); R[2] = intr_enabled ? 0 : (uint32_t)-102; intr_enabled = 0; }
static void f_CpuResumeIntr(void) { intr_enabled = R[4] ? 1 : 0; R[2] = 0; }
static void f_QueryIntrContext(void) { R[2] = in_intr > 0; }
/* ---- thbase */
static void f_CreateThread(void) {
    uint32_t p = R[4]; int i = th_alloc(); if (i < 0) { R[2] = (uint32_t)-400; return; }
    Th *t = &th[i]; memset(t, 0, sizeof *t);
    t->attr = RD32(p); t->option = RD32(p + 4); t->entry = RD32(p + 8); t->stack_size = RD32(p + 12); t->prio = RD32(p + 16);
    t->stack = heap_alloc(1, t->stack_size, 0); t->gp = R[28]; t->status = TS_DORMANT;
    L("IOP: CreateThread(entry 0x%x, stack 0x%x, prio %d) -> %d\n", t->entry, t->stack_size, t->prio, i); R[2] = i;
}
static void f_DeleteThread(void) { int i = R[4]; if (i > 0 && i < MAXTH && th[i].status == TS_DORMANT) { heap_free(th[i].stack); th[i].status = TS_FREE; } R[2] = 0; }
static void f_StartThread(void) { int i = R[4]; if (i <= 0 || i >= MAXTH || th[i].status != TS_DORMANT) { R[2] = (uint32_t)-406; return; }
    th_start(i, R[5]); R[2] = 0; if (th[i].prio < th[cur].prio) iop.yield = 1; }
static void f_ExitThread(void) { th[cur].status = TS_DORMANT; iop.yield = 1; }
static void f_TerminateThread(void) { int i = R[4]; if (i > 0 && i < MAXTH && i != cur) th[i].status = TS_DORMANT; R[2] = 0; }
static void f_ChangeThreadPriority(void) { int i = R[4] ? (int)R[4] : cur; if (i > 0 && i < MAXTH) { th[i].prio = R[5]; th[i].seq = seqc++; } R[2] = 0; iop.yield = 1; }
static void f_GetThreadId(void) { R[2] = cur; }
static void f_ReferThreadStatus(void) { int i = R[4] ? (int)R[4] : cur; uint32_t p = R[5];
    if (p) { WR32(p, th[i].attr); WR32(p + 4, th[i].option); WR32(p + 8, th[i].status == TS_READY ? 2 : th[i].status == TS_WAIT ? 4 : 0x10);
             WR32(p + 12, th[i].entry); WR32(p + 16, th[i].stack); WR32(p + 20, th[i].stack_size); WR32(p + 24, th[i].gp);
             WR32(p + 28, th[i].prio); WR32(p + 32, th[i].prio); } R[2] = 0; }
static void f_DelayThread(void) { th[cur].wake = now + (uint64_t)(R[4] * (IOP_CLOCK / 1e6)); R[2] = 0; block(W_DELAY, 0); }
static void f_GetSystemTime(void) { WR32(R[4], (uint32_t)now); WR32(R[4] + 4, (uint32_t)(now >> 32)); R[2] = 0; }
static void f_USec2SysClock(void) { uint64_t c = (uint64_t)(R[4] * (IOP_CLOCK / 1e6)); WR32(R[5], (uint32_t)c); WR32(R[5] + 4, (uint32_t)(c >> 32)); R[2] = 0; }
static void f_SysClock2USec(void) { uint64_t c = RD32(R[4]) | (uint64_t)RD32(R[4] + 4) << 32; uint64_t us = (uint64_t)(c / (IOP_CLOCK / 1e6));
    WR32(R[5], (uint32_t)(us / 1000000)); WR32(R[6], (uint32_t)(us % 1000000)); R[2] = 0; }
static void f_SleepThread(void) { if (th[cur].wakeups > 0) { th[cur].wakeups--; R[2] = 0; return; } R[2] = 0; block(W_SLEEP, 0); }
static void f_WakeupThread(void) { int i = R[4]; if (i > 0 && i < MAXTH) { if (th[i].status == TS_WAIT && th[i].wait == W_SLEEP) make_ready(i); else th[i].wakeups++; } R[2] = 0; }
/* ---- thsemap */
static void f_CreateSema(void) { uint32_t p = R[4]; for (int i = 1; i < 128; i++) if (!sem[i].used) {
    sem[i] = (Sem){ 1, (int)RD32(p + 8), (int)RD32(p + 12), RD32(p) }; if (!sem[i].max) sem[i].max = 0x7FFFFFFF; R[2] = i; return; } R[2] = (uint32_t)-400; }
static void f_DeleteSema(void) { if (R[4] < 128) sem[R[4]].used = 0; R[2] = 0; }
static void f_SignalSema(void) { int id = R[4]; if (id <= 0 || id >= 128 || !sem[id].used) { R[2] = (uint32_t)-408; return; } sema_wake(id); R[2] = 0; if (!in_intr) iop.yield = 1; }
static void f_WaitSema(void) { int id = R[4]; if (id <= 0 || id >= 128 || !sem[id].used) { R[2] = (uint32_t)-408; return; }
    if (sem[id].count > 0) { sem[id].count--; R[2] = 0; return; } R[2] = 0; th[cur].seq = seqc++; block(W_SEMA, id); }
static void f_PollSema(void) { int id = R[4]; if (id > 0 && id < 128 && sem[id].used && sem[id].count > 0) { sem[id].count--; R[2] = 0; } else R[2] = (uint32_t)-419; }
/* ---- thevent */
static void f_CreateEventFlag(void) { uint32_t p = R[4]; for (int i = 1; i < 64; i++) if (!evf[i].used) { evf[i] = (Ev){ 1, RD32(p + 8), RD32(p) }; R[2] = i; return; } R[2] = (uint32_t)-400; }
static void f_DeleteEventFlag(void) { if (R[4] < 64) evf[R[4]].used = 0; R[2] = 0; }
static void f_SetEventFlag(void) { int id = R[4]; if (id <= 0 || id >= 64) { R[2] = (uint32_t)-409; return; } evf[id].bits |= R[5]; ev_check(id); R[2] = 0; if (!in_intr) iop.yield = 1; }
static void f_ClearEventFlag(void) { int id = R[4]; if (id > 0 && id < 64) evf[id].bits &= R[5]; R[2] = 0; }
static void f_WaitEventFlag(void) { int id = R[4]; uint32_t bits = R[5], mode = R[6], res = R[7];
    if (id <= 0 || id >= 64) { R[2] = (uint32_t)-409; return; }
    if (ev_match(id, bits, mode)) { if (res) WR32(res, evf[id].bits); if (mode & 0x10) evf[id].bits = 0; R[2] = 0; return; }
    th[cur].ev_bits = bits; th[cur].ev_mode = mode; th[cur].ev_res = res; R[2] = 0; block(W_EVENT, id); }
static void f_ReferEventFlagStatus(void) { int id = R[4]; uint32_t p = R[5]; if (p) { WR32(p, evf[id].attr); WR32(p + 4, 0); WR32(p + 8, 0); WR32(p + 12, evf[id].bits); } R[2] = 0; }
/* ---- timrman */
static void f_AllocHardTimer(void) { for (int i = 0; i < 6; i++) if (!tmr[i].used) { tmr[i] = (Tmr){ 1, 0, R[4], R[6], 0, 0, 0, 0 }; R[2] = ((i + 1) << 28) | 0x100; L("IOP: AllocHardTimer(source %d, size %d, prescale %d) -> 0x%x\n", R[4], R[5], R[6], R[2]); return; } R[2] = (uint32_t)-150; }
static Tmr *tmr_of(uint32_t id) { uint32_t i = (id >> 28) - 1; return i < 6 ? &tmr[i] : 0; }
static void f_FreeHardTimer(void) { Tmr *t = tmr_of(R[4]); if (t) t->used = t->running = 0; R[2] = 0; }
static void f_SetTimerHandler(void) { Tmr *t = tmr_of(R[4]); if (t) { t->compare = R[5]; t->handler = R[6]; t->common = R[7]; } R[2] = 0; }
static void f_SetupHardTimer(void) { Tmr *t = tmr_of(R[4]); if (t) { t->source = R[5]; t->prescale = R[7]; } R[2] = 0; }
static void f_StartHardTimer(void) { Tmr *t = tmr_of(R[4]); if (t) { t->running = 1; tmr_schedule(t);
    L("IOP: StartHardTimer 0x%x: compare %u, every %.1f us, handler 0x%x\n", R[4], t->compare, tmr_cycles_per_tick(t) * t->compare / (IOP_CLOCK / 1e6), t->handler); } R[2] = 0; }
static void f_StopHardTimer(void) { Tmr *t = tmr_of(R[4]); if (t) t->running = 0; R[2] = 0; }

/* ---- sifcmd: RPC servers */
typedef struct { uint32_t sd, sid, func, buf, cfunc, cbuf, qd; } Srv;
static Srv srv[32]; static int nsrv;
typedef struct { uint32_t qd; int thid; } Que;
static Que que[16]; static int nque;
typedef struct { int srv; uint32_t fno, size, recv, rsize; uint32_t endpkt[8]; } Req;
static Req rq[64]; static int nrq; static int rpc_active_th;   /* requests waiting; the one being served is rq[0] when a thread runs it */
static uint64_t n_iop_rpc;
static void f_sceSifInitRpc(void) { R[2] = 0; }
static void f_sceSifRegisterRpc(void) {
    if (nsrv >= 32) { R[2] = 0; return; }
    Srv *s = &srv[nsrv++]; s->sd = R[4]; s->sid = R[5]; s->func = R[6]; s->buf = R[7]; s->cfunc = arg(4); s->cbuf = arg(5); s->qd = arg(6);
    L("IOP: sceSifRegisterRpc(sid 0x%08x, func 0x%x, buf 0x%x, queue 0x%x)\n", s->sid, s->func, s->buf, s->qd); R[2] = 0;
}
static void f_sceSifSetRpcQueue(void) { if (nque < 16) { que[nque].qd = R[4]; que[nque++].thid = R[5]; } R[2] = 0; }
static void serve_next(int t);
static void f_sceSifRpcLoop(void) { th[cur].rpc_qd = R[4]; serve_next(cur); }
static void f_sceSifRemoveRpc(void) { R[2] = 0; }
/* IOP as RPC client of an EE server: packets go to the game's own EE libsif (SifRpcClientData: buf +0x14, cbuf +0x18,
   end_function +0x1c, end_param +0x20, server +0x24) */
static uint64_t n_ee_rpc;
static void f_sceSifBindRpc(void) {
    uint32_t cd = R[4], sid = R[5], mode = R[6];
    WR32(cd + 0x24, 0);
    uint32_t pl[5] = { 0, cd, 0, cd, sid };
    hle_iop_send_cmd(0x80000009, pl, 5);
    R[2] = 0;
    if (!(mode & 1)) block(W_SIFC, (int)cd);
}
static void f_sceSifCallRpc(void) {
    uint32_t cd = R[4], fno = R[5], mode = R[6], send = R[7], ssize = arg(4), recv = arg(5), rsize = arg(6), endf = arg(7), endp = arg(8);
    uint32_t buf = RD32(cd + 0x14);
    if (ssize && buf) { uint8_t *p = iop_ptr(send); if (p) memcpy(gptr(buf), p, ssize); }
    WR32(cd + 0x1c, endf); WR32(cd + 0x20, endp);
    uint32_t pl[10] = { 0, cd, 0, cd, fno, ssize, recv, rsize, 1, RD32(cd + 0x24) };
    hle_iop_send_cmd(0x8000000A, pl, 10); n_ee_rpc++;
    R[2] = 0;
    if (!(mode & 1)) block(W_SIFC, (int)cd);
}
/* RPC END from the EE (words = whole packet): bind result or call completion */
void iop_ee_rpc_end(const uint32_t *w) {
    uint32_t cd = w[7], cid = w[8];
    if (cid == 0x80000009) { WR32(cd + 0x24, w[9]); WR32(cd + 0x14, w[10]); WR32(cd + 0x18, w[11]);
        L("IOP: bind from IOP client 0x%x -> EE server 0x%x\n", cd, w[9]); }
    else if (cid == 0x8000000A) { uint32_t f = RD32(cd + 0x1c); if (f) iop_call(f, RD32(cd + 0x20), 0, 0); }
    for (int i = 1; i < MAXTH; i++) if (th[i].status == TS_WAIT && th[i].wait == W_SIFC && (uint32_t)th[i].wait_id == cd) make_ready(i);
}
static void f_sceSifCheckStatRpc(void) { R[2] = 0; }
/* a server thread asks for work: start the next request for its queue, or wait */
static int req_for_queue(uint32_t qd) { for (int i = 0; i < nrq; i++) if (srv[rq[i].srv].qd == qd) return i; return -1; }
static Req serving[MAXTH];
static void serve_next(int t) {
    uint32_t qd = th[t].rpc_qd;
    int k = req_for_queue(qd);
    if (k < 0) { th[t].status = TS_WAIT; th[t].wait = W_RPC; if (t == cur && live) iop.yield = 1; return; }
    Req r = rq[k]; memmove(&rq[k], &rq[k + 1], (nrq - k - 1) * sizeof(Req)); nrq--;
    serving[t] = r;
    int running = t == cur && live;
    IopCpu *c = running ? &iop : &th[t].ctx;
    c->pc = srv[r.srv].func; c->in_delay = 0; c->r[4] = r.fno; c->r[5] = srv[r.srv].buf; c->r[6] = r.size; c->r[31] = TRAP(T_RPCRET);
    if (!running) make_ready(t);
}
static void rpc_return(void) {             /* server function returned: v0 = reply data */
    Req *r = &serving[cur]; uint32_t data = R[2];
    if (r->recv && r->rsize && data) { uint8_t *p = iop_ptr(data); if (p) memcpy(gptr(r->recv), p, r->rsize); }
    hle_iop_rpc_end(r->endpkt);
    n_iop_rpc++;
    if (getenv("HG_RPCLOG")) L("IOP: RPC done sid 0x%08x fn 0x%x (thread %d) reply 0x%x -> %08x\n", srv[r->srv].sid, r->fno, cur, data, data ? RD32(data) : 0);
    serve_next(cur);
}
/* ---- sifman */
static uint32_t sifdma_id = 1;
static void f_sceSifInit(void) { R[2] = 0; }
static void f_sceSifSetDma(void) {
    uint32_t d = R[4]; int n = R[5];
    for (int i = 0; i < n; i++) { uint32_t src = RD32(d + 16 * i), dst = RD32(d + 16 * i + 4), size = RD32(d + 16 * i + 8);
        uint8_t *p = iop_ptr(src); if (p && size < 0x100000) memcpy(gptr(dst & 0x1FFFFFFF), p, size); }
    R[2] = sifdma_id++;
}
static void f_sceSifDmaStat(void) { R[2] = (uint32_t)-1; }
static void f_sceSifCheckInit(void) { R[2] = 1; }
/* ---- stdio / sysclib */
static int fmt(char *out, size_t n, const char *f, int ai) {
    size_t o = 0;
    for (; *f && o + 1 < n; f++) {
        if (*f != '%') { out[o++] = *f; continue; }
        char spec[16] = "%"; int k = 1; f++;
        while (*f && strchr("-+ #0123456789.lh", *f) && k < 14) { if (*f != 'l' && *f != 'h') spec[k++] = *f; f++; }
        char c = *f; if (!c) break;
        spec[k++] = c == 'p' ? 'x' : c; spec[k] = 0;
        if (c == 's') o += snprintf(out + o, n - o, spec, istr(arg(ai++)));
        else if (c == '%') out[o++] = '%';
        else o += snprintf(out + o, n - o, spec, arg(ai++));
        if (o >= n) o = n - 1;
    }
    out[o] = 0; return (int)o;
}
static void f_printf(void) { char b[1024]; int n = fmt(b, sizeof b, istr(R[4]), 1); L("IOP printf: %s%s", b, n && b[n - 1] == '\n' ? "" : "\n"); R[2] = n; }
static void f_sprintf(void) { char b[1024]; int n = fmt(b, sizeof b, istr(R[5]), 2); for (int i = 0; i <= n; i++) iop_write(R[4] + i, b[i], 1); R[2] = n; }
static uint32_t ctype_tab;
static void f_look_ctype_table(void) {
    if (!ctype_tab) { ctype_tab = heap_alloc(0, 256, 0);
        for (int c = 0; c < 128; c++) { int f = 0; if (isupper(c)) f |= 1; if (islower(c)) f |= 2; if (isdigit(c)) f |= 4; if (isspace(c)) f |= 8;
            if (ispunct(c)) f |= 16; if (iscntrl(c)) f |= 32; if (isxdigit(c)) f |= 64; if (c == ' ') f |= 128; iop_ram[ctype_tab + c] = f; } }
    R[2] = ctype_tab;
}
static void f_memcmp(void) { int r = 0; for (uint32_t i = 0; i < R[6] && !r; i++) r = (int)iop_read(R[4] + i, 1) - (int)iop_read(R[5] + i, 1); R[2] = r; }
static void f_memcpy(void) { uint8_t *d = iop_ptr(R[4]), *s = iop_ptr(R[5]); if (d && s) memmove(d, s, R[6]); R[2] = R[4]; }
static void f_memset(void) { uint8_t *d = iop_ptr(R[4]); if (d) memset(d, R[5] & 0xFF, R[6]); R[2] = R[4]; }
static void f_bzero(void) { uint8_t *d = iop_ptr(R[4]); if (d) memset(d, 0, R[5]); R[2] = R[4]; }
static void f_strcat(void) { uint32_t d = R[4]; while (iop_read(d, 1)) d++; uint32_t s = R[5]; uint8_t c; do { c = iop_read(s++, 1); iop_write(d++, c, 1); } while (c); R[2] = R[4]; }
static void f_strcpy(void) { uint32_t d = R[4], s = R[5]; uint8_t c; do { c = iop_read(s++, 1); iop_write(d++, c, 1); } while (c); R[2] = R[4]; }
static void f_strlen(void) { uint32_t n = 0; while (iop_read(R[4] + n, 1)) n++; R[2] = n; }
static void f_strncmp(void) { int r = 0; for (uint32_t i = 0; i < R[6]; i++) { int a = iop_read(R[4] + i, 1), b = iop_read(R[5] + i, 1); if (a != b) { r = a - b; break; } if (!a) break; } R[2] = r; }
static void f_strncpy(void) { uint32_t i = 0; for (; i < R[6]; i++) { uint8_t c = iop_read(R[5] + i, 1); iop_write(R[4] + i, c, 1); if (!c) break; } for (; i < R[6]; i++) iop_write(R[4] + i, 0, 1); R[2] = R[4]; }
static void f_strrchr(void) { uint32_t r = 0, p = R[4]; uint8_t c; do { c = iop_read(p, 1); if (c == (R[5] & 0xFF)) r = p; p++; } while (c); R[2] = r; }
static void f_strstr(void) { const char *h = istr(R[4]), *n = istr(R[5]); const char *f = strstr(h, n); R[2] = f ? R[4] + (uint32_t)(f - h) : 0; }
static void f_strtol(void) { const char *s = istr(R[4]); char *e; long v = strtol(s, &e, R[6]); if (R[5]) WR32(R[5], R[4] + (uint32_t)(e - s)); R[2] = (uint32_t)v; }
static void f_wmemcopy(void) { for (uint32_t i = 0; i < R[6]; i += 4) WR32(R[4] + i, RD32(R[5] + i)); R[2] = R[4]; }
static void f_wmemset(void) { for (uint32_t i = 0; i < R[6]; i += 4) WR32(R[4] + i, R[5]); R[2] = R[4]; }
/* ---- ioman (not used by the boot path; logged) */
static void f_io_open(void) { L("IOP: open(\"%s\", 0x%x) -> not supported\n", istr(R[4]), R[5]); R[2] = (uint32_t)-1; }
static void f_io_fail(void) { R[2] = (uint32_t)-1; }
/* ---- cdvdman: reads go to the same virtual-LSN view of DATA.CVM as the EE CDVD HLE */
static struct { uint32_t lsn; int on; } cdst;
static void f_sceCdRead(void) { int r = hle_cd_read_iop(R[4], R[5], R[6]); R[2] = r == 0; }
static void f_sceCdSeek(void) { R[2] = 1; }
static void f_sceCdSearchFile(void) { L("IOP: sceCdSearchFile(\"%s\") not supported\n", istr(R[5])); R[2] = 0; }
static void f_sceCdSync(void) { R[2] = 0; }
static void f_sceCdStInit(void) { R[2] = 1; }
static void f_sceCdStStart(void) { cdst.lsn = R[4]; cdst.on = 1; R[2] = 1; }
static void f_sceCdStRead(void) { uint32_t n = R[4]; int r = hle_cd_read_iop(cdst.lsn, n, R[5]); cdst.lsn += n; if (R[7]) WR32(R[7], 0); R[2] = r == 0 ? n : 0; }
static void f_sceCdStStat(void) { R[2] = cdst.on ? 0x100 : 0; }
static void f_sceCdStStop(void) { cdst.on = 0; R[2] = 1; }
/* ---- vblank */
static void f_WaitVblank(void) { R[2] = 0; block(W_VBLANK, 0); }

static const Hle hle_fns[] = {
    {"sysmem", 4, f_AllocSysMemory, "AllocSysMemory"}, {"sysmem", 5, f_FreeSysMemory, "FreeSysMemory"},
    {"sysmem", 7, f_QueryMaxFree, "QueryMaxFreeMemSize"}, {"sysmem", 8, f_QueryTotalFree, "QueryTotalFreeMemSize"},
    {"loadcore", 5, f_FlushDcache, "FlushDcache"}, {"loadcore", 6, f_RegisterLibraryEntries, "RegisterLibraryEntries"},
    {"intrman", 4, f_RegisterIntrHandler, "RegisterIntrHandler"}, {"intrman", 5, f_ReleaseIntrHandler, "ReleaseIntrHandler"},
    {"intrman", 6, f_EnableIntr, "EnableIntr"}, {"intrman", 7, f_DisableIntr, "DisableIntr"},
    {"intrman", 17, f_CpuSuspendIntr, "CpuSuspendIntr"}, {"intrman", 18, f_CpuResumeIntr, "CpuResumeIntr"}, {"intrman", 23, f_QueryIntrContext, "QueryIntrContext"},
    {"thbase", 4, f_CreateThread, "CreateThread"}, {"thbase", 5, f_DeleteThread, "DeleteThread"}, {"thbase", 6, f_StartThread, "StartThread"},
    {"thbase", 8, f_ExitThread, "ExitThread"}, {"thbase", 10, f_TerminateThread, "TerminateThread"}, {"thbase", 14, f_ChangeThreadPriority, "ChangeThreadPriority"},
    {"thbase", 20, f_GetThreadId, "GetThreadId"}, {"thbase", 22, f_ReferThreadStatus, "ReferThreadStatus"}, {"thbase", 24, f_SleepThread, "SleepThread"},
    {"thbase", 25, f_WakeupThread, "WakeupThread"}, {"thbase", 26, f_WakeupThread, "iWakeupThread"},
    {"thbase", 33, f_DelayThread, "DelayThread"}, {"thbase", 34, f_GetSystemTime, "GetSystemTime"},
    {"thbase", 39, f_USec2SysClock, "USec2SysClock"}, {"thbase", 40, f_SysClock2USec, "SysClock2USec"},
    {"thsemap", 4, f_CreateSema, "CreateSema"}, {"thsemap", 5, f_DeleteSema, "DeleteSema"}, {"thsemap", 6, f_SignalSema, "SignalSema"},
    {"thsemap", 7, f_SignalSema, "iSignalSema"}, {"thsemap", 8, f_WaitSema, "WaitSema"}, {"thsemap", 9, f_PollSema, "PollSema"},
    {"thevent", 4, f_CreateEventFlag, "CreateEventFlag"}, {"thevent", 5, f_DeleteEventFlag, "DeleteEventFlag"}, {"thevent", 6, f_SetEventFlag, "SetEventFlag"},
    {"thevent", 7, f_SetEventFlag, "iSetEventFlag"}, {"thevent", 8, f_ClearEventFlag, "ClearEventFlag"}, {"thevent", 9, f_ClearEventFlag, "iClearEventFlag"},
    {"thevent", 10, f_WaitEventFlag, "WaitEventFlag"}, {"thevent", 13, f_ReferEventFlagStatus, "ReferEventFlagStatus"},
    {"timrman", 4, f_AllocHardTimer, "AllocHardTimer"}, {"timrman", 6, f_FreeHardTimer, "FreeHardTimer"}, {"timrman", 20, f_SetTimerHandler, "SetTimerHandler"},
    {"timrman", 22, f_SetupHardTimer, "SetupHardTimer"}, {"timrman", 23, f_StartHardTimer, "StartHardTimer"}, {"timrman", 24, f_StopHardTimer, "StopHardTimer"},
    {"sifcmd", 14, f_sceSifInitRpc, "sceSifInitRpc"}, {"sifcmd", 15, f_sceSifBindRpc, "sceSifBindRpc"}, {"sifcmd", 16, f_sceSifCallRpc, "sceSifCallRpc"}, {"sifcmd", 17, f_sceSifRegisterRpc, "sceSifRegisterRpc"},
    {"sifcmd", 18, f_sceSifCheckStatRpc, "sceSifCheckStatRpc"}, {"sifcmd", 19, f_sceSifSetRpcQueue, "sceSifSetRpcQueue"}, {"sifcmd", 22, f_sceSifRpcLoop, "sceSifRpcLoop"},
    {"sifcmd", 24, f_sceSifRemoveRpc, "sceSifRemoveRpc"}, {"sifcmd", 25, f_sceSifRemoveRpc, "sceSifRemoveRpcQueue"},
    {"sifman", 5, f_sceSifInit, "sceSifInit"}, {"sifman", 7, f_sceSifSetDma, "sceSifSetDma"}, {"sifman", 8, f_sceSifDmaStat, "sceSifDmaStat"}, {"sifman", 29, f_sceSifCheckInit, "sceSifCheckInit"},
    {"stdio", 4, f_printf, "printf"},
    {"sysclib", 8, f_look_ctype_table, "look_ctype_table"}, {"sysclib", 11, f_memcmp, "memcmp"}, {"sysclib", 12, f_memcpy, "memcpy"}, {"sysclib", 13, f_memcpy, "memmove"},
    {"sysclib", 14, f_memset, "memset"}, {"sysclib", 17, f_bzero, "bzero"}, {"sysclib", 19, f_sprintf, "sprintf"}, {"sysclib", 20, f_strcat, "strcat"},
    {"sysclib", 23, f_strcpy, "strcpy"}, {"sysclib", 27, f_strlen, "strlen"}, {"sysclib", 29, f_strncmp, "strncmp"}, {"sysclib", 30, f_strncpy, "strncpy"},
    {"sysclib", 32, f_strrchr, "strrchr"}, {"sysclib", 34, f_strstr, "strstr"}, {"sysclib", 36, f_strtol, "strtol"},
    {"sysclib", 40, f_wmemcopy, "_wmemcopy"}, {"sysclib", 41, f_wmemset, "_wmemset"},
    {"ioman", 4, f_io_open, "open"}, {"ioman", 5, f_io_fail, "close"}, {"ioman", 6, f_io_fail, "read"}, {"ioman", 7, f_io_fail, "write"},
    {"ioman", 8, f_io_fail, "lseek"}, {"ioman", 10, f_io_fail, "remove"},
    {"cdvdman", 6, f_sceCdRead, "sceCdRead"}, {"cdvdman", 7, f_sceCdSeek, "sceCdSeek"}, {"cdvdman", 10, f_sceCdSearchFile, "sceCdSearchFile"},
    {"cdvdman", 11, f_sceCdSync, "sceCdSync"}, {"cdvdman", 56, f_sceCdStInit, "sceCdStInit"}, {"cdvdman", 57, f_sceCdStRead, "sceCdStRead"},
    {"cdvdman", 59, f_sceCdStStart, "sceCdStStart"}, {"cdvdman", 60, f_sceCdStStat, "sceCdStStat"}, {"cdvdman", 61, f_sceCdStStop, "sceCdStStop"},
    {"vblank", 4, f_WaitVblank, "WaitVblankStart"}, {"vblank", 5, f_WaitVblank, "WaitVblankEnd"},
};
#define NHLE ((int)(sizeof hle_fns / sizeof hle_fns[0]))
static uint64_t hle_count[256];

void iop_hle_trap(uint32_t pc) {
    uint32_t n = (pc - IOP_HLE_BASE) / 8;
    switch (n) {
    case T_CALLRET: call_done = 1; iop.yield = 1; return;
    case T_THREADEXIT: th[cur].exit_code = R[2]; th[cur].status = TS_DORMANT; iop.yield = 1; return;
    case T_RPCRET: rpc_return(); return;
    case T_UNRESOLVED: L("IOP: call to an unresolved import (ra 0x%08x)\n", R[31]); R[2] = 0; return;
    }
    int k = n - T_FIRST_FN;
    if (k < 0 || k >= NHLE) { iop_fault("IOP: bad HLE trap 0x%x", pc); return; }
    hle_count[k]++;
    static int trace = -1; if (trace < 0) trace = getenv("HG_IOPTRACE") != 0;
    uint32_t a0 = R[4], a1 = R[5], a2 = R[6], a3 = R[7], ra = R[31];
    hle_fns[k].fn();
    if (trace) L("IOP[%d] %s(0x%x, 0x%x, 0x%x, 0x%x) -> 0x%x  ra 0x%x\n", cur, hle_fns[k].name, a0, a1, a2, a3, R[2], ra);
}
void iop_syscall(uint32_t pc) { L("IOP: syscall at 0x%08x (not expected)\n", pc); }

/* ================================================================== IRX loading */
typedef struct { char name[32]; uint32_t base, size, entry, gp; } Mod;
static Mod mods[16]; static int nmods;
static int unresolved;
static uint32_t resolve(const char *lib, int idx) {
    for (int i = 0; i < nexps; i++) if (!strcmp(exps[i].name, lib)) {
        uint32_t f = RD32(exps[i].table + 20 + 4 * idx); if (f) return f;
    }
    for (int k = 0; k < NHLE; k++) if (!strcmp(hle_fns[k].lib, lib) && hle_fns[k].idx == idx) return TRAP(T_FIRST_FN + k);
    if (unresolved++ < 40) L("IOP: unresolved import %s #%d\n", lib, idx);
    return TRAP(T_UNRESOLVED);
}
static void link_imports(uint32_t base, uint32_t size) {
    for (uint32_t a = base; a + 20 <= base + size; a += 4) {
        if (RD32(a) != 0x41E00000u || RD32(a + 4) != 0) continue;
        char lib[9]; for (int i = 0; i < 8; i++) lib[i] = iop_read(a + 12 + i, 1); lib[8] = 0;
        uint32_t s = a + 20;
        while (RD32(s) == 0x03E00008u && (RD32(s + 4) >> 16) == 0x2400) {
            int idx = RD32(s + 4) & 0xFFFF; uint32_t t = resolve(lib, idx);
            WR32(s, 0x08000000u | ((t >> 2) & 0x3FFFFFF)); WR32(s + 4, 0);
            s += 8;
        }
        a = s - 4;
    }
}
static uint32_t rd32f(const uint8_t *d, size_t o) { uint32_t v; memcpy(&v, d + o, 4); return v; }
static uint16_t rd16f(const uint8_t *d, size_t o) { uint16_t v; memcpy(&v, d + o, 2); return v; }
static int load_irx(const char *path, const char *name, Mod *m) {
    FILE *f = fopen(path, "rb"); if (!f) { L("IOP: cannot open %s\n", path); return -1; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *d = malloc(n); if (fread(d, 1, n, f) != (size_t)n) { fclose(f); free(d); return -1; } fclose(f);
    if (rd32f(d, 0) != 0x464C457F || rd16f(d, 16) != 0xFF80) { L("IOP: %s is not an IRX\n", name); free(d); return -1; }
    uint32_t phoff = rd32f(d, 28), shoff = rd32f(d, 32); int phnum = rd16f(d, 44), shnum = rd16f(d, 48);
    uint32_t iopmod = 0;
    for (int i = 0; i < shnum; i++) if (rd32f(d, shoff + 40 * i + 4) == 0x70000080) iopmod = rd32f(d, shoff + 40 * i + 16);
    uint32_t off = 0, filesz = 0, memsz = 0;
    for (int i = 0; i < phnum; i++) if (rd32f(d, phoff + 32 * i) == 1) { off = rd32f(d, phoff + 32 * i + 4); filesz = rd32f(d, phoff + 32 * i + 16); memsz = rd32f(d, phoff + 32 * i + 20); }
    if (!iopmod || !memsz) { L("IOP: %s: no .iopmod or load segment\n", name); free(d); return -1; }
    uint32_t base = heap_alloc(0, memsz + 16, 0); if (!base) { free(d); return -1; }
    memcpy(iop_ram + base, d + off, filesz);
    /* relocations */
    uint32_t hi_pending[64]; int nhi = 0;
    for (int i = 0; i < shnum; i++) {
        if (rd32f(d, shoff + 40 * i + 4) != 9) continue;
        uint32_t ro = rd32f(d, shoff + 40 * i + 16), rs = rd32f(d, shoff + 40 * i + 20);
        for (uint32_t k = 0; k < rs; k += 8) {
            uint32_t o = rd32f(d, ro + k), type = rd32f(d, ro + k + 4) & 0xFF, a = base + o;
            uint32_t w = RD32(a);
            switch (type) {
            case 2: WR32(a, w + base); break;
            case 4: WR32(a, (w & 0xFC000000u) | ((((w & 0x3FFFFFF) << 2) + base) >> 2 & 0x3FFFFFF)); break;
            case 5: if (nhi < 64) hi_pending[nhi++] = a; break;
            case 6: {
                int32_t lo = (int16_t)(w & 0xFFFF);
                for (int h = 0; h < nhi; h++) { uint32_t hw = RD32(hi_pending[h]); uint32_t ahl = ((hw & 0xFFFF) << 16) + lo + base;
                    WR32(hi_pending[h], (hw & 0xFFFF0000u) | (((ahl + 0x8000) >> 16) & 0xFFFF)); }
                nhi = 0;
                WR32(a, (w & 0xFFFF0000u) | ((lo + base) & 0xFFFF)); break; }
            case 0: case 7: break;
            default: L("IOP: %s: relocation type %u not handled\n", name, type);
            }
        }
    }
    snprintf(m->name, sizeof m->name, "%s", name);
    m->base = base; m->size = memsz; m->entry = rd32f(d, iopmod + 4) + base; m->gp = rd32f(d, iopmod + 8) + base;
    { extern uint32_t iop_text_lo[16], iop_text_hi[16]; extern int iop_ntext;
      if (iop_ntext < 16) { iop_text_lo[iop_ntext] = base; iop_text_hi[iop_ntext] = base + rd32f(d, iopmod + 12); iop_ntext++; } }
    free(d);
    link_imports(base, filesz);
    return 0;
}

/* ================================================================== scheduler / time */
static void switch_to(int n) {
    if (n == cur && live) return;
    if (live && cur > 0) th_save(cur);
    if (n != cur) nswitch++;
    cur = n; th_load(n); live = 1;
}
static uint64_t next_event(void) {
    uint64_t e = ~0ull;
    for (int i = 1; i < MAXTH; i++) if (th[i].status == TS_WAIT && th[i].wait == W_DELAY && th[i].wake < e) e = th[i].wake;
    for (int i = 0; i < 6; i++) if (tmr[i].running && tmr[i].handler && tmr[i].next < e) e = tmr[i].next;
    if (spu2_adma_active()) { uint64_t t = now + 768 * 64; if (t < e) e = t; }   /* stream input drains with time */
    return e;
}
static void wake_delays(void) { for (int i = 1; i < MAXTH; i++) if (th[i].status == TS_WAIT && th[i].wait == W_DELAY && th[i].wake <= now) { make_ready(i); } }
static uint64_t spu_done;     /* SPU2 samples generated so far */
static void spu_catch_up(void) { uint64_t want = now / 768; if (want > spu_done) { spu2_run(want - spu_done); spu_done = want; } }
/* run IOP threads until time reaches target (or, with target == now, until nothing is runnable) */
static void iop_run_until(uint64_t target, long budget) {
    long spent = 0; int immediate = target <= now;
    for (;;) {
        deliver_irqs(); run_timers(); wake_delays();
        int n = pick_thread();
        if (!n) {
            if (now >= target) break;
            uint64_t e = next_event(); now = e < target ? e : target; spu_catch_up(); continue;
        }
        if (!immediate && now >= target) break;
        switch_to(n);
        long done = iop_exec(2000);
        now += (uint64_t)done * 2; spent += done; spu_catch_up();
        if (iop.yield) { iop.yield = 0; th_save(cur); live = 0; if (th[cur].status == TS_READY) th[cur].seq = seqc++; }
        if (spent > budget) { th_save(cur); live = 0; break; }
    }
}

/* ================================================================== interface for hle.c */
static int is_emulated(const char *name) {
    static const char *const set[] = { "LIBSD", "MODHSYN", "MODMIDI", "MODMSIN", "SNDDRV", "CRI_ADXI", 0 };
    for (int i = 0; set[i]; i++) if (!strcmp(name, set[i])) return 1;
    return 0;
}
static int iop_on = -1;
int iop_enabled(void) { if (iop_on < 0) iop_on = !(getenv("HG_IOP") && !strcmp(getenv("HG_IOP"), "0")); return iop_on; }
/* returns module id (>0) if the module was loaded into the IOP emulator, 0 if the caller should fake it */
int iop_load_module(const char *path, const char *args, int arg_len) {
    if (!iop_enabled()) return 0;
    const char *b = strrchr(path, '\\'); b = b ? b + 1 : path;
    char name[32]; int i = 0; for (; b[i] && b[i] != '.' && b[i] != ';' && i < 31; i++) name[i] = toupper((unsigned char)b[i]); name[i] = 0;
    if (!is_emulated(name)) return 0;
    char file[64]; snprintf(file, sizeof file, "MODULES/%s.IRX", name);
    Mod *m = &mods[nmods];
    if (load_irx(game_path(file), name, m) < 0) return -1;
    nmods++;
    /* run module_start(argc, argv) in a loader thread; argv[0] = path, then the NUL-separated LOADFILE arguments */
    uint32_t argv = heap_alloc(0, 1024, 0), strs = argv + 128, p = strs; int argc = 0;
    snprintf((char *)iop_ram + p, 64, "cdrom0:\\MODULES\\%s.IRX", name); WR32(argv, p); argc = 1; p += strlen((char *)iop_ram + p) + 1;
    for (int k = 0; k < arg_len && argc < 30; ) {
        int l = strnlen(args + k, arg_len - k); memcpy(iop_ram + p, args + k, l); iop_ram[p + l] = 0;
        WR32(argv + 4 * argc++, p); p += l + 1; k += l + 1;
    }
    WR32(argv + 4 * argc, 0);
    int t = th_alloc(); Th *T = &th[t]; memset(T, 0, sizeof *T);
    T->entry = m->entry; T->stack_size = 0x4000; T->stack = heap_alloc(1, T->stack_size, 0); T->gp = m->gp; T->prio = 8; T->status = TS_DORMANT; T->is_loader = 1;
    th_start(t, argc); T->ctx.r[5] = argv;
    for (int guard = 0; guard < 2000 && th[t].status != TS_DORMANT; guard++) iop_run_until(now + 36864, 4000000);
    int ret = th[t].exit_code & 3;
    { char a[256] = ""; for (int k = 1; k < argc; k++) { strncat(a, istr(RD32(argv + 4 * k)), 200 - strlen(a)); strcat(a, " "); }
      if (argc > 1) L("IOP: %s args: %s\n", name, a); }
    L("IOP: loaded %s at 0x%x (size 0x%x, entry 0x%x, gp 0x%x), module_start -> %d%s\n", name, m->base, m->size, m->entry, m->gp, th[t].exit_code,
      th[t].status != TS_DORMANT ? " (start did not finish)" : "");
    heap_free(T->stack); th[t].status = TS_FREE;
    return ret == 1 ? -1 : 0x200 + nmods;
}
int iop_serves_sid(uint32_t sid) {
    for (int i = 0; i < nmods; i++) {
        if (sid == 0x90000200 && !strcmp(mods[i].name, "CRI_ADXI")) return 1;
        if ((sid == 0x77777777 || sid == 0x77777778) && !strcmp(mods[i].name, "SNDDRV")) return 1;
    }
    return 0;
}
int iop_rpc_find(uint32_t sid, uint32_t *sd, uint32_t *buf, uint32_t *cbuf) {
    for (int i = 0; i < nsrv; i++) if (srv[i].sid == sid) { *sd = srv[i].sd; *buf = srv[i].buf; *cbuf = srv[i].cbuf; return 1; }
    return 0;
}
int iop_rpc_call(uint32_t sd, uint32_t fno, uint32_t size, uint32_t recv, uint32_t rsize, const uint32_t endpkt[8]) {
    int s = -1; for (int i = 0; i < nsrv; i++) if (srv[i].sd == sd) s = i;
    if (s < 0 || nrq >= 64) return 0;
    if (getenv("HG_RPCLOG")) L("IOP: RPC queued sid 0x%08x fn 0x%x size %u rsize %u  [%08x %08x %08x %08x] vb %llu\n", srv[s].sid, fno, size, rsize,
        RD32(srv[s].buf), RD32(srv[s].buf + 4), RD32(srv[s].buf + 8), RD32(srv[s].buf + 12), (unsigned long long)vblanks);
    Req *r = &rq[nrq++]; r->srv = s; r->fno = fno; r->size = size; r->recv = recv; r->rsize = rsize; memcpy(r->endpkt, endpkt, 32);
    for (int i = 1; i < MAXTH; i++) if (th[i].status == TS_WAIT && th[i].wait == W_RPC && th[i].rpc_qd == srv[s].qd) { serve_next(i); break; }
    iop_run_until(now, 2000000);
    return 1;
}
void iop_vblank(void) {
    if (!nmods) return;
    vblanks++;
    for (int i = 1; i < MAXTH; i++) if (th[i].status == TS_WAIT && th[i].wait == W_VBLANK) make_ready(i);
    iop_raise_irq(IOP_IRQ_VBLANK);
    static uint64_t vtarget; if (!vtarget) vtarget = now;
    vtarget += (uint64_t)(IOP_CLOCK / 59.94);
    if (now < vtarget) iop_run_until(vtarget, 8000000);
}
void iop_dump(void) {
    if (!nmods) return;
    L("IOP: interrupts %s, pending 0x%llx\n", intr_enabled ? "enabled" : "DISABLED", (unsigned long long)irq_pending);
    L("IOP: %d modules, %llu instructions, %llu switches, %.2f s of IOP time, %llu RPCs served, %llu timer calls\n", nmods, (unsigned long long)iop_insns,
      (unsigned long long)nswitch, now / IOP_CLOCK, (unsigned long long)n_iop_rpc, (unsigned long long)ntimer_calls);
    for (int i = 1; i < MAXTH; i++) if (th[i].status != TS_FREE)
        L("  IOP thread %d: entry 0x%x prio %d %s wait %d/%d pc 0x%x ra 0x%x\n", i, th[i].entry, th[i].prio, th[i].status == TS_READY ? "READY" : th[i].status == TS_WAIT ? "WAIT" : "DORMANT", th[i].wait, th[i].wait_id,
          (i == cur && live) ? iop.pc : th[i].ctx.pc, (i == cur && live) ? iop.r[31] : th[i].ctx.r[31]);
    for (int i = 1; i < 128; i++) if (sem[i].used) L("  IOP sema %d: count %d max %d\n", i, sem[i].count, sem[i].max);
    for (int i = 1; i < 64; i++) if (evf[i].used) L("  IOP event flag %d: bits 0x%x\n", i, evf[i].bits);
    L("  IOP HLE calls:"); for (int k = 0; k < NHLE; k++) if (hle_count[k]) L(" %s=%llu", hle_fns[k].name, (unsigned long long)hle_count[k]); L("\n");
    spu2_dump_stats();
}
