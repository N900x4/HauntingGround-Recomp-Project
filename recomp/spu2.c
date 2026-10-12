/* SPU2 (stage 1: registers, sound RAM, DMA). Voices, envelopes, reverb and output come next.
   Register layout: ps2sdk common/include/spu2_mmio_hwport.h (core 0 at 0x000, core 1 at 0x400, 0x760+ per-core volumes). */
#include <stdio.h>
#include <string.h>
#include "ee_runtime.h"
#include "hle.h"
#include "iop.h"

static uint16_t reg[0x400];               /* 0x800 bytes of registers as halfwords */
uint16_t spu_ram[1 << 20];                /* 2 MB sound RAM, addressed in halfwords */
static uint32_t tsa_cur[2];               /* transfer address (halfwords) */
static uint64_t n_dma_hw[2], n_samples;

#define CORE(c, off) ((((c) << 10) + (off)) >> 1)
static uint32_t pair(int c, int off) { return ((uint32_t)reg[CORE(c, off)] << 16) | reg[CORE(c, off + 2)]; }

uint32_t spu2_read(uint32_t off, int size) {
    uint32_t i = (off & 0x7FF) >> 1;
    int core = off >= 0x400 && off < 0x760 ? 1 : 0, r = off & 0x3FF;
    if (off < 0x760 && r == 0x344) return 0x80;                        /* STATX: ready, no transfer in progress */
    if (off < 0x760 && r == 0x1AC) {                                   /* manual read from sound RAM */
        uint16_t v = spu_ram[tsa_cur[core] & 0xFFFFF]; tsa_cur[core]++; return v; }
    uint32_t v = reg[i];
    if (size == 4) v |= (uint32_t)reg[(i + 1) & 0x3FF] << 16;
    return v;
}
void spu2_write(uint32_t off, uint32_t v, int size) {
    uint32_t i = (off & 0x7FF) >> 1;
    int core = off >= 0x400 && off < 0x760 ? 1 : 0, r = off & 0x3FF;
    if (size == 4) { spu2_write(off, v & 0xFFFF, 2); spu2_write(off + 2, v >> 16, 2); return; }
    reg[i] = (uint16_t)v;
    if (off < 0x760) {
        if (r == 0x1A8 || r == 0x1AA) tsa_cur[core] = pair(core, 0x1A8) & 0xFFFFF;   /* TSA */
        else if (r == 0x1AC) { spu_ram[tsa_cur[core] & 0xFFFFF] = (uint16_t)v; tsa_cur[core]++; }   /* manual write */
    }
}
/* auto-DMA (ADMA) input stream per core: blocks of 256 L samples then 256 R samples, drained at 48 kHz.
   The DMA completes once only the SPU2's own two-block buffer is still queued. */
#define ADMA_CAP (1 << 18)
typedef struct { int16_t buf[ADMA_CAP]; uint32_t rd, wr; int pending; uint32_t cur_l[256], cur_r[256]; int16_t outl[256], outr[256]; int pos, have; } Adma;
static Adma adma[2];
static uint32_t adma_queued(int c) { return adma[c].wr - adma[c].rd; }
static void adma_check_done(int c) { if (adma[c].pending && adma_queued(c) <= 1024) { adma[c].pending = 0; iop_dma_done(c); } }
int spu2_adma_active(void) { return adma[0].pending || adma[1].pending || adma_queued(0) || adma_queued(1); }
int16_t spu2_adma_sample(int c, int right) { return adma[c].have ? (right ? adma[c].outr[adma[c].pos] : adma[c].outl[adma[c].pos]) : 0; }
static void adma_step(int c) {          /* advance one 48 kHz sample */
    Adma *a = &adma[c];
    if (a->have && ++a->pos < 256) return;
    a->have = 0; a->pos = 0;
    if (adma_queued(c) >= 512) {
        for (int i = 0; i < 256; i++) { a->outl[i] = a->buf[(a->rd + i) & (ADMA_CAP - 1)]; a->outr[i] = a->buf[(a->rd + 256 + i) & (ADMA_CAP - 1)]; }
        a->rd += 512; a->have = 1;
    }
    adma_check_done(c);
}
int spu2_dma(int core, uint8_t *mem, uint32_t halfwords, int to_spu) {
    uint32_t a = tsa_cur[core];
    uint16_t admas = reg[CORE(core, 0x1B0)];
    if (to_spu && (admas & (1 << core))) {
        Adma *s = &adma[core];
        for (uint32_t k = 0; k < halfwords; k++) { int16_t v; memcpy(&v, mem + 2 * k, 2); s->buf[(s->wr + k) & (ADMA_CAP - 1)] = v; }
        s->wr += halfwords; n_dma_hw[core] += halfwords; s->pending = 1;
        adma_check_done(core);
        return 0;
    }
    for (uint32_t k = 0; k < halfwords; k++) {
        uint16_t *p = (uint16_t *)mem + k;
        if (to_spu) spu_ram[(a + k) & 0xFFFFF] = *p; else *p = spu_ram[(a + k) & 0xFFFFF];
    }
    tsa_cur[core] = (a + halfwords) & 0xFFFFF;
    n_dma_hw[core] += halfwords;
    return 1;
}
void spu2_run(uint64_t samples) {
    for (uint64_t s = 0; s < samples; s++) { adma_step(0); adma_step(1); }
    n_samples += samples;
}
void spu2_dump_stats(void) {
    L("SPU2: %llu samples, DMA core0 %llu / core1 %llu halfwords\n", (unsigned long long)n_samples, (unsigned long long)n_dma_hw[0], (unsigned long long)n_dma_hw[1]);
}
