/* SPU2: two SPU cores of 24 voices each at 48 kHz, 2 MB sound RAM (halfword addressed).
   Hardware behaviour follows the psx-spx SPU documentation (ADPCM, pitch counter, 4-point Gaussian
   interpolation, ADSR/sweep envelopes, noise, reverb formula and its 39-tap resampling filter), applied
   per core with the SPU2 register layout from ps2sdk (spu2_mmio_hwport.h). PS2 specifics: per-core mixer
   (MMIX routes voices, core 0 output and the auto-DMA stream into dry and wet paths), 32-bit reverb
   addresses (ESA..EEA), IRQ address checks on voice block reads and DMA. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "ee_runtime.h"
#include "hle.h"
#include "iop.h"

static const int16_t gauss[512] = {
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 3, 3,
    3, 4, 4, 5, 5, 6, 7, 7, 8, 9, 9, 10, 11, 12, 13, 14,
    15, 16, 17, 18, 19, 21, 22, 24, 25, 27, 28, 30, 32, 33, 35, 37,
    39, 41, 44, 46, 48, 51, 53, 56, 58, 61, 64, 67, 70, 73, 77, 80,
    84, 87, 91, 95, 99, 103, 107, 111, 116, 120, 125, 130, 135, 140, 145, 150,
    156, 161, 167, 173, 179, 186, 192, 199, 205, 212, 219, 227, 234, 242, 250, 257,
    266, 274, 283, 291, 300, 309, 319, 328, 338, 348, 358, 369, 379, 390, 401, 412,
    424, 436, 448, 460, 473, 485, 498, 512, 525, 539, 553, 567, 582, 597, 612, 627,
    643, 659, 675, 692, 708, 726, 743, 761, 779, 797, 816, 835, 854, 874, 894, 914,
    935, 956, 977, 999, 1020, 1043, 1066, 1089, 1112, 1136, 1160, 1184, 1209, 1234, 1260, 1286,
    1312, 1339, 1366, 1394, 1422, 1450, 1479, 1508, 1537, 1567, 1598, 1628, 1660, 1691, 1723, 1756,
    1789, 1822, 1856, 1890, 1924, 1959, 1995, 2031, 2067, 2104, 2141, 2179, 2217, 2256, 2295, 2334,
    2374, 2415, 2456, 2497, 2539, 2582, 2624, 2668, 2712, 2756, 2801, 2846, 2892, 2938, 2985, 3032,
    3079, 3128, 3176, 3225, 3275, 3325, 3376, 3427, 3479, 3531, 3584, 3637, 3691, 3745, 3799, 3855,
    3910, 3967, 4023, 4081, 4138, 4197, 4255, 4315, 4374, 4435, 4495, 4557, 4619, 4681, 4744, 4807,
    4871, 4935, 5000, 5065, 5131, 5197, 5264, 5332, 5399, 5468, 5536, 5606, 5676, 5746, 5817, 5888,
    5959, 6032, 6104, 6177, 6251, 6325, 6400, 6475, 6550, 6626, 6702, 6779, 6856, 6934, 7012, 7091,
    7170, 7249, 7329, 7409, 7490, 7571, 7653, 7735, 7817, 7900, 7983, 8066, 8150, 8234, 8319, 8404,
    8489, 8575, 8661, 8748, 8834, 8922, 9009, 9097, 9185, 9273, 9362, 9451, 9541, 9630, 9720, 9811,
    9901, 9992, 10083, 10174, 10266, 10358, 10450, 10542, 10635, 10727, 10820, 10913, 11007, 11100, 11194, 11288,
    11382, 11476, 11571, 11665, 11760, 11855, 11950, 12045, 12140, 12236, 12331, 12427, 12522, 12618, 12714, 12809,
    12905, 13001, 13097, 13193, 13289, 13385, 13481, 13577, 13673, 13769, 13865, 13961, 14056, 14152, 14248, 14343,
    14439, 14534, 14630, 14725, 14820, 14915, 15010, 15104, 15199, 15293, 15387, 15481, 15575, 15669, 15762, 15855,
    15948, 16041, 16133, 16226, 16317, 16409, 16500, 16592, 16682, 16773, 16863, 16953, 17042, 17131, 17220, 17308,
    17396, 17484, 17571, 17658, 17744, 17830, 17916, 18001, 18086, 18170, 18254, 18337, 18420, 18502, 18584, 18665,
    18746, 18826, 18905, 18985, 19063, 19141, 19219, 19295, 19372, 19447, 19522, 19597, 19671, 19744, 19816, 19888,
    19959, 20030, 20100, 20169, 20238, 20306, 20373, 20439, 20505, 20570, 20634, 20698, 20760, 20822, 20884, 20944,
    21004, 21063, 21121, 21178, 21235, 21290, 21345, 21399, 21452, 21505, 21556, 21607, 21657, 21706, 21754, 21801,
    21848, 21893, 21938, 21982, 22025, 22066, 22107, 22148, 22187, 22225, 22262, 22299, 22334, 22369, 22402, 22435,
    22467, 22498, 22527, 22556, 22584, 22611, 22637, 22662, 22686, 22709, 22731, 22752, 22772, 22791, 22809, 22826,
    22842, 22857, 22872, 22885, 22897, 22908, 22918, 22927, 22935, 22942, 22948, 22953, 22957, 22960, 22962, 22963,
};

static const int16_t rev_fir[39] = {-1, 0, 2, 0, -10, 0, 35, 0, -103, 0, 266, 0, -616, 0, 1332, 0, -2960, 0, 10246, 16384, 10246, 0, -2960, 0, 1332, 0, -616, 0, 266, 0, -103, 0, 35, 0, -10, 0, 2, 0, -1};


static uint16_t reg[0x400];               /* 0x800 bytes of registers as halfwords */
uint16_t spu_ram[1 << 20];                /* 2 MB sound RAM, addressed in halfwords */
#define RAMMASK 0xFFFFF
static uint32_t tsa_cur[2];
static uint64_t n_dma_hw[2], n_samples, n_keyon, n_irq;

