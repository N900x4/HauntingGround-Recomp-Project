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
#include "vu1_state.h"
#define VU1_DEFINE
#include "vu1_core.h"


uint8_t vu1_mem[16384];
uint8_t vu1_code[16384];
uint32_t vu1_tpc;
uint32_t vu1_top, vu1_itop;
uint32_t vu1_code_gen = 1;

static Dec dec[2048];
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

void vu1_kick(uint32_t addr) {
    n_kicks++;
    for (int guard = 0; guard < 1024; guard++) {
        uint64_t q[2]; memcpy(q, vu1_mem + ((addr & 0x3FF) << 4), 16); addr++;
        if (kick_check) { kick_hash = (kick_hash ^ q[0]) * 0x100000001B3ull; kick_hash = (kick_hash ^ q[1]) * 0x100000001B3ull; }
        gif_qword(1, q);
        if (gif_path_done(1)) break;
    }
}
/* Ahead-of-time compiled programs (gen/vu1_aot_*.c, made by vu1gen.py from the game's 19 VU1 programs). After micro
   memory changes, the program it holds is identified by comparing code; runs then use the compiled version, which
   hands over to the interpreter for anything outside it. Used with the default timing model only (HG_VUSTALL=1,
   HG_VUFLAGDELAY=0); HG_VU1AOT=0 turns it off. */
extern const AotProg aot_progs[]; extern const int aot_nprogs;   /* gen/vu1_aot_tab.c */
static int aot_on, aot_cur = -1; static uint32_t aot_gen; static uint64_t n_aot_runs, n_aot_handover;
static int aot_find(void) {
    int best = -1;
    for (int p = 0; p < aot_nprogs; p++)
        if (!memcmp(vu1_code, aot_progs[p].code, aot_progs[p].n * 8) && (best < 0 || aot_progs[p].n > aot_progs[best].n)) best = p;
    return best;
}

static void vu1_run_inner(uint32_t start) {
    n_runs++;
    RunSt s = { .pc = start & 2047, .start = start, .prev_vi = -1 };
    vf[0].f[0] = vf[0].f[1] = vf[0].f[2] = 0; vf[0].f[3] = 1.0f; vi[0] = 0;
    if (vu_stall) { memset(vf_rdy, 0, sizeof vf_rdy); acc_rdy = 0; }   /* the pipeline has drained between program runs */
    if (aot_on) {
        if (vu1_code_gen != aot_gen) { aot_gen = vu1_code_gen; aot_cur = aot_find(); }
        if (aot_cur >= 0) { n_aot_runs++; if (aot_progs[aot_cur].run(&s)) return; n_aot_handover++; }
    }
    for (; s.n < VU1_MAXN; s.n++) if (step(decoded(s.pc), &s, s.pc, vu_stall)) return;
    L("VU1: program at %u did not end\n", start);
    vu1_tpc = s.pc;
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
        aot_on = aot_nprogs > 0 && vu_stall == 1 && flag_delay == 0 && !(getenv("HG_VU1AOT") && !strcmp(getenv("HG_VU1AOT"), "0"));
        L("VU1: %d ahead-of-time compiled programs, %s\n", aot_nprogs, aot_on ? "in use" : "not used");
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
    L("VU1 flag reads:"); for (int i = 0; i < 64; i++) if (i < (int)(sizeof nm / sizeof nm[0]) && nm[i] && lk_count[i]) L(" %s %llu", nm[i], (unsigned long long)lk_count[i]); L("\n"); L("VU1: %llu runs, %llu instructions, %llu XGKICKs\n", (unsigned long long)n_runs, (unsigned long long)n_insns, (unsigned long long)n_kicks);
    L("VU1 AOT: %llu runs compiled, %llu handed over to the interpreter\n", (unsigned long long)n_aot_runs, (unsigned long long)n_aot_handover); }
