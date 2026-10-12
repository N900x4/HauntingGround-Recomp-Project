/* IOP (R3000A) interpreter and IOP-side hardware.
   The game's own sound modules (LIBSD, MODHSYN, MODMIDI, MODMSIN, SNDDRV, CRI_ADXI) run here unmodified; the IOP
   kernel they import is implemented on the host (iop_kernel.c). Import stubs are linked to "HLE trap" addresses in
   the unused ROM area: when the PC reaches one, the host function runs and execution continues at $ra.
   Memory: 2 MB RAM (shared with the SIF HLE in hle.c), 1 KB scratchpad, interrupt controller, DMA channels 4 and 7
   (SPU2 cores 0 and 1), SPU2 registers (spu2.c). Load delay slots are not modelled (compiled IOP code never relies on them). */
#include <stdio.h>
#include <string.h>
#include "ee_runtime.h"
#include "hle.h"
#include "iop.h"
#include <stdlib.h>

IopCpu iop;
static uint8_t scratch[1024];
static uint32_t i_stat, i_mask, i_ctrl;
static uint32_t dma_madr[16], dma_bcr[16], dma_chcr[16], dpcr, dicr, dpcr2, dicr2;
uint64_t iop_insns;
uint32_t iop_jtrace[32]; int iop_jtrace_n;
uint32_t iop_ptrace[64]; int iop_ptrace_n;
#define JT(from, to) do { iop_jtrace[iop_jtrace_n++ & 31] = (from); iop_jtrace[iop_jtrace_n++ & 31] = (to); } while (0)

/* ------------------------------------------------------------------ memory */
static inline uint32_t phys(uint32_t a) { return a & 0x1FFFFFFF; }
static int dma_index(uint32_t pa) {    /* returns channel 0..13 for the MADR/BCR/CHCR block, -1 otherwise */
    if (pa >= 0x1F801080 && pa < 0x1F8010F0) return (pa - 0x1F801080) >> 4;
    if (pa >= 0x1F801500 && pa < 0x1F801570) return 7 + ((pa - 0x1F801500) >> 4);
    return -1;
}
void iop_raise_irq(int irq);
static void dma_start(int ch);
static uint32_t hw_read32(uint32_t pa) {
    int ch = dma_index(pa);
    if (ch >= 0) { int r = pa & 15; return r == 0 ? dma_madr[ch] : r == 4 ? dma_bcr[ch] : dma_chcr[ch]; }
    switch (pa) {
    case 0x1F801070: return i_stat;
    case 0x1F801074: return i_mask;
    case 0x1F801078: { uint32_t v = i_ctrl; i_ctrl = 0; return v; }
    case 0x1F8010F0: return dpcr; case 0x1F8010F4: return dicr;
    case 0x1F801570: return dpcr2; case 0x1F801574: return dicr2;
    case 0x1D000000: case 0x1D000010: case 0x1D000020: case 0x1D000030: case 0x1D000040: case 0x1D000060: return 0;
    }
    iop_unmapped(pa, 0, 4, 0); return 0;
}
static void hw_write32(uint32_t pa, uint32_t v) {
    int ch = dma_index(pa);
    if (ch >= 0) {
        int r = pa & 15;
        if (r == 0) dma_madr[ch] = v; else if (r == 4) dma_bcr[ch] = v;
        else { dma_chcr[ch] = v; if (v & 0x01000000) dma_start(ch); }
        return;
    }
    switch (pa) {
    case 0x1F801070: i_stat &= v; return;
    case 0x1F801074: i_mask = v; return;
    case 0x1F801078: i_ctrl = v; return;
    case 0x1F8010F0: dpcr = v; return; case 0x1F8010F4: dicr = (dicr & 0x7F000000u & ~v) | (v & 0x00FFFFFF); return;
    case 0x1F801570: dpcr2 = v; return; case 0x1F801574: dicr2 = (dicr2 & 0x7F000000u & ~v) | (v & 0x00FFFFFF); return;
    }
    iop_unmapped(pa, v, 4, 1);
}
uint8_t *iop_ptr(uint32_t a) {
    uint32_t pa = phys(a);
    if (pa < 0x00800000) return iop_ram + (pa & 0x1FFFFF);
    if (pa >= 0x1F800000 && pa < 0x1F800400) return scratch + (pa & 0x3FF);
    return 0;
}
uint32_t iop_read(uint32_t a, int size) {
    uint8_t *p = iop_ptr(a); uint32_t v = 0;
    if (p) { memcpy(&v, p, size); return v; }
    uint32_t pa = phys(a);
    if (pa >= 0x1F900000 && pa < 0x1F900800) return spu2_read(pa & 0x7FF, size);
    if (size == 4) return hw_read32(pa);
    if (pa >= 0x1F801080 && pa < 0x1F801580) { uint32_t w = hw_read32(pa & ~3u); return (w >> ((pa & 3) * 8)) & (size == 2 ? 0xFFFF : 0xFF); }
    if (pa == 0x1F801070 || pa == 0x1F801074) return hw_read32(pa) & (size == 2 ? 0xFFFF : 0xFF);
    iop_unmapped(pa, 0, size, 0); return 0;
}
void iop_write(uint32_t a, uint32_t v, int size) {
    uint8_t *p = iop_ptr(a);
    if (p) { memcpy(p, &v, size); return; }
    uint32_t pa = phys(a);
    if (pa >= 0x1F900000 && pa < 0x1F900800) { spu2_write(pa & 0x7FF, v, size); return; }
    if (pa == 0x1FFE0130 || pa == 0x1FFE0140 || pa == 0x1FFE0144) return;          /* cache control */
    if (size == 4) { hw_write32(pa, v); return; }
    if (pa >= 0x1F801080 && pa < 0x1F801580) {                                      /* 16-bit DMA register writes (libsd: MODE/SIZE) */
        uint32_t w = hw_read32(pa & ~3u), sh = (pa & 3) * 8, m = (size == 2 ? 0xFFFFu : 0xFFu) << sh;
        int ch = dma_index(pa & ~3u), r = (pa & ~3u) & 15;
        w = (w & ~m) | ((v << sh) & m);
        if (ch >= 0 && r == 8) { dma_chcr[ch] = w; if (w & 0x01000000) dma_start(ch); } else hw_write32(pa & ~3u, w);
        return;
    }
    if (pa == 0x1F801070 || pa == 0x1F801074) { hw_write32(pa, v); return; }
    iop_unmapped(pa, v, size, 1);
}