#define CORE(c, off) ((((c) << 10) + (off)) >> 1)
#define VREG(c, v, off) CORE(c, ((v) << 4) + (off))
#define VADDR(c, v, off) CORE(c, 0x1C0 + (v) * 12 + (off))
static uint32_t pair(int c, int off) { return (((uint32_t)reg[CORE(c, off)] << 16) | reg[CORE(c, off + 2)]) & RAMMASK; }
static uint32_t mask24(int c, int off) { return reg[CORE(c, off)] | ((uint32_t)reg[CORE(c, off + 2)] << 16); }
static void set_mask24(int c, int off, uint32_t m) { reg[CORE(c, off)] = m & 0xFFFF; reg[CORE(c, off + 2)] = (m >> 16) & 0xFF; }
static int16_t sat16(int32_t x) { return x > 32767 ? 32767 : x < -32768 ? -32768 : x; }
static int32_t preg(int c, int off) { return (int16_t)reg[(0x760 + c * 0x28 + off) >> 1]; }   /* per-core 0x760 block */

/* ------------------------------------------------------------------ envelopes */
typedef struct { int32_t level; uint32_t counter; } Env;
/* one 48 kHz step of the envelope algorithm. shift 0..31 and step 0..3 are the register fields: the level moves by
   (+7..+4 or -8..-5) << max(0, 11 - shift) once every 1 << max(0, shift - 11) samples. An exponential increase above
   0x6000 runs 4x slower (shift + 2); an exponential decrease scales the step by the level. Decay and release have
   a fixed step of -8 (step field 0). The counter counts samples since the last change. */
static void env_step(Env *e, int shift, int stepv, int exponential, int decreasing, int negative) {
    if (stepv == 3 && shift == 31) return;                   /* all-ones rate: never steps */
    int32_t step = 7 - stepv;
    if (decreasing ^ negative) step = ~step;
    int s = shift;
    if (exponential && !decreasing && e->level > 0x6000) s += 2;
    step = step << (11 - s > 0 ? 11 - s : 0);
    if (exponential && decreasing) step = (int32_t)(((int64_t)step * e->level) >> 15);   /* arithmetic shift: floors to -1, so release reaches 0 */
    if (++e->counter < (1u << (s - 11 > 0 ? s - 11 : 0))) return;
    e->counter = 0;
    int32_t l = e->level + step;
    if (!decreasing) l = l > 0x7FFF ? 0x7FFF : l < -0x8000 ? -0x8000 : l;
    else if (negative) l = l < -0x8000 ? -0x8000 : l > 0 ? 0 : l;
    else l = l < 0 ? 0 : l;
    e->level = l;
}
typedef struct { Env e; int32_t cur; } Vol;    /* VOLL/VOLR in direct or sweep mode */
static int32_t vol_step(Vol *v, uint16_t r) {
    if (!(r & 0x8000)) { v->cur = (int16_t)(r << 1); v->e.level = v->cur; return v->cur; }
    int exp = (r >> 14) & 1, dec = (r >> 13) & 1, neg = (r >> 12) & 1;
    v->e.level = v->cur;
    env_step(&v->e, (r >> 2) & 31, r & 3, exp, dec, neg);
    v->cur = v->e.level; return v->cur;
}

/* ------------------------------------------------------------------ voices */
enum { PH_OFF = 0, PH_ATTACK, PH_DECAY, PH_SUSTAIN, PH_RELEASE };
typedef struct {
    int phase; Env adsr;
    uint32_t addr, loop;        /* current block address, loop address (halfwords) */
    uint32_t counter;           /* pitch counter: bit 12+ = sample index within the 28-sample block */
    int16_t buf[28]; int16_t hist[4]; int32_t p1, p2; int decoded;
    int sample_idx;             /* next sample of buf to push into hist */
    Vol vl, vr; int32_t out;    /* last output (for PMON) */
    int keyon_delay;
} Voice;
static Voice voice[2][24];
static uint32_t endx[2];
static uint32_t irq_pending_info;
static int irq_armed[2];
static void check_irq(int c, uint32_t a, uint32_t n) {
    for (int k = 0; k < 2; k++) {
        if (!(reg[CORE(k, 0x19A)] & 0x40) || !irq_armed[k]) continue;
        uint32_t ia = pair(k, 0x19C);
        if (ia >= a && ia < a + n) { reg[0x7C2 >> 1] |= 4 << k; irq_armed[k] = 0; n_irq++; iop_raise_irq(IOP_IRQ_SPU); }
    }
    (void)c;
}
static const int f0[5] = { 0, 60, 115, 98, 122 }, f1[5] = { 0, 0, -52, -55, -60 };
static void decode_block(int c, Voice *v) {
    check_irq(c, v->addr, 8);
    uint16_t hdr = spu_ram[v->addr & RAMMASK];
    int shift = hdr & 15, filt = (hdr >> 4) & 7, flags = hdr >> 8;
    if (shift > 12) shift = 9;
    if (filt > 4) filt = 4;
    if (flags & 4) v->loop = v->addr;                               /* loop start */
    for (int i = 0; i < 28; i++) {
        uint16_t w = spu_ram[(v->addr + 1 + i / 4) & RAMMASK];
        int32_t nib = (int16_t)(((w >> ((i & 3) * 4)) & 15) << 12) >> shift;
        int32_t s = nib + ((v->p1 * f0[filt] + v->p2 * f1[filt] + 32) >> 6);
        s = sat16(s); v->p2 = v->p1; v->p1 = s; v->buf[i] = s;
    }
    v->decoded = flags;                                             /* flags applied when the block finishes */
}
static void block_done(int c, int n, Voice *v) {
    int flags = v->decoded;
    if (flags & 1) {                                                /* loop end */
        endx[c] |= 1u << n;
        v->addr = v->loop;
        if (!(flags & 2)) { v->phase = PH_RELEASE; v->adsr.level = 0; }   /* end + mute */
    } else v->addr = (v->addr + 8) & RAMMASK;
}
static void key_on(int c, int n) {
    Voice *v = &voice[c][n];
    v->addr = pair(c, 0x1C0 + n * 12); v->loop = v->addr;
    v->counter = 0; v->p1 = v->p2 = 0; memset(v->hist, 0, sizeof v->hist); v->sample_idx = 0;
    v->phase = PH_ATTACK; v->adsr.level = 0; v->adsr.counter = 0;
    endx[c] &= ~(1u << n);
    decode_block(c, v);
    n_keyon++;
    static FILE *kl; static int kinit;
    if (!kinit) { kinit = 1; const char *p = getenv("HG_KONLOG"); if (p) kl = fopen(p, "w"); }
    if (kl) fprintf(kl, "%llu %d %d ssa=%05x pitch=%04x adsr=%04x/%04x vol=%04x/%04x vmix=%d%d%d%d\n", (unsigned long long)n_samples, c, n, v->addr,
                    reg[VREG(c, n, 4)], reg[VREG(c, n, 6)], reg[VREG(c, n, 8)], reg[VREG(c, n, 0)], reg[VREG(c, n, 2)],
                    (mask24(c, 0x188) >> n) & 1, (mask24(c, 0x190) >> n) & 1, (mask24(c, 0x18C) >> n) & 1, (mask24(c, 0x194) >> n) & 1);
}
static void key_off(int c, int n) { Voice *v = &voice[c][n]; if (v->phase != PH_OFF) { v->phase = PH_RELEASE; v->adsr.counter = 0; } }
static void adsr_tick(int c, int n, Voice *v) {
    uint16_t a1 = reg[VREG(c, n, 6)], a2 = reg[VREG(c, n, 8)];
    switch (v->phase) {
    case PH_ATTACK:
        env_step(&v->adsr, (a1 >> 10) & 31, (a1 >> 8) & 3, a1 >> 15, 0, 0);
        if (v->adsr.level >= 0x7FFF) { v->phase = PH_DECAY; v->adsr.counter = 0; }
        break;
    case PH_DECAY: {
        int32_t sl = ((a1 & 15) + 1) * 0x800;
        env_step(&v->adsr, (a1 >> 4) & 15, 0, 1, 1, 0);                 /* decay: shift 0..15, step -8, exponential */
        if (v->adsr.level <= sl) { v->phase = PH_SUSTAIN; v->adsr.counter = 0; }
        break; }
    case PH_SUSTAIN:
        env_step(&v->adsr, (a2 >> 8) & 31, (a2 >> 6) & 3, a2 >> 15, (a2 >> 14) & 1, 0);
        break;
    case PH_RELEASE:
        env_step(&v->adsr, a2 & 31, 0, (a2 >> 5) & 1, 1, 0);           /* release: shift 0..31, step -8 */
        if (v->adsr.level <= 0) { v->adsr.level = 0; v->phase = PH_OFF; }
        break;
    }
    reg[VREG(c, n, 0xA)] = (uint16_t)v->adsr.level;
}
static uint32_t noise_level = 1; static int32_t noise_timer;
static void noise_tick(void) {
    uint16_t attr = reg[CORE(0, 0x19A)];
    int shift = (attr >> 10) & 15, step = ((attr >> 8) & 3) + 4;
    noise_timer -= step;
    int parity = ((noise_level >> 15) ^ (noise_level >> 12) ^ (noise_level >> 11) ^ (noise_level >> 10) ^ 1) & 1;
    if (noise_timer < 0) { noise_level = (noise_level << 1) | parity; noise_timer += 0x20000 >> shift; if (noise_timer < 0) noise_timer += 0x20000 >> shift; }
}
/* one output sample of voice n; returns the enveloped mono sample */
static int32_t voice_sample(int c, int n, Voice *v, int32_t prev_out, uint32_t pmon, uint32_t non) {
    if (v->phase == PH_OFF) { v->out = 0; reg[VREG(c, n, 0xA)] = 0; return 0; }
    /* interpolate */
    int i = (v->counter >> 4) & 0xFF;
    int32_t s = ((gauss[0xFF - i] * v->hist[0]) >> 15) + ((gauss[0x1FF - i] * v->hist[1]) >> 15)
              + ((gauss[0x100 + i] * v->hist[2]) >> 15) + ((gauss[i] * v->hist[3]) >> 15);
    if (non & (1u << n)) s = (int16_t)noise_level;
    int32_t outv = (int32_t)((int64_t)s * v->adsr.level >> 15);
    adsr_tick(c, n, v);
    /* advance pitch */
    uint32_t step = reg[VREG(c, n, 4)];
    if ((pmon & (1u << n)) && n > 0) { int32_t f = prev_out + 0x8000; int32_t st = (int16_t)step; st = (st * f) >> 15; step = (uint32_t)st & 0xFFFF; }
    if (step > 0x3FFF) step = 0x4000;
    v->counter += step;
    while (v->counter >= 0x1000) {
        v->counter -= 0x1000;
        v->hist[0] = v->hist[1]; v->hist[1] = v->hist[2]; v->hist[2] = v->hist[3]; v->hist[3] = v->buf[v->sample_idx];
        if (++v->sample_idx == 28) { v->sample_idx = 0; block_done(c, n, v); decode_block(c, v); }
    }
    v->out = outv;
    return outv;
}