/* ------------------------------------------------------------------ DMA to/from SPU2 (channel 4 = core 0, 7 = core 1) */
static void dma_start(int ch) {
    if (ch != 4 && ch != 7) { L("IOP: DMA channel %d started (not modelled)\n", ch); dma_chcr[ch] &= ~0x01000000u; return; }
    int core = ch == 7;
    uint32_t chcr = dma_chcr[ch], madr = dma_madr[ch] & 0x1FFFFF, bcr = dma_bcr[ch];
    uint32_t words = (bcr & 0xFFFF) * ((bcr >> 16) ? (bcr >> 16) : 1);
    int to_spu = chcr & 1;
    int done = spu2_dma(core, iop_ram + madr, words * 2, to_spu);   /* in halfwords */
    dma_madr[ch] += words * 4;
    if (done) iop_dma_done(core);
}
void iop_dma_done(int core) {
    int ch = core ? 7 : 4;
    dma_chcr[ch] &= ~0x01000000u;
    iop_raise_irq(core ? IOP_IRQ_DMA_SPU2 : IOP_IRQ_DMA_SPU);
}
int iop_dma_busy(int core) { return (dma_chcr[core ? 7 : 4] & 0x01000000u) != 0; }

/* ------------------------------------------------------------------ interpreter */
#define R iop.r
#define RS ((op >> 21) & 31)
#define RT ((op >> 16) & 31)
#define RD ((op >> 11) & 31)
#define SA ((op >> 6) & 31)
#define IMM ((int32_t)(int16_t)op)
#define UIMM (op & 0xFFFF)
uint32_t iop_text_lo[16], iop_text_hi[16]; int iop_ntext;
static inline void branch(uint32_t target) {
    iop.next_pc = target; iop.in_delay = 1;
    if (target < IOP_HLE_BASE || target >= IOP_HLE_END) {
        int ok = 0; for (int i = 0; i < iop_ntext; i++) if (target >= iop_text_lo[i] && target < iop_text_hi[i]) { ok = 1; break; }
        if (!ok && iop_ntext) iop_fault("IOP: jump to 0x%08x outside module code (from 0x%08x)", target, iop.pc - 4);
    }
}
int iop_exec(long max) {
    long n = 0;
    while (n < max && !iop.yield) {
        uint32_t pc = iop.pc; iop_ptrace[iop_ptrace_n++ & 63] = pc;
        if (pc >= IOP_HLE_BASE && pc < IOP_HLE_END) { iop.pc = R[31]; iop_hle_trap(pc); n++; continue; }
        static int chk = -1; if (chk < 0) chk = getenv("HG_IOPCHECK") != 0;
        if (chk && iop_ntext) { int ok = 0; for (int i = 0; i < iop_ntext; i++) if (pc >= iop_text_lo[i] && pc < iop_text_hi[i]) { ok = 1; break; }
            if (!ok) { iop_fault("IOP: executing outside module code at 0x%08x (previous pc 0x%08x)", pc, iop_ptrace[(iop_ptrace_n - 2) & 63]); return (int)n; } }
        uint32_t op; uint8_t *p = iop_ptr(pc);
        if (!p) { iop_fault("IOP: fetch from unmapped 0x%08x (ra 0x%08x)", pc, R[31]); return (int)n; }
        memcpy(&op, p, 4);
        uint32_t npc = pc + 4;
        if (iop.in_delay) { npc = iop.next_pc; iop.in_delay = 0; }
        iop.pc = npc;
        n++; iop_insns++;
        switch (op >> 26) {
        case 0x00:
            switch (op & 63) {
            case 0x00: R[RD] = R[RT] << SA; break;
            case 0x02: R[RD] = R[RT] >> SA; break;
            case 0x03: R[RD] = (uint32_t)((int32_t)R[RT] >> SA); break;
            case 0x04: R[RD] = R[RT] << (R[RS] & 31); break;
            case 0x06: R[RD] = R[RT] >> (R[RS] & 31); break;
            case 0x07: R[RD] = (uint32_t)((int32_t)R[RT] >> (R[RS] & 31)); break;
            case 0x08: JT(pc, R[RS]); branch(R[RS]); break;
            case 0x09: { uint32_t t = R[RS]; JT(pc, t); R[RD] = pc + 8; branch(t); break; }
            case 0x0C: iop_syscall(pc); break;
            case 0x0D: iop_fault("IOP: break at 0x%08x", pc); return (int)n;
            case 0x10: R[RD] = iop.hi; break;
            case 0x11: iop.hi = R[RS]; break;
            case 0x12: R[RD] = iop.lo; break;
            case 0x13: iop.lo = R[RS]; break;
            case 0x18: { int64_t m = (int64_t)(int32_t)R[RS] * (int32_t)R[RT]; iop.lo = (uint32_t)m; iop.hi = (uint32_t)(m >> 32); break; }
            case 0x19: { uint64_t m = (uint64_t)R[RS] * R[RT]; iop.lo = (uint32_t)m; iop.hi = (uint32_t)(m >> 32); break; }
            case 0x1A: { int32_t a = R[RS], b = R[RT];
                if (b == 0) { iop.lo = a >= 0 ? 0xFFFFFFFFu : 1; iop.hi = a; }
                else if (a == (int32_t)0x80000000 && b == -1) { iop.lo = 0x80000000u; iop.hi = 0; }
                else { iop.lo = a / b; iop.hi = a % b; } break; }
            case 0x1B: { uint32_t a = R[RS], b = R[RT]; if (b == 0) { iop.lo = 0xFFFFFFFFu; iop.hi = a; } else { iop.lo = a / b; iop.hi = a % b; } break; }
            case 0x20: case 0x21: R[RD] = R[RS] + R[RT]; break;
            case 0x22: case 0x23: R[RD] = R[RS] - R[RT]; break;
            case 0x24: R[RD] = R[RS] & R[RT]; break;
            case 0x25: R[RD] = R[RS] | R[RT]; break;
            case 0x26: R[RD] = R[RS] ^ R[RT]; break;
            case 0x27: R[RD] = ~(R[RS] | R[RT]); break;
            case 0x2A: R[RD] = (int32_t)R[RS] < (int32_t)R[RT]; break;
            case 0x2B: R[RD] = R[RS] < R[RT]; break;
            default: iop_fault("IOP: unknown SPECIAL 0x%08x at 0x%08x", op, pc); return (int)n;
            }
            break;
        case 0x01: {
            int32_t s = R[RS]; int rt = RT; int take = (rt & 1) ? s >= 0 : s < 0;
            if (rt & 0x10) R[31] = pc + 8;
            if (take) branch(pc + 4 + (IMM << 2));
            break; }
        case 0x02: branch((pc & 0xF0000000u) | ((op & 0x3FFFFFF) << 2)); break;
        case 0x03: R[31] = pc + 8; branch((pc & 0xF0000000u) | ((op & 0x3FFFFFF) << 2)); break;
        case 0x04: if (R[RS] == R[RT]) branch(pc + 4 + (IMM << 2)); break;
        case 0x05: if (R[RS] != R[RT]) branch(pc + 4 + (IMM << 2)); break;
        case 0x06: if ((int32_t)R[RS] <= 0) branch(pc + 4 + (IMM << 2)); break;
        case 0x07: if ((int32_t)R[RS] > 0) branch(pc + 4 + (IMM << 2)); break;
        case 0x08: case 0x09: R[RT] = R[RS] + IMM; break;
        case 0x0A: R[RT] = (int32_t)R[RS] < IMM; break;
        case 0x0B: R[RT] = R[RS] < (uint32_t)IMM; break;
        case 0x0C: R[RT] = R[RS] & UIMM; break;
        case 0x0D: R[RT] = R[RS] | UIMM; break;
        case 0x0E: R[RT] = R[RS] ^ UIMM; break;
        case 0x0F: R[RT] = UIMM << 16; break;
        case 0x10: {   /* COP0 */
            uint32_t f = RS;
            if (f == 0) R[RT] = iop.cop0[RD];
            else if (f == 4) iop.cop0[RD] = R[RT];
            else if (f == 16 && (op & 63) == 0x10) { uint32_t s = iop.cop0[12]; iop.cop0[12] = (s & ~0xFu) | ((s >> 2) & 0xF); }   /* RFE */
            break; }
        case 0x20: R[RT] = (uint32_t)(int32_t)(int8_t)iop_read(R[RS] + IMM, 1); break;
        case 0x21: R[RT] = (uint32_t)(int32_t)(int16_t)iop_read(R[RS] + IMM, 2); break;
        case 0x22: { uint32_t a = R[RS] + IMM, w = iop_read(a & ~3u, 4), sh = (3 - (a & 3)) * 8; R[RT] = (R[RT] & ((1u << sh) - 1)) | (w << sh); if (sh == 0) R[RT] = w; break; }   /* LWL */
        case 0x23: R[RT] = iop_read(R[RS] + IMM, 4); break;
        case 0x24: R[RT] = iop_read(R[RS] + IMM, 1); break;
        case 0x25: R[RT] = iop_read(R[RS] + IMM, 2); break;
        case 0x26: { uint32_t a = R[RS] + IMM, w = iop_read(a & ~3u, 4), sh = (a & 3) * 8; R[RT] = sh ? (R[RT] & ~(0xFFFFFFFFu >> sh)) | (w >> sh) : w; break; }   /* LWR */
        case 0x28: iop_write(R[RS] + IMM, R[RT] & 0xFF, 1); break;
        case 0x29: iop_write(R[RS] + IMM, R[RT] & 0xFFFF, 2); break;
        case 0x2A: { uint32_t a = R[RS] + IMM, w = iop_read(a & ~3u, 4), sh = (3 - (a & 3)) * 8;   /* SWL */
            w = sh ? (w & ~(0xFFFFFFFFu >> sh)) | (R[RT] >> sh) : R[RT]; iop_write(a & ~3u, w, 4); break; }
        case 0x2B: iop_write(R[RS] + IMM, R[RT], 4); break;
        case 0x2E: { uint32_t a = R[RS] + IMM, w = iop_read(a & ~3u, 4), sh = (a & 3) * 8;          /* SWR */
            w = sh ? (w & ((1u << sh) - 1)) | (R[RT] << sh) : R[RT]; iop_write(a & ~3u, w, 4); break; }
        default: iop_fault("IOP: unknown opcode 0x%08x at 0x%08x", op, pc); return (int)n;
        }
        R[0] = 0;
    }
    return (int)n;
}