/* ------------------------------------------------------------------ reverb (per core, 24 kHz) */
typedef struct { int16_t inl[64], inr[64]; int pos; uint32_t cur; int32_t outl, outr, pl, pr; int16_t hl[64], hr[64]; } Rev;
static Rev rev[2];
/* EEA holds only the upper address bits (the low 16 are all ones). Drivers write it as the byte address >> 17 and
   may pass 'the top of RAM' as 0x7FFFFFFF (SNDDRV does: 0x3FFF); the SPU2 keeps only the bits its 2 MB decode, as
   for every other address register here. Unmasked, the work area ran past the end of sound RAM and the reverb
   writes wrapped round all of it, overwriting the sample banks and the stream input area. */
static uint32_t rv_end(int c) { return (((uint32_t)reg[CORE(c, 0x33C)] << 16) | 0xFFFF) & RAMMASK; }
static uint32_t rv_addr(int c, int off) { return pair(c, 0x2E4 + off); }   /* reverb address pair at 0x2E4 + off */
static int16_t rd_rev(int c, uint32_t a) {
    uint32_t esa = pair(c, 0x2E0), eea = rv_end(c), size = eea - esa + 1;
    if (eea <= esa) return 0;
    uint32_t x = esa + ((rev[c].cur - esa + a) % size);
    return (int16_t)spu_ram[x & RAMMASK];
}
static void wr_rev(int c, uint32_t a, int32_t v, int enabled) {
    if (!enabled) return;
    uint32_t esa = pair(c, 0x2E0), eea = rv_end(c), size = eea - esa + 1;
    if (eea <= esa) return;
    uint32_t x = esa + ((rev[c].cur - esa + a) % size);
    spu_ram[x & RAMMASK] = (uint16_t)sat16(v);
}
static int32_t mulv(int32_t a, int32_t v) { return (int32_t)(((int64_t)a * v) >> 15); }
static void reverb_core(int c, int32_t lin, int32_t rin, int32_t *lout, int32_t *rout) {
    int en = (reg[CORE(c, 0x19A)] >> 7) & 1;
    int32_t vIIR = preg(c, 0x14), vC1 = preg(c, 0x16), vC2 = preg(c, 0x18), vC3 = preg(c, 0x1A), vC4 = preg(c, 0x1C),
            vWALL = preg(c, 0x1E), vAPF1 = preg(c, 0x20), vAPF2 = preg(c, 0x22), vLIN = preg(c, 0x24), vRIN = preg(c, 0x26);
    uint32_t dAPF1 = rv_addr(c, 0x00), dAPF2 = rv_addr(c, 0x04), mLSAME = rv_addr(c, 0x08), mRSAME = rv_addr(c, 0x0C),
             mLCOMB1 = rv_addr(c, 0x10), mRCOMB1 = rv_addr(c, 0x14), mLCOMB2 = rv_addr(c, 0x18), mRCOMB2 = rv_addr(c, 0x1C),
             dLSAME = rv_addr(c, 0x20), dRSAME = rv_addr(c, 0x24), mLDIFF = rv_addr(c, 0x28), mRDIFF = rv_addr(c, 0x2C),
             mLCOMB3 = rv_addr(c, 0x30), mRCOMB3 = rv_addr(c, 0x34), mLCOMB4 = rv_addr(c, 0x38), mRCOMB4 = rv_addr(c, 0x3C),
             dLDIFF = rv_addr(c, 0x40), dRDIFF = rv_addr(c, 0x44), mLAPF1 = rv_addr(c, 0x48), mRAPF1 = rv_addr(c, 0x4C),
             mLAPF2 = rv_addr(c, 0x50), mRAPF2 = rv_addr(c, 0x54);
    int32_t L = mulv(lin, vLIN), Rr = mulv(rin, vRIN);
    if (en) {
        wr_rev(c, mLSAME, mulv(L + mulv(rd_rev(c, dLSAME), vWALL) - rd_rev(c, mLSAME - 1), vIIR) + rd_rev(c, mLSAME - 1), en);
        wr_rev(c, mRSAME, mulv(Rr + mulv(rd_rev(c, dRSAME), vWALL) - rd_rev(c, mRSAME - 1), vIIR) + rd_rev(c, mRSAME - 1), en);
        wr_rev(c, mLDIFF, mulv(L + mulv(rd_rev(c, dRDIFF), vWALL) - rd_rev(c, mLDIFF - 1), vIIR) + rd_rev(c, mLDIFF - 1), en);
        wr_rev(c, mRDIFF, mulv(Rr + mulv(rd_rev(c, dLDIFF), vWALL) - rd_rev(c, mRDIFF - 1), vIIR) + rd_rev(c, mRDIFF - 1), en);
    }
    int32_t Lo = mulv(vC1, rd_rev(c, mLCOMB1)) + mulv(vC2, rd_rev(c, mLCOMB2)) + mulv(vC3, rd_rev(c, mLCOMB3)) + mulv(vC4, rd_rev(c, mLCOMB4));
    int32_t Ro = mulv(vC1, rd_rev(c, mRCOMB1)) + mulv(vC2, rd_rev(c, mRCOMB2)) + mulv(vC3, rd_rev(c, mRCOMB3)) + mulv(vC4, rd_rev(c, mRCOMB4));
    Lo = Lo - mulv(vAPF1, rd_rev(c, mLAPF1 - dAPF1)); wr_rev(c, mLAPF1, Lo, en); Lo = mulv(Lo, vAPF1) + rd_rev(c, mLAPF1 - dAPF1);
    Ro = Ro - mulv(vAPF1, rd_rev(c, mRAPF1 - dAPF1)); wr_rev(c, mRAPF1, Ro, en); Ro = mulv(Ro, vAPF1) + rd_rev(c, mRAPF1 - dAPF1);
    Lo = Lo - mulv(vAPF2, rd_rev(c, mLAPF2 - dAPF2)); wr_rev(c, mLAPF2, Lo, en); Lo = mulv(Lo, vAPF2) + rd_rev(c, mLAPF2 - dAPF2);
    Ro = Ro - mulv(vAPF2, rd_rev(c, mRAPF2 - dAPF2)); wr_rev(c, mRAPF2, Ro, en); Ro = mulv(Ro, vAPF2) + rd_rev(c, mRAPF2 - dAPF2);
    *lout = sat16(Lo); *rout = sat16(Ro);
    uint32_t esa = pair(c, 0x2E0), eea = rv_end(c);
    if (eea > esa) { rev[c].cur++; if (rev[c].cur > eea || rev[c].cur < esa) rev[c].cur = esa; }
}
/* 48 kHz in/out around the 24 kHz reverb using the 39-tap filter */
static void reverb_48k(int c, int32_t inl, int32_t inr, int32_t *outl, int32_t *outr, int odd) {
    Rev *r = &rev[c];
    r->inl[r->pos & 63] = sat16(inl); r->inr[r->pos & 63] = sat16(inr);
    if (odd) {
        int32_t dl = 0, dr = 0;
        for (int k = 0; k < 39; k++) { dl += r->inl[(r->pos - k) & 63] * rev_fir[k]; dr += r->inr[(r->pos - k) & 63] * rev_fir[k]; }
        int32_t lo, ro; reverb_core(c, dl >> 15, dr >> 15, &lo, &ro);
        r->hl[r->pos & 63] = lo; r->hr[r->pos & 63] = ro;
    } else { r->hl[r->pos & 63] = 0; r->hr[r->pos & 63] = 0; }
    int32_t ol = 0, orr = 0;
    for (int k = 0; k < 39; k++) { ol += r->hl[(r->pos - k) & 63] * rev_fir[k]; orr += r->hr[(r->pos - k) & 63] * rev_fir[k]; }
    *outl = sat16(ol >> 14); *outr = sat16(orr >> 14);
    r->pos++;
}

/* auto-DMA (ADMA) input stream per core: blocks of 256 L samples then 256 R samples, drained at 48 kHz.
   The DMA completes once only the SPU2's own two-block buffer is still queued. */
#define ADMA_CAP (1 << 18)
typedef struct { int16_t buf[ADMA_CAP]; uint32_t rd, wr; int pending; uint32_t cur_l[256], cur_r[256]; int16_t outl[256], outr[256]; int pos, have; } Adma;
static Adma adma[2];
static uint32_t in_pos[2];
static int16_t in_l[2], in_r[2];
static uint32_t adma_queued(int c) { return adma[c].wr - adma[c].rd; }
static void adma_check_done(int c) { if (adma[c].pending && adma_queued(c) <= 1024) { adma[c].pending = 0; iop_dma_done(c); } }
int spu2_adma_active(void) { return adma[0].pending || adma[1].pending || adma_queued(0) || adma_queued(1); }
int16_t spu2_adma_sample(int c, int right) { return adma[c].have ? (right ? in_r[c] : in_l[c]) : 0; }
/* Input area in sound RAM (halfwords): core c left at 0x2000 + 0x400*c, right at +0x200; two halves of 256 samples.
   The play position advances at 48 kHz; when it enters one half, the next queued block is written into the other. */
static void adma_fill_half(int c, int half) {
    Adma *a = &adma[c];
    if (adma_queued(c) < 512) return;                       /* underrun: the old data plays again, as on hardware */
    uint32_t bl = 0x2000 + 0x400 * c + 256 * half, br = bl + 0x200;
    for (int i = 0; i < 256; i++) { spu_ram[bl + i] = (uint16_t)a->buf[(a->rd + i) & (ADMA_CAP - 1)]; spu_ram[br + i] = (uint16_t)a->buf[(a->rd + 256 + i) & (ADMA_CAP - 1)]; }
    a->rd += 512; a->have = 1;
    check_irq(c, bl, 256); check_irq(c, br, 256);
}
static void adma_step(int c) {          /* advance one 48 kHz sample */
    uint32_t p = in_pos[c];
    if (p == 0) adma_fill_half(c, 1); else if (p == 256) adma_fill_half(c, 0);
    uint32_t al = 0x2000 + 0x400 * c + p, ar = al + 0x200;
    in_l[c] = (int16_t)spu_ram[al]; in_r[c] = (int16_t)spu_ram[ar];
    if (adma[c].have) { check_irq(c, al, 1); check_irq(c, ar, 1); }
    in_pos[c] = (p + 1) & 511;
    adma_check_done(c);
}
int spu2_dma(int core, uint8_t *mem, uint32_t halfwords, int to_spu) {
    uint32_t a = tsa_cur[core];
    check_irq(core, a, halfwords);
    uint16_t admas = reg[CORE(core, 0x1B0)];
    if (to_spu && (admas & (1 << core))) {
        Adma *s = &adma[core];
        for (uint32_t k = 0; k < halfwords; k++) { int16_t v; memcpy(&v, mem + 2 * k, 2); s->buf[(s->wr + k) & (ADMA_CAP - 1)] = v; }
        s->wr += halfwords; n_dma_hw[core] += halfwords; s->pending = 1;
        if (GETENV_ONCE("HG_ADMAWAV") && core == 0) {   /* raw stream PCM (L block, R block per 1 KB) -> interleaved stereo WAV */
            static FILE *af; if (!af) { af = fopen(getenv("HG_ADMAWAV"), "wb"); }
            if (af) for (uint32_t b = 0; b + 512 <= halfwords; b += 512) for (int i = 0; i < 256; i++) {
                int16_t lr[2]; memcpy(&lr[0], mem + 2 * (b + i), 2); memcpy(&lr[1], mem + 2 * (b + 256 + i), 2); fwrite(lr, 2, 2, af); }
            if (af) fflush(af);
        }
        if (getenv("HG_ADMALOG")) {   /* level of the PCM the IOP driver sends, once per second of stream */
            static double e; static uint64_t n; for (uint32_t k = 0; k < halfwords; k++) { double v = (int16_t)((uint16_t *)mem)[k]; e += v * v; }
            n += halfwords; if (n >= 96000) { L("SPU2: ADMA core %d input rms %.1f (at sample %llu)\n", core, sqrt(e / n), (unsigned long long)n_samples); e = 0; n = 0; } }
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

/* ------------------------------------------------------------------ mixer */
static int16_t out_buf[2 * 48000]; static int out_n;
static Vol mvol[2][2];
static FILE *wav; static uint64_t wav_samples;
static void wav_close(void) {
    if (!wav) return;
    uint32_t data = (uint32_t)(wav_samples * 4), riff = 36 + data;
    fseek(wav, 4, SEEK_SET); fwrite(&riff, 4, 1, wav); fseek(wav, 40, SEEK_SET); fwrite(&data, 4, 1, wav); fclose(wav); wav = 0;
}
static void wav_open(void) {
    const char *p = getenv("HG_WAV"); if (!p) return;
    wav = fopen(p, "wb"); if (!wav) return;
    uint8_t h[44] = "RIFF\0\0\0\0WAVEfmt \x10\0\0\0\x01\0\x02\0\x80\xbb\0\0\0\xee\x02\0\x04\0\x10\0data\0\0\0\0";
    fwrite(h, 1, 44, wav); atexit(wav_close);
}
void spu2_flush_output(void);
/* HG_SPUSTAT=1: once per second, every voice (and the stream input) audible in the dry mix: rms, mean, envelope
   phase and level, pitch, current and loop address, ADSR and volume registers */
static int st_on = -1; static double st_s[2][25][2], st_q[2][25]; static int st_n;
static void stat_flush(void) {
    L("SPUSTAT t=%.1f:", n_samples / 48000.0);
    for (int c = 0; c < 2; c++) for (int n = 0; n < 25; n++) {
        double r = sqrt(st_q[c][n] / (2.0 * st_n)); if (r < 20) continue;
        if (n < 24) { Voice *v = &voice[c][n]; L(" [c%d v%d rms %.0f dc %.0f/%.0f ph %d env %d pitch %04x addr %05x loop %05x adsr %04x/%04x vol %04x/%04x]", c, n, r, st_s[c][n][0] / st_n, st_s[c][n][1] / st_n,
            v->phase, v->adsr.level, reg[VREG(c, n, 4)], v->addr, v->loop, reg[VREG(c, n, 6)], reg[VREG(c, n, 8)], reg[VREG(c, n, 0)], reg[VREG(c, n, 2)]); }
        else L(" [c%d ADMA rms %.0f dc %.0f/%.0f]", c, r, st_s[c][n][0] / st_n, st_s[c][n][1] / st_n);
    }
    L("\n"); memset(st_s, 0, sizeof st_s); memset(st_q, 0, sizeof st_q); st_n = 0;
}
static void mix_sample(void) {
    if (st_on < 0) st_on = getenv("HG_SPUSTAT") != 0;
    static int odd; odd ^= 1;
    int32_t core_out_l = 0, core_out_r = 0;
    noise_tick();
    for (int c = 0; c < 2; c++) {
        uint32_t vmixl = mask24(c, 0x188), vmixel = mask24(c, 0x18C), vmixr = mask24(c, 0x190), vmixer = mask24(c, 0x194);
        uint32_t pmon = mask24(c, 0x180), non = mask24(c, 0x184);
        uint16_t mmix = reg[CORE(c, 0x198)], attr = reg[CORE(c, 0x19A)];
        int32_t dl = 0, dr = 0, wl = 0, wr = 0, prev = 0;
        for (int n = 0; n < 24; n++) {
            Voice *v = &voice[c][n];
            int32_t s = voice_sample(c, n, v, prev, pmon, non); prev = s;
            int32_t l = mulv(s, vol_step(&v->vl, reg[VREG(c, n, 0)])), r = mulv(s, vol_step(&v->vr, reg[VREG(c, n, 2)]));
            reg[VREG(c, n, 0xC)] = (uint16_t)v->vl.cur; reg[VREG(c, n, 0xE)] = (uint16_t)v->vr.cur;
            if (vmixl & (1u << n)) dl += l; if (vmixr & (1u << n)) dr += r;
            if (vmixel & (1u << n)) wl += l; if (vmixer & (1u << n)) wr += r;
            if (st_on) { int32_t dl1 = (vmixl >> n & 1) ? l : 0, dr1 = (vmixr >> n & 1) ? r : 0; st_s[c][n][0] += dl1; st_s[c][n][1] += dr1; st_q[c][n] += (double)dl1 * dl1 + (double)dr1 * dr1; }
        }
        if (!(mmix & 0x800)) dl = 0; if (!(mmix & 0x400)) dr = 0; if (!(mmix & 0x200)) wl = 0; if (!(mmix & 0x100)) wr = 0;
        /* memory input (auto-DMA stream) with BVOL, sound input (core 0 output into core 1) with AVOL */
        int32_t ml = mulv(spu2_adma_sample(c, 0), preg(c, 0x0C)), mr = mulv(spu2_adma_sample(c, 1), preg(c, 0x0E));
        int32_t sl = c ? mulv(core_out_l, preg(c, 0x08)) : 0, sr = c ? mulv(core_out_r, preg(c, 0x0A)) : 0;
        if (st_on) { st_s[c][24][0] += ml; st_s[c][24][1] += mr; st_q[c][24] += (double)ml * ml + (double)mr * mr; }
        if (mmix & 0x80) dl += ml; if (mmix & 0x40) dr += mr; if (mmix & 0x20) wl += ml; if (mmix & 0x10) wr += mr;
        if (mmix & 0x08) dl += sl; if (mmix & 0x04) dr += sr; if (mmix & 0x02) wl += sl; if (mmix & 0x01) wr += sr;
        int32_t rl, rr; reverb_48k(c, wl, wr, &rl, &rr, odd);
        int32_t ol = dl + mulv(rl, preg(c, 0x04)), orr = dr + mulv(rr, preg(c, 0x06));
        ol = mulv(sat16(ol), vol_step(&mvol[c][0], reg[(0x760 + c * 0x28) >> 1]));
        orr = mulv(sat16(orr), vol_step(&mvol[c][1], reg[(0x762 + c * 0x28) >> 1]));
        if (!(attr & 0x8000)) ol = orr = 0;
        set_mask24(c, 0x340, endx[c]);
        core_out_l = sat16(ol); core_out_r = sat16(orr);
    }
    if (st_on && ++st_n >= 48000) stat_flush();
    out_buf[out_n * 2] = (int16_t)core_out_l; out_buf[out_n * 2 + 1] = (int16_t)core_out_r;
    if (++out_n >= 800) spu2_flush_output();
}
void spu2_flush_output(void) {
    if (!out_n) return;
    if (wav) { fwrite(out_buf, 4, out_n, wav); wav_samples += out_n; }
    host_audio_push(out_buf, out_n);
    out_n = 0;
}

/* ------------------------------------------------------------------ registers */
uint32_t spu2_read(uint32_t off, int size) {
    uint32_t i = (off & 0x7FF) >> 1;
    int core = off >= 0x400 && off < 0x760 ? 1 : 0, r = off & 0x3FF;
    if (off < 0x760 && r == 0x344) return 0x80;                        /* STATX: ready, no transfer in progress */
    if (off < 0x760 && r == 0x1AC) { uint16_t v = spu_ram[tsa_cur[core] & RAMMASK]; tsa_cur[core]++; return v; }
    if (off < 0x760 && r >= 0x1C0 && r < 0x2E0 && ((r - 0x1C0) % 12) >= 8) {          /* NAX: current address */
        int n = (r - 0x1C0) / 12; uint32_t a = voice[core][n].addr;
        return ((r - 0x1C0) % 12) == 8 ? (a >> 16) & 0xF : a & 0xFFFF;
    }
    uint32_t v = reg[i];
    if (size == 4) v |= (uint32_t)reg[(i + 1) & 0x3FF] << 16;
    return v;
}
void spu2_write(uint32_t off, uint32_t v, int size) {
    if (size == 4) { spu2_write(off, v & 0xFFFF, 2); spu2_write(off + 2, v >> 16, 2); return; }
    uint32_t i = (off & 0x7FF) >> 1;
    int core = off >= 0x400 && off < 0x760 ? 1 : 0, r = off & 0x3FF;
    static long rl_lo = -2, rl_hi; if (rl_lo == -2) { const char *p = getenv("HG_SPUREGLOG"); rl_lo = -1; if (p) { char *e; rl_lo = strtol(p, &e, 16); rl_hi = *e == '-' ? strtol(e + 1, 0, 16) : rl_lo; } }
    if (rl_lo >= 0 && (long)(off & 0x7FF) >= rl_lo && (long)(off & 0x7FF) <= rl_hi) L("SPU2 reg %03x = %04x (t %.3f s) iop pc %08x ra %08x\n", off & 0x7FF, v & 0xFFFF, n_samples / 48000.0, iop.pc, iop.r[31]);
    if (off < 0x760) {
        if (r == 0x1A0 || r == 0x1A2) { uint32_t m = r == 0x1A0 ? (v & 0xFFFF) : (v & 0xFF) << 16; for (int n = 0; n < 24; n++) if (m & (1u << n)) key_on(core, n); return; }
        if (r == 0x1A4 || r == 0x1A6) { uint32_t m = r == 0x1A4 ? (v & 0xFFFF) : (v & 0xFF) << 16; for (int n = 0; n < 24; n++) if (m & (1u << n)) key_off(core, n); return; }
        if (r == 0x340 || r == 0x342) { endx[core] &= r == 0x340 ? 0xFF0000u : 0xFFFFu; }
        if (r >= 0x1C0 && r < 0x2E0 && ((r - 0x1C0) % 12) >= 8) {   /* NAX write: jump */
            int n = (r - 0x1C0) / 12; reg[i] = (uint16_t)v; voice[core][n].addr = pair(core, 0x1C0 + n * 12 + 8); return;
        }
    }
    if (off < 0x760 && r == 0x19A && (v & 0x40) && !(reg[i] & 0x40)) irq_armed[core] = 1;   /* IRQ enable 0 -> 1 re-arms */
    if (off == 0x7C2) { reg[i] &= ~v; return; }                                             /* IRQINFO: write 1 to clear */
    reg[i] = (uint16_t)v;
    if (off < 0x760) {
        if (r == 0x19C || r == 0x19E) irq_armed[core] = 1;
        if (r == 0x1A8 || r == 0x1AA) tsa_cur[core] = pair(core, 0x1A8);
        else if (r == 0x1AC) { check_irq(core, tsa_cur[core], 1); spu_ram[tsa_cur[core] & RAMMASK] = (uint16_t)v; tsa_cur[core]++; }
        else if (r == 0x2E0 || r == 0x2E2) rev[core].cur = pair(core, 0x2E0);
    }
}

void spu2_run(uint64_t samples) {
    static int init; if (!init) { init = 1; wav_open(); }
    double t0 = host_now();
    for (uint64_t s = 0; s < samples; s++) { adma_step(0); adma_step(1); mix_sample(); }
    tprof[3] += host_now() - t0;
    n_samples += samples;
}
void spu2_dump_stats(void) {
    int active = 0; for (int c = 0; c < 2; c++) for (int n = 0; n < 24; n++) active += voice[c][n].phase != PH_OFF;
    L("SPU2: %llu samples, %llu key-ons, %d voices active, %llu IRQs, DMA core0 %llu / core1 %llu halfwords\n", (unsigned long long)n_samples,
      (unsigned long long)n_keyon, active, (unsigned long long)n_irq, (unsigned long long)n_dma_hw[0], (unsigned long long)n_dma_hw[1]);
    for (int c = 0; c < 2; c++) {
        double e = 0; for (int k = 0; k < 1024; k++) { double v = (int16_t)spu_ram[0x2000 + 0x400 * c + k]; e += v * v; }
        L("  SPU2 core %d: AVOL %04x/%04x, input-area rms %.1f; voice env/vol:", c, (uint16_t)preg(c, 8), (uint16_t)preg(c, 0xA), sqrt(e / 1024));
        for (int n = 0; n < 24; n++) if (voice[c][n].phase != PH_OFF) L(" v%d env %d vol %04x/%04x", n, voice[c][n].adsr.level, reg[VREG(c, n, 0)], reg[VREG(c, n, 2)]);
        L("\n"); }
    for (int c = 0; c < 2; c++) L("  SPU2 core %d: ATTR 0x%04x IRQA 0x%05x armed %d MMIX 0x%04x ADMAS 0x%x MVOL %04x/%04x BVOL %04x/%04x EVOL %04x/%04x ESA 0x%05x input pos %u queued %u\n", c,
        reg[CORE(c, 0x19A)], pair(c, 0x19C), irq_armed[c], reg[CORE(c, 0x198)], reg[CORE(c, 0x1B0)], (uint16_t)preg(c, 0), (uint16_t)preg(c, 2), (uint16_t)preg(c, 0xC), (uint16_t)preg(c, 0xE),
        (uint16_t)preg(c, 4), (uint16_t)preg(c, 6), pair(c, 0x2E0), in_pos[c], adma_queued(c));
    spu2_flush_output(); wav_close();
    if (getenv("HG_SPUDUMP")) { FILE *f = fopen(getenv("HG_SPUDUMP"), "wb"); if (f) { fwrite(spu_ram, 2, 1 << 20, f); fclose(f); } }
}
