/* Graphics Synthesizer: software implementation for the boot/title (2D) path.
   - 4 MB local memory with the PS2 page/block/column layouts (CT32/24, CT16/16S, T8, T4, 8H/4HL/4HH, Z formats)
   - GIF packet parser per path (PACKED / REGLIST / IMAGE), host->local and local->local transfers
   - rasterizer: points, lines, triangles (strip/fan), sprites; Gouraud/flat, ST/UV texturing with CLUT, TFX, TCC,
     CLAMP modes, nearest/bilinear, alpha test, destination alpha test, Z test, alpha blending, PABE/FBA, FBMSK/ZMSK, fog
   - display: reads PMODE/DISPFB/DISPLAY and writes frames as PPM when HG_FRAMES is set
   Written from the GS register specification; the T8/T4 column tables are checked for bijectivity at start-up. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <pthread.h>
#include <time.h>
#include <stdatomic.h>
#include "compat.h"
#include "ee_runtime.h"
#include "hle.h"
#if defined(__x86_64__)
#include <immintrin.h>
#endif

/* ================================================================== local memory and swizzling */
static uint32_t vm[(1 << 20) + 4];               /* 4 MB = 1M words (+ padding for 4-byte gathers at byte addresses) */
#define VM8 ((uint8_t *)vm)
#define VM16 ((uint16_t *)vm)

static const uint8_t blockTable32[4][8] = { {0,1,4,5,16,17,20,21}, {2,3,6,7,18,19,22,23}, {8,9,12,13,24,25,28,29}, {10,11,14,15,26,27,30,31} };
static const uint8_t blockTable32Z[4][8] = { {24,25,28,29,8,9,12,13}, {26,27,30,31,10,11,14,15}, {16,17,20,21,0,1,4,5}, {18,19,22,23,2,3,6,7} };
static const uint8_t blockTable16[8][4] = { {0,2,8,10}, {1,3,9,11}, {4,6,12,14}, {5,7,13,15}, {16,18,24,26}, {17,19,25,27}, {20,22,28,30}, {21,23,29,31} };
static const uint8_t blockTable16S[8][4] = { {0,2,16,18}, {1,3,17,19}, {8,10,24,26}, {9,11,25,27}, {4,6,20,22}, {5,7,21,23}, {12,14,28,30}, {13,15,29,31} };
static const uint8_t blockTable16Z[8][4] = { {24,26,16,18}, {25,27,17,19}, {28,30,20,22}, {29,31,21,23}, {8,10,0,2}, {9,11,1,3}, {12,14,4,6}, {13,15,5,7} };
static const uint8_t blockTable8[4][8] = { {0,1,4,5,16,17,20,21}, {2,3,6,7,18,19,22,23}, {8,9,12,13,24,25,28,29}, {10,11,14,15,26,27,30,31} };
static const uint8_t blockTable4[8][4] = { {0,2,8,10}, {1,3,9,11}, {4,6,12,14}, {5,7,13,15}, {16,18,24,26}, {17,19,25,27}, {20,22,28,30}, {21,23,29,31} };
static const uint8_t columnTable32[8][8] = {
    {0,1,4,5,8,9,12,13}, {2,3,6,7,10,11,14,15}, {16,17,20,21,24,25,28,29}, {18,19,22,23,26,27,30,31},
    {32,33,36,37,40,41,44,45}, {34,35,38,39,42,43,46,47}, {48,49,52,53,56,57,60,61}, {50,51,54,55,58,59,62,63} };
static const uint8_t columnTable16[8][16] = {
    {0,2,8,10,16,18,24,26,1,3,9,11,17,19,25,27}, {4,6,12,14,20,22,28,30,5,7,13,15,21,23,29,31},
    {32,34,40,42,48,50,56,58,33,35,41,43,49,51,57,59}, {36,38,44,46,52,54,60,62,37,39,45,47,53,55,61,63},
    {64,66,72,74,80,82,88,90,65,67,73,75,81,83,89,91}, {68,70,76,78,84,86,92,94,69,71,77,79,85,87,93,95},
    {96,98,104,106,112,114,120,122,97,99,105,107,113,115,121,123}, {100,102,108,110,116,118,124,126,101,103,109,111,117,119,125,127} };
static const uint8_t columnTable8[16][16] = {
    {0,4,16,20,32,36,48,52,2,6,18,22,34,38,50,54}, {8,12,24,28,40,44,56,60,10,14,26,30,42,46,58,62},
    {33,37,1,5,49,53,17,21,35,39,3,7,51,55,19,23}, {41,45,9,13,57,61,25,29,43,47,11,15,59,63,27,31},
    {96,100,112,116,64,68,80,84,98,102,114,118,66,70,82,86}, {104,108,120,124,72,76,88,92,106,110,122,126,74,78,90,94},
    {65,69,81,85,97,101,113,117,67,71,83,87,99,103,115,119}, {73,77,89,93,105,109,121,125,75,79,91,95,107,111,123,127},
    {128,132,144,148,160,164,176,180,130,134,146,150,162,166,178,182}, {136,140,152,156,168,172,184,188,138,142,154,158,170,174,186,190},
    {161,165,129,133,177,181,145,149,163,167,131,135,179,183,147,151}, {169,173,137,141,185,189,153,157,171,175,139,143,187,191,155,159},
    {224,228,240,244,192,196,208,212,226,230,242,246,194,198,210,214}, {232,236,248,252,200,204,216,220,234,238,250,254,202,206,218,222},
    {193,197,209,213,225,229,241,245,195,199,211,215,227,231,243,247}, {201,205,217,221,233,237,249,253,203,207,219,223,235,239,251,255} };
static uint16_t columnTable4[16][32];
static void build_t4_table(void) {
    static const uint16_t c0[4][32] = {
        {0,8,32,40,64,72,96,104,2,10,34,42,66,74,98,106,4,12,36,44,68,76,100,108,6,14,38,46,70,78,102,110},
        {16,24,48,56,80,88,112,120,18,26,50,58,82,90,114,122,20,28,52,60,84,92,116,124,22,30,54,62,86,94,118,126},
        {65,73,97,105,1,9,33,41,67,75,99,107,3,11,35,43,69,77,101,109,5,13,37,45,71,79,103,111,7,15,39,47},
        {81,89,113,121,17,25,49,57,83,91,115,123,19,27,51,59,85,93,117,125,21,29,53,61,87,95,119,127,23,31,55,63} };
    static const uint16_t c1[4][32] = {
        {192,200,224,232,128,136,160,168,194,202,226,234,130,138,162,170,196,204,228,236,132,140,164,172,198,206,230,238,134,142,166,174},
        {208,216,240,248,144,152,176,184,210,218,242,250,146,154,178,186,212,220,244,252,148,156,180,188,214,222,246,254,150,158,182,190},
        {129,137,161,169,193,201,225,233,131,139,163,171,195,203,227,235,133,141,165,173,197,205,229,237,135,143,167,175,199,207,231,239},
        {145,153,177,185,209,217,241,249,147,155,179,187,211,219,243,251,149,157,181,189,213,221,245,253,151,159,183,191,215,223,247,255} };
    for (int r = 0; r < 4; r++) for (int x = 0; x < 32; x++) {
        columnTable4[r][x] = c0[r][x]; columnTable4[4 + r][x] = c1[r][x];
        columnTable4[8 + r][x] = c0[r][x] + 256; columnTable4[12 + r][x] = c1[r][x] + 256;
    }
}
static int check_bijective(void) {
    uint8_t seen[512];
    memset(seen, 0, 256); for (int y = 0; y < 16; y++) for (int x = 0; x < 16; x++) seen[columnTable8[y][x]]++;
    for (int i = 0; i < 256; i++) if (seen[i] != 1) return 8;
    memset(seen, 0, 512); for (int y = 0; y < 16; y++) for (int x = 0; x < 32; x++) seen[columnTable4[y][x]]++;
    for (int i = 0; i < 512; i++) if (seen[i] != 1) return 4;
    return 0;
}

/* bw = buffer width in 64-pixel units; return element index in the format's unit (word, halfword, byte, nibble).
   32-bit layouts: the x contribution only depends on y mod 32, so it is tabulated (xoff32 / xoff32z, x < 2048). */
static uint32_t xoff32[33][2048], xoff32z[33][2048];   /* row 32: padding for 8-wide loads that run past x = 2047 */
static void build_xoff(void) {
    for (int y = 0; y < 32; y++) for (int x = 0; x < 2048; x++) {
        xoff32[y][x] = (x >> 6) * 2048 + blockTable32[(y >> 3) & 3][(x >> 3) & 7] * 64 + columnTable32[y & 7][x & 7];
        xoff32z[y][x] = (x >> 6) * 2048 + blockTable32Z[(y >> 3) & 3][(x >> 3) & 7] * 64 + columnTable32[y & 7][x & 7];
    }
}
static inline uint32_t addr32(uint32_t bp, uint32_t bw, int x, int y) {
    return (bp * 64 + (uint32_t)(y >> 5) * bw * 2048 + xoff32[y & 31][x & 2047]) & 0xFFFFF;
}
static inline uint32_t addr32z(uint32_t bp, uint32_t bw, int x, int y) {
    return (bp * 64 + (uint32_t)(y >> 5) * bw * 2048 + xoff32z[y & 31][x & 2047]) & 0xFFFFF;
}
static inline uint32_t addr16(uint32_t bp, uint32_t bw, int x, int y, const uint8_t bt[8][4]) {
    uint32_t page = (x >> 6) + (y >> 6) * bw;
    return ((bp + page * 32 + bt[(y >> 3) & 7][(x >> 4) & 3]) * 128 + columnTable16[y & 7][x & 15]) & 0x1FFFFF;
}
static inline uint32_t addr8(uint32_t bp, uint32_t bw, int x, int y) {
    uint32_t page = (x >> 7) + (y >> 6) * ((bw + 1) >> 1);
    return ((bp + page * 32 + blockTable8[(y >> 4) & 3][(x >> 4) & 7]) * 256 + columnTable8[y & 15][x & 15]) & 0x3FFFFF;
}
static inline uint32_t addr4(uint32_t bp, uint32_t bw, int x, int y) {
    uint32_t page = (x >> 7) + (y >> 7) * ((bw + 1) >> 1);
    return ((bp + page * 32 + blockTable4[(y >> 4) & 7][(x >> 5) & 3]) * 512 + columnTable4[y & 15][x & 31]) & 0x7FFFFF;
}

enum { CT32 = 0x00, CT24 = 0x01, CT16 = 0x02, CT16S = 0x0a, T8 = 0x13, T4 = 0x14, T8H = 0x1b, T4HL = 0x24, T4HH = 0x2c,
       Z32 = 0x30, Z24 = 0x31, Z16 = 0x32, Z16S = 0x3a };

static inline uint32_t read_px(uint32_t bp, uint32_t bw, int psm, int x, int y) {
    switch (psm) {
    case CT32: case CT24: return vm[addr32(bp, bw, x, y)];
    case CT16: return VM16[addr16(bp, bw, x, y, blockTable16)];
    case CT16S: return VM16[addr16(bp, bw, x, y, blockTable16S)];
    case T8: return VM8[addr8(bp, bw, x, y)];
    case T4: { uint32_t a = addr4(bp, bw, x, y); return (VM8[a >> 1] >> ((a & 1) * 4)) & 15; }
    case T8H: return vm[addr32(bp, bw, x, y)] >> 24;
    case T4HL: return (vm[addr32(bp, bw, x, y)] >> 24) & 15;
    case T4HH: return vm[addr32(bp, bw, x, y)] >> 28;
    case Z32: case Z24: return vm[addr32z(bp, bw, x, y)];
    case Z16: return VM16[addr16(bp, bw, x, y, blockTable16Z)];
    case Z16S: return VM16[addr16(bp, bw, x, y, blockTable16Z)];
    }
    return vm[addr32(bp, bw, x, y)];
}
static void write_px(uint32_t bp, uint32_t bw, int psm, int x, int y, uint32_t v) {
    switch (psm) {
    case CT32: vm[addr32(bp, bw, x, y)] = v; break;
    case CT24: { uint32_t *p = &vm[addr32(bp, bw, x, y)]; *p = (*p & 0xFF000000u) | (v & 0xFFFFFF); break; }
    case CT16: VM16[addr16(bp, bw, x, y, blockTable16)] = v; break;
    case CT16S: VM16[addr16(bp, bw, x, y, blockTable16S)] = v; break;
    case T8: VM8[addr8(bp, bw, x, y)] = v; break;
    case T4: { uint32_t a = addr4(bp, bw, x, y); uint8_t *p = &VM8[a >> 1]; *p = (a & 1) ? (*p & 0x0F) | (v << 4) : (*p & 0xF0) | (v & 15); break; }
    case T8H: { uint32_t *p = &vm[addr32(bp, bw, x, y)]; *p = (*p & 0x00FFFFFFu) | (v << 24); break; }
    case T4HL: { uint32_t *p = &vm[addr32(bp, bw, x, y)]; *p = (*p & 0xF0FFFFFFu) | ((v & 15) << 24); break; }
    case T4HH: { uint32_t *p = &vm[addr32(bp, bw, x, y)]; *p = (*p & 0x0FFFFFFFu) | ((v & 15) << 28); break; }
    case Z32: vm[addr32z(bp, bw, x, y)] = v; break;
    case Z24: { uint32_t *p = &vm[addr32z(bp, bw, x, y)]; *p = (*p & 0xFF000000u) | (v & 0xFFFFFF); break; }
    case Z16: case Z16S: VM16[addr16(bp, bw, x, y, blockTable16Z)] = v; break;
    default: vm[addr32(bp, bw, x, y)] = v;
    }
}
static inline uint32_t c16to32(uint32_t c, int aem, uint8_t ta0, uint8_t ta1) {
    uint32_t r = (c & 31) << 3, g = ((c >> 5) & 31) << 3, b = ((c >> 10) & 31) << 3;
    uint32_t a = (c & 0x8000) ? ta1 : (aem && !(c & 0x7FFF)) ? 0 : ta0;
    return r | g << 8 | b << 16 | a << 24;
}
static inline uint32_t c32to16(uint32_t c) {
    return ((c >> 3) & 31) | (((c >> 11) & 31) << 5) | (((c >> 19) & 31) << 10) | ((c >> 16) & 0x8000);
}

/* ================================================================== registers */
typedef struct {
    uint64_t tex0, clamp, tex1, xyoffset, scissor, alpha, test, fba, frame, zbuf;
} GsCtx;
static GsCtx gc[2];
static uint64_t r_prim, r_rgbaq, r_st, r_uv, r_fog, r_prmodecont = 1, r_prmode, r_texclut, r_texa, r_fogcol, r_dimx, r_dthe, r_colclamp = 1, r_pabe;
static uint64_t r_bitbltbuf, r_trxpos, r_trxreg;
static float st_q = 1.0f;
/* CLUT snapshots: every load writes a new one, so draws queued for the GS workers keep the palette they were set up
   with (TexState.clut) */
#define CLUT_RING 64
static uint32_t clut_ring[CLUT_RING][256]; static int clut_slot; static uint64_t clut_slot_seq[CLUT_RING];
static uint32_t *clut = clut_ring[0];
static uint32_t cbp0, cbp1;
static uint8_t *gspriv;
static int mtgs;                                  /* GS runs on its own thread (see "GS thread" below) */
static uint64_t n_prims, n_xfer_qw, n_kicks_frame;
/* pixel counters per rasterizing thread (the GS workers draw in parallel); gs_count() sums them */
#define GS_MAXW 16
typedef struct { uint64_t pixels, visits; char pad[48]; } GsCount;
static GsCount gs_cnt[GS_MAXW + 1];
static _Thread_local GsCount *gcnt = &gs_cnt[0];
#define n_pixels (gcnt->pixels)
#define n_visits (gcnt->visits)
static uint64_t gs_count(int visits) { uint64_t s = 0; for (int i = 0; i <= GS_MAXW; i++) s += visits ? gs_cnt[i].visits : gs_cnt[i].pixels; return s; }
/* rows this thread draws: all of them, or for GS worker band_k of band_n its 8-row bands */
static _Thread_local int band_k, band_n = 1;
#define ROW_MINE(y) (band_n == 1 || (((y) >> 3) % band_n) == band_k)
/* GS workers (see "GS workers" below), used from the CLUT and transfer code */
static int gsw_n;
static void gsw_drain(void);
static void gsw_wait_seq(uint64_t seq);
static void gsw_access(const uint64_t pages[8], int write);
static void mark_pages(uint64_t pages[8], uint32_t bp, uint32_t bw, int psm, int x0, int y0, int x1, int y1);
static void *gs_sprhist_ptr; static int gs_sprhist_n;
static uint64_t st_prims[3], st_px[3], st_visits[3], st_tex[3], st_blend[3], st_z[3]; static double st_time[3];

#define BITS(v, lo, n) ((uint32_t)(((v) >> (lo)) & ((1ull << (n)) - 1)))

/* ================================================================== CLUT */
static void clut_load(uint64_t tex0) {
    int cld = BITS(tex0, 61, 3); uint32_t cbp = BITS(tex0, 37, 14);
    switch (cld) {
    case 0: return;
    case 1: break;
    case 2: cbp0 = cbp; break;
    case 3: cbp1 = cbp; break;
    case 4: if (cbp == cbp0) return; cbp0 = cbp; break;
    case 5: if (cbp == cbp1) return; cbp1 = cbp; break;
    default: return;
    }
    int psm = BITS(tex0, 20, 6), cpsm = BITS(tex0, 51, 4), csm = BITS(tex0, 55, 1), csa = BITS(tex0, 56, 5);
    int n = (psm == T8 || psm == T8H) ? 256 : 16;
    int off = n == 16 ? (cpsm == CT32 ? (csa & 15) * 16 : csa * 16) : 0;
    if (gsw_n) {   /* the CLUT is read from local memory now: queued draws must have written it */
        uint64_t pg[8] = {0}; const int cps = (cpsm == CT32 || cpsm == CT24) ? CT32 : cpsm == CT16S ? CT16S : CT16;
        if (csm) mark_pages(pg, cbp, BITS(r_texclut, 0, 6), cps, BITS(r_texclut, 6, 6) * 16, BITS(r_texclut, 12, 10), BITS(r_texclut, 6, 6) * 16 + n - 1, BITS(r_texclut, 12, 10));
        else mark_pages(pg, cbp, 1, cps, 0, 0, 15, 15);
        gsw_access(pg, 0);
        /* a new snapshot, once no queued draw reads the slot it reuses */
        const int s = (clut_slot + 1) % CLUT_RING; gsw_wait_seq(clut_slot_seq[s]);
        memcpy(clut_ring[s], clut, sizeof clut_ring[s]); clut_slot = s; clut = clut_ring[s];
    }
    for (int i = 0; i < n; i++) {
        int x, y;
        if (csm == 0) { if (n == 256) { x = (i & 7) | ((i & 0x10) >> 1); y = ((i & 0xE0) >> 4) | ((i & 8) >> 3); } else { x = i & 7; y = i >> 3; } }
        else { int cbw = BITS(r_texclut, 0, 6), cou = BITS(r_texclut, 6, 6), cov = BITS(r_texclut, 12, 10); x = cou * 16 + i; y = cov; (void)cbw; }
        uint32_t c;
        if (cpsm == CT32 || cpsm == CT24) c = read_px(cbp, csm ? BITS(r_texclut, 0, 6) : 1, CT32, x, y);
        else c = c16to32(read_px(cbp, csm ? BITS(r_texclut, 0, 6) : 1, cpsm == CT16S ? CT16S : CT16, x, y), BITS(r_texa, 15, 1), BITS(r_texa, 0, 8), BITS(r_texa, 32, 8));
        clut[(off + i) & 255] = c;
    }
}

/* ================================================================== texturing */
typedef struct { uint32_t tbp, tbw, psm, tw, th, tcc, tfx, csa, cpsm; int wms, wmt, minu, maxu, minv, maxv; int bilinear; int aem; uint8_t ta0, ta1; const uint32_t *clut; } TexState;
static void tex_setup(TexState *t, const GsCtx *c) {
    uint64_t t0 = c->tex0;
    t->tbp = BITS(t0, 0, 14); t->tbw = BITS(t0, 14, 6); t->psm = BITS(t0, 20, 6);
    t->tw = 1u << BITS(t0, 26, 4); t->th = 1u << BITS(t0, 30, 4); t->tcc = BITS(t0, 34, 1); t->tfx = BITS(t0, 35, 2);
    t->cpsm = BITS(t0, 51, 4); t->csa = BITS(t0, 56, 5);
    if (t->tw > 1024) t->tw = 1024; if (t->th > 1024) t->th = 1024;
    t->wms = BITS(c->clamp, 0, 2); t->wmt = BITS(c->clamp, 2, 2);
    t->minu = BITS(c->clamp, 4, 10); t->maxu = BITS(c->clamp, 14, 10); t->minv = BITS(c->clamp, 24, 10); t->maxv = BITS(c->clamp, 34, 10);
    t->bilinear = BITS(c->tex1, 5, 1);
    t->aem = BITS(r_texa, 15, 1); t->ta0 = BITS(r_texa, 0, 8); t->ta1 = BITS(r_texa, 32, 8);
    t->clut = clut;
}
static inline int wrap(int v, int size, int mode, int mn, int mx) {
    switch (mode) {
    case 0: return v & (size - 1);
    case 1: return v < 0 ? 0 : v >= size ? size - 1 : v;
    case 2: return v < mn ? mn : v > mx ? mx : v;
    default: return (v & mn) | mx;
    }
}
static inline uint32_t texel(const TexState *t, int u, int v) {
    u = wrap(u, t->tw, t->wms, t->minu, t->maxu); v = wrap(v, t->th, t->wmt, t->minv, t->maxv);
    switch (t->psm) {
    case CT32: return read_px(t->tbp, t->tbw, CT32, u, v);
    case CT24: { uint32_t c = read_px(t->tbp, t->tbw, CT32, u, v) & 0xFFFFFF; return c | (uint32_t)((t->aem && !c) ? 0 : t->ta0) << 24; }
    case CT16: case CT16S: return c16to32(read_px(t->tbp, t->tbw, t->psm, u, v), t->aem, t->ta0, t->ta1);
    case T8: case T8H: return t->clut[read_px(t->tbp, t->tbw, t->psm, u, v) & 255];
    case T4: case T4HL: case T4HH: return t->clut[((t->cpsm == CT32 ? (t->csa & 15) : t->csa) * 16 + read_px(t->tbp, t->tbw, t->psm, u, v)) & 255];
    /* depth formats used as textures (depth-based effects): the Z layouts, read like CT32/CT24/CT16 */
    case Z32: return read_px(t->tbp, t->tbw, Z32, u, v);
    case Z24: { uint32_t c = read_px(t->tbp, t->tbw, Z32, u, v) & 0xFFFFFF; return c | (uint32_t)((t->aem && !c) ? 0 : t->ta0) << 24; }
    case Z16: case Z16S: return c16to32(read_px(t->tbp, t->tbw, t->psm, u, v), t->aem, t->ta0, t->ta1);
    }
    return read_px(t->tbp, t->tbw, CT32, u, v);
}
/* u, v in 1/16 texel units */
static inline uint32_t sample(const TexState *t, int u16, int v16) {
    if (!t->bilinear) return texel(t, u16 >> 4, v16 >> 4);
    int u = u16 - 8, v = v16 - 8, fu = u & 15, fv = v & 15; u >>= 4; v >>= 4;
    if (!fu && !fv) return texel(t, u, v);                     /* exact texel: other weights are zero */
    uint32_t c00 = texel(t, u, v), c10 = fu ? texel(t, u + 1, v) : 0, c01 = fv ? texel(t, u, v + 1) : 0, c11 = (fu && fv) ? texel(t, u + 1, v + 1) : 0;
    /* packed: two 8-bit channels per lane; same integer result as the per-channel form (sum of weights = 256) */
    uint32_t w00 = (16 - fu) * (16 - fv), w10 = fu * (16 - fv), w01 = (16 - fu) * fv, w11 = fu * fv;
    uint32_t rb = ((c00 & 0xFF00FF) * w00 + (c10 & 0xFF00FF) * w10 + (c01 & 0xFF00FF) * w01 + (c11 & 0xFF00FF) * w11) >> 8;
    uint32_t ag = (((c00 >> 8) & 0xFF00FF) * w00 + ((c10 >> 8) & 0xFF00FF) * w10 + ((c01 >> 8) & 0xFF00FF) * w01 + ((c11 >> 8) & 0xFF00FF) * w11) >> 8;
    return (rb & 0xFF00FF) | ((ag & 0xFF00FF) << 8);
}

/* ================================================================== pixel pipeline */
typedef struct {
    const GsCtx *c; TexState tex; int tme, abe, fge, fst, iip;
    uint32_t fbp, fbw, fpsm, fbmsk, zbp, zpsm, zmsk;
    int scx0, scx1, scy0, scy1;
    int ate, atst, aref, afail, date, datm, zte, ztst;
    int a, b, cc, d, fix, pabe, fba, colclamp;
    int need_dst, zread, fb16;     /* derived: destination needed, Z must be read, 16-bit frame buffer */
    uint32_t fogcol;               /* FOGCOL at setup (draws may be queued for the GS workers) */
} Draw;
static void draw_setup(Draw *d) {
    int ctxt = BITS(r_prmodecont ? r_prim : r_prmode, 9, 1);
    uint64_t p = r_prmodecont ? r_prim : r_prmode;
    const GsCtx *c = d->c = &gc[ctxt];
    d->tme = BITS(p, 4, 1); d->fge = BITS(p, 5, 1); d->abe = BITS(p, 6, 1) | BITS(p, 7, 1); d->fst = BITS(p, 8, 1); d->iip = BITS(p, 3, 1);
    if (d->tme) tex_setup(&d->tex, c);
    d->fbp = BITS(c->frame, 0, 9) * 32; d->fbw = BITS(c->frame, 16, 6); d->fpsm = BITS(c->frame, 24, 6); d->fbmsk = (uint32_t)(c->frame >> 32);
    d->zbp = BITS(c->zbuf, 0, 9) * 32; d->zpsm = BITS(c->zbuf, 24, 4) | 0x30; d->zmsk = BITS(c->zbuf, 32, 1);
    d->scx0 = BITS(c->scissor, 0, 11); d->scx1 = BITS(c->scissor, 16, 11); d->scy0 = BITS(c->scissor, 32, 11); d->scy1 = BITS(c->scissor, 48, 11);
    d->ate = BITS(c->test, 0, 1); d->atst = BITS(c->test, 1, 3); d->aref = BITS(c->test, 4, 8); d->afail = BITS(c->test, 12, 2);
    d->date = BITS(c->test, 14, 1); d->datm = BITS(c->test, 15, 1); d->zte = BITS(c->test, 16, 1); d->ztst = BITS(c->test, 17, 2);
    d->a = BITS(c->alpha, 0, 2); d->b = BITS(c->alpha, 2, 2); d->cc = BITS(c->alpha, 4, 2); d->d = BITS(c->alpha, 6, 2); d->fix = BITS(c->alpha, 32, 8);
    d->pabe = r_pabe & 1; d->fba = c->fba & 1; d->colclamp = r_colclamp & 1;
    d->fb16 = d->fpsm == CT16 || d->fpsm == CT16S || d->fpsm == Z16 || d->fpsm == Z16S;   /* Z16 frame buffers: 16-bit colour in the Z16 layouts */
    d->need_dst = d->abe || d->date || d->fbmsk || (d->ate && d->afail == 3) || d->fpsm == CT24 || d->fpsm == Z24 || d->fb16;
    d->zread = d->zte && d->ztst >= 2;
    d->fogcol = (uint32_t)r_fogcol;
}
static inline int clamp255(int v, int cc) { return cc ? (v < 0 ? 0 : v > 255 ? 255 : v) : (v & 255); }
static int watch_x = -1, watch_y; static long watch_vb; static uint64_t cur_vb; static char watch_prim[400];
static inline __attribute__((always_inline)) void pixel(const Draw *d, int x, int y, uint32_t z, uint32_t col /* rgba8, a 0x80 = 1.0 */) {
    if (!ROW_MINE(y)) return;
    n_visits++;
    if (__builtin_expect(x == watch_x && y == watch_y, 0) && (long)cur_vb >= watch_vb) L("PIXWATCH (%d,%d) z %u zbuf-now %u col %08x fbp %u: %s\n", x, y, z, read_px(d->zbp, d->fbw, d->zpsm, x, y) & (d->zpsm == Z24 ? 0xFFFFFF : 0xFFFFFFFF), col, d->fbp, watch_prim);
    if (x < d->scx0 || x > d->scx1 || y < d->scy0 || y > d->scy1) return;
    int as = col >> 24;
    int fb_ok = 1, z_ok = 1, rgb_only = 0;
    if (d->ate) {
        int pass;
        switch (d->atst) { case 0: pass = 0; break; case 1: pass = 1; break; case 2: pass = as < d->aref; break; case 3: pass = as <= d->aref; break;
            case 4: pass = as == d->aref; break; case 5: pass = as >= d->aref; break; case 6: pass = as > d->aref; break; default: pass = as != d->aref; }
        if (!pass) switch (d->afail) { case 0: return; case 1: z_ok = 0; break; case 2: fb_ok = 0; break; default: z_ok = 0; rgb_only = 1; }
    }
    uint32_t fa = 0, dst = 0, dst32 = 0;
    if (!d->fb16) fa = (d->fpsm == Z32 || d->fpsm == Z24) ? addr32z(d->fbp, d->fbw, x, y) : addr32(d->fbp, d->fbw, x, y);   /* Z32/Z24 frame buffers: Z layout */
    if (d->need_dst) {
        dst = d->fb16 ? read_px(d->fbp, d->fbw, d->fpsm, x, y) : vm[fa];
        dst32 = d->fb16 ? c16to32(dst, 0, 0, 0x80) : dst;
    }
    if (d->date) { int bit = d->fb16 ? (dst >> 15) & 1 : (dst >> 31) & 1; if (bit != d->datm) return; }
    if (d->zte) {
        if (d->ztst == 0) return;
        if (d->zread) {
            uint32_t zd = read_px(d->zbp, d->fbw, d->zpsm, x, y);
            if (d->zpsm == Z24) zd &= 0xFFFFFF;
            if (d->ztst == 2 ? z < zd : z <= zd) return;
        }
    } else z_ok = 0;
    if (fb_ok) {
        uint32_t out = col;
        if (d->abe && !(d->pabe && !(as & 0x80))) {
            int ad = dst32 >> 24, cf = d->cc == 0 ? as : d->cc == 1 ? ad : d->fix;
            out = 0;
            for (int s = 0; s < 24; s += 8) {
                int cs = (col >> s) & 255, cd = (dst32 >> s) & 255;
                int A = d->a == 0 ? cs : d->a == 1 ? cd : 0, B = d->b == 0 ? cs : d->b == 1 ? cd : 0, D = d->d == 0 ? cs : d->d == 1 ? cd : 0;
                out |= (uint32_t)clamp255(((A - B) * cf >> 7) + D, d->colclamp) << s;
            }
            out |= col & 0xFF000000u;
        }
        if (d->fba) out |= 0x80000000u;
        if (rgb_only) out = (out & 0xFFFFFF) | (dst32 & 0xFF000000u);
        if (d->fb16) { uint32_t o = c32to16(out); o = (o & ~d->fbmsk) | (dst & d->fbmsk & 0xFFFF); write_px(d->fbp, d->fbw, d->fpsm, x, y, o); }
        else if (d->fpsm == CT24 || d->fpsm == Z24) vm[fa] = (dst & 0xFF000000u) | (((out & ~d->fbmsk) | (dst & d->fbmsk)) & 0xFFFFFF);
        else vm[fa] = (out & ~d->fbmsk) | (dst & d->fbmsk);
        n_pixels++;
    }
    if (z_ok && !d->zmsk) write_px(d->zbp, d->fbw, d->zpsm, x, y, z);
}
static inline __attribute__((always_inline)) uint32_t shade(const Draw *d, uint32_t vcol, int u16, int v16, int fog) {
    uint32_t c = vcol;
    if (d->tme) {
        uint32_t t = sample(&d->tex, u16, v16);
        int ta = t >> 24, fa = vcol >> 24; uint32_t out = 0;
        for (int s = 0; s < 24; s += 8) {
            int ct = (t >> s) & 255, cf = (vcol >> s) & 255, v;
            switch (d->tex.tfx) { case 0: v = ct * cf >> 7; break; case 1: v = ct; break; default: v = (ct * cf >> 7) + fa; }
            out |= (uint32_t)(v > 255 ? 255 : v) << s;
        }
        int a;
        switch (d->tex.tfx) { case 0: a = d->tex.tcc ? ta * fa >> 7 : fa; break; case 1: a = d->tex.tcc ? ta : fa; break; case 2: a = d->tex.tcc ? ta + fa : fa; break; default: a = d->tex.tcc ? ta : fa; }
        c = out | (uint32_t)(a > 255 ? 255 : a) << 24;
    }
    if (d->fge) {
        uint32_t fc = d->fogcol, out = c & 0xFF000000u;
        for (int s = 0; s < 24; s += 8) out |= (uint32_t)(((((c >> s) & 255) * fog) + (((fc >> s) & 255) * (255 - fog))) >> 8) << s;
        c = out;
    }
    return c;
}

/* ================================================================== primitives */
typedef struct { int x, y; uint32_t z; uint32_t rgba; float s, t, q; int u, v; int f; } Vtx;
static Vtx vq[3]; static int nvq, prim_count;
static void vtx_uv(const Draw *d, const Vtx *v, float *u, float *w) {
    if (d->fst) { *u = v->u; *w = v->v; }
    else { float q = v->q != 0 ? v->q : 1.0f; *u = v->s / q * d->tex.tw * 16.0f; *w = v->t / q * d->tex.th * 16.0f; }
}
/* Sprite fast path: same arithmetic as pixel()/shade() for the common full-screen pass setups (32-bit frame buffer,
   no destination alpha test, alpha test off or always, no fog, Z either untouched or a Z32/Z24 buffer), with the
   scissor test and the frame/Z row addressing hoisted out of the pixel loop. Output is identical to the generic path. */
static inline int sprite_span_ok(const Draw *d) {
    if (d->fb16 || d->fpsm != CT32 || d->date || (d->ate && d->atst != 1) || d->fge) return 0;
    if (d->zte && (d->ztst == 0 || ((d->zread || !d->zmsk) && d->zpsm != Z32 && d->zpsm != Z24))) return 0;
    return 1;
}
/* One sprite row. TEX: 0 none, 1 CT32 nearest, 2 any other (sample()); TFX: 0-3; BL: 0 off, 1 on. The fields of *d are
   copied to locals because vm stores could otherwise alias them (-fno-strict-aliasing) and force reloads per pixel. */
static inline __attribute__((always_inline)) void sprite_row(const Draw *d, const Vtx *b, int y, int px0, int px1, int x0, float u0, float du, int v16,
                                                           const int TEX, const int TFX, const int TCC, const int BL) {
    const uint32_t fbp = d->fbp, fbw = d->fbw, fbmsk = d->fbmsk, zbp = d->zbp, zpsm = d->zpsm, vcol = b->rgba, z = b->z;
    const int zread = d->zte && d->zread, zwrite = d->zte && !d->zmsk, ztst = d->ztst, need_dst = d->need_dst;
    const int pabe = d->pabe, fba = d->fba, colclamp = d->colclamp, sa = d->a, sb = d->b, sc = d->cc, sd = d->d, fix = d->fix;
    const uint32_t zcmp = zpsm == Z24 ? z & 0xFFFFFF : z;
    const uint32_t fbase = fbp * 64 + (uint32_t)(y >> 5) * fbw * 2048, zbase = zbp * 64 + (uint32_t)(y >> 5) * fbw * 2048;
    const uint32_t *xo = xoff32[y & 31], *xz = xoff32z[y & 31];
    const TexState tx = d->tex;
    uint32_t tbase = 0; const uint32_t *txo = xoff32[0];
    if (TEX == 1) { int v = wrap(v16 >> 4, tx.th, tx.wmt, tx.minv, tx.maxv); tbase = tx.tbp * 64 + (uint32_t)(v >> 5) * tx.tbw * 2048; txo = xoff32[v & 31]; }
    const int fa_v = vcol >> 24;
    for (int x = px0; x < px1; x++) {
        if (zread) {
            uint32_t zd = vm[(zbase + xz[x & 2047]) & 0xFFFFF];
            if (zpsm == Z24) zd &= 0xFFFFFF;
            if (ztst == 2 ? zcmp < zd : zcmp <= zd) continue;
        }
        uint32_t col = vcol;
        if (TEX) {
            const int u16 = (int)(u0 + ((x << 4) - x0) * du);
            uint32_t t;
            if (TEX == 1) t = vm[(tbase + txo[wrap(u16 >> 4, tx.tw, tx.wms, tx.minu, tx.maxu) & 2047]) & 0xFFFFF];
            else t = sample(&tx, u16, v16);
            const int ta = t >> 24; uint32_t out = 0;
            for (int s = 0; s < 24; s += 8) {
                const int ct = (t >> s) & 255, cf = (vcol >> s) & 255; int v;
                if (TFX == 0) v = ct * cf >> 7; else if (TFX == 1) v = ct; else v = (ct * cf >> 7) + fa_v;
                out |= (uint32_t)(v > 255 ? 255 : v) << s;
            }
            int a;
            if (TFX == 0) a = TCC ? ta * fa_v >> 7 : fa_v; else if (TFX == 2) a = TCC ? ta + fa_v : fa_v; else a = TCC ? ta : fa_v;
            col = out | (uint32_t)(a > 255 ? 255 : a) << 24;
        }
        const uint32_t fa = (fbase + xo[x & 2047]) & 0xFFFFF;
        const uint32_t dst = need_dst ? vm[fa] : 0;
        uint32_t out = col; const int as = col >> 24;
        if (BL && !(pabe && !(as & 0x80))) {
            const int ad = dst >> 24, cf = sc == 0 ? as : sc == 1 ? ad : fix;
            out = 0;
            for (int s = 0; s < 24; s += 8) {
                const int cs = (col >> s) & 255, cd = (dst >> s) & 255;
                const int A = sa == 0 ? cs : sa == 1 ? cd : 0, B = sb == 0 ? cs : sb == 1 ? cd : 0, D = sd == 0 ? cs : sd == 1 ? cd : 0;
                out |= (uint32_t)clamp255(((A - B) * cf >> 7) + D, colclamp) << s;
            }
            out |= col & 0xFF000000u;
        }
        if (fba) out |= 0x80000000u;
        vm[fa] = (out & ~fbmsk) | (dst & fbmsk);
        if (zwrite) { uint32_t *zp = &vm[(zbase + xz[x & 2047]) & 0xFFFFF]; *zp = zpsm == Z24 ? (*zp & 0xFF000000u) | (z & 0xFFFFFF) : z; }
    }
}
#if defined(__x86_64__)
/* AVX2 version of sprite_row for 8 pixels at a time: same integer arithmetic per lane, texels and destination pixels
   fetched with gathers, stores done per lane (the swizzled addresses are not contiguous). Used for the frame-buffer
   configurations sprite_span_ok() accepts when Z is neither read nor written; TEX: 0 none, 1 CT32 nearest, 3 CT32 bilinear. */
__attribute__((target("avx2"))) static inline __m256i wrap8(__m256i v, int size, int mode, int mn, int mx) {
    switch (mode) {
    case 0: return _mm256_and_si256(v, _mm256_set1_epi32(size - 1));
    case 1: return _mm256_min_epi32(_mm256_max_epi32(v, _mm256_setzero_si256()), _mm256_set1_epi32(size - 1));
    case 2: { __m256i lo = _mm256_set1_epi32(mn), hi = _mm256_set1_epi32(mx);
        __m256i t = _mm256_blendv_epi8(v, hi, _mm256_cmpgt_epi32(v, hi));
        return _mm256_blendv_epi8(t, lo, _mm256_cmpgt_epi32(lo, v)); }
    default: return _mm256_or_si256(_mm256_and_si256(v, _mm256_set1_epi32(mn)), _mm256_set1_epi32(mx));
    }
}
__attribute__((target("avx2"))) static inline __m256i texel8(const TexState *t, __m256i u, uint32_t tbase, const uint32_t *txo) {
    __m256i o = _mm256_i32gather_epi32((const int *)txo, _mm256_and_si256(wrap8(u, t->tw, t->wms, t->minu, t->maxu), _mm256_set1_epi32(2047)), 4);
    return _mm256_i32gather_epi32((const int *)vm, _mm256_and_si256(_mm256_add_epi32(o, _mm256_set1_epi32(tbase)), _mm256_set1_epi32(0xFFFFF)), 4);
}
__attribute__((target("avx2"))) static void sprite_row_avx2(const Draw *d, const Vtx *b, int y, int px0, int px1, int x0, float u0, float du, int v16, int TEX) {
    const TexState tx = d->tex;
    const uint32_t fbase = d->fbp * 64 + (uint32_t)(y >> 5) * d->fbw * 2048, vcol = b->rgba;
    const uint32_t *xo = xoff32[y & 31];
    const int tfx = tx.tfx, tcc = tx.tcc, need_dst = d->need_dst, bl = d->abe, pabe = d->pabe;
    const int sa = d->a, sb = d->b, sc = d->cc, sd = d->d;
    const __m256i m255 = _mm256_set1_epi32(255), mask20 = _mm256_set1_epi32(0xFFFFF), lane = _mm256_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7);
    const __m256i fbmsk = _mm256_set1_epi32((int)d->fbmsk), fixv = _mm256_set1_epi32(d->fix);
    __m256i vc[3], fav = _mm256_set1_epi32((int)(vcol >> 24));
    for (int k = 0; k < 3; k++) vc[k] = _mm256_set1_epi32((vcol >> (8 * k)) & 255);
    /* texture rows: nearest uses row v, bilinear rows v and v+1 with weight fv */
    uint32_t tb0 = 0, tb1 = 0; const uint32_t *to0 = xoff32[0], *to1 = xoff32[0]; int fv = 0;
    if (TEX == 1) { int v = wrap(v16 >> 4, tx.th, tx.wmt, tx.minv, tx.maxv); tb0 = tx.tbp * 64 + (uint32_t)(v >> 5) * tx.tbw * 2048; to0 = xoff32[v & 31]; }
    if (TEX == 3) {
        int v = v16 - 8; fv = v & 15; v >>= 4;
        int va = wrap(v, tx.th, tx.wmt, tx.minv, tx.maxv), vb = wrap(v + 1, tx.th, tx.wmt, tx.minv, tx.maxv);
        tb0 = tx.tbp * 64 + (uint32_t)(va >> 5) * tx.tbw * 2048; to0 = xoff32[va & 31];
        tb1 = tx.tbp * 64 + (uint32_t)(vb >> 5) * tx.tbw * 2048; to1 = xoff32[vb & 31];
    }
    int x = px0;
    for (; x + 8 <= px1; x += 8) {
        const __m256i xv = _mm256_add_epi32(_mm256_set1_epi32(x), lane);
        const __m256i fa = _mm256_and_si256(_mm256_add_epi32(_mm256_loadu_si256((const __m256i *)(xo + (x & 2047))), _mm256_set1_epi32(fbase)), mask20);
        const __m256i dst = need_dst ? _mm256_i32gather_epi32((const int *)vm, fa, 4) : _mm256_setzero_si256();
        __m256i col = _mm256_set1_epi32((int)vcol);
        if (TEX) {
            __m256 uf = _mm256_cvtepi32_ps(_mm256_sub_epi32(_mm256_slli_epi32(xv, 4), _mm256_set1_epi32(x0)));
            uf = _mm256_add_ps(_mm256_set1_ps(u0), _mm256_mul_ps(uf, _mm256_set1_ps(du)));
            const __m256i u16 = _mm256_cvttps_epi32(uf);
            __m256i t;
            if (TEX == 1) t = texel8(&tx, _mm256_srai_epi32(u16, 4), tb0, to0);
            else {
                const __m256i uu = _mm256_sub_epi32(u16, _mm256_set1_epi32(8)), fu = _mm256_and_si256(uu, _mm256_set1_epi32(15)), ui = _mm256_srai_epi32(uu, 4);
                const __m256i ui1 = _mm256_add_epi32(ui, _mm256_set1_epi32(1));
                const __m256i c00 = texel8(&tx, ui, tb0, to0), c10 = texel8(&tx, ui1, tb0, to0), c01 = texel8(&tx, ui, tb1, to1), c11 = texel8(&tx, ui1, tb1, to1);
                const __m256i f16 = _mm256_set1_epi32(16), fvv = _mm256_set1_epi32(fv), gu = _mm256_sub_epi32(f16, fu), gv = _mm256_sub_epi32(f16, fvv);
                const __m256i w00 = _mm256_mullo_epi32(gu, gv), w10 = _mm256_mullo_epi32(fu, gv), w01 = _mm256_mullo_epi32(gu, fvv), w11 = _mm256_mullo_epi32(fu, fvv);
                const __m256i mrb = _mm256_set1_epi32(0xFF00FF);
                __m256i rb = _mm256_add_epi32(_mm256_add_epi32(_mm256_mullo_epi32(_mm256_and_si256(c00, mrb), w00), _mm256_mullo_epi32(_mm256_and_si256(c10, mrb), w10)),
                                              _mm256_add_epi32(_mm256_mullo_epi32(_mm256_and_si256(c01, mrb), w01), _mm256_mullo_epi32(_mm256_and_si256(c11, mrb), w11)));
                __m256i ag = _mm256_add_epi32(_mm256_add_epi32(_mm256_mullo_epi32(_mm256_and_si256(_mm256_srli_epi32(c00, 8), mrb), w00), _mm256_mullo_epi32(_mm256_and_si256(_mm256_srli_epi32(c10, 8), mrb), w10)),
                                              _mm256_add_epi32(_mm256_mullo_epi32(_mm256_and_si256(_mm256_srli_epi32(c01, 8), mrb), w01), _mm256_mullo_epi32(_mm256_and_si256(_mm256_srli_epi32(c11, 8), mrb), w11)));
                t = _mm256_or_si256(_mm256_and_si256(_mm256_srli_epi32(rb, 8), mrb), _mm256_slli_epi32(_mm256_and_si256(_mm256_srli_epi32(ag, 8), mrb), 8));
            }
            __m256i out = _mm256_setzero_si256();
            for (int k = 0; k < 3; k++) {
                const __m256i ct = _mm256_and_si256(_mm256_srli_epi32(t, 8 * k), m255);
                __m256i v;
                if (tfx == 1) v = ct;
                else { v = _mm256_srai_epi32(_mm256_mullo_epi32(ct, vc[k]), 7); if (tfx != 0) v = _mm256_add_epi32(v, fav); }
                out = _mm256_or_si256(out, _mm256_slli_epi32(_mm256_min_epi32(v, m255), 8 * k));
            }
            const __m256i ta = _mm256_srli_epi32(t, 24);
            __m256i a;
            if (!tcc) a = fav;
            else if (tfx == 0) a = _mm256_srai_epi32(_mm256_mullo_epi32(ta, fav), 7);
            else if (tfx == 2) a = _mm256_add_epi32(ta, fav);
            else a = ta;
            col = _mm256_or_si256(out, _mm256_slli_epi32(_mm256_min_epi32(a, m255), 24));
        }
        __m256i out = col;
        if (bl) {
            const __m256i as = _mm256_srli_epi32(col, 24), ad = _mm256_srli_epi32(dst, 24);
            const __m256i cf = sc == 0 ? as : sc == 1 ? ad : fixv;
            __m256i o = _mm256_and_si256(col, _mm256_set1_epi32((int)0xFF000000u));
            for (int k = 0; k < 3; k++) {
                const __m256i cs = _mm256_and_si256(_mm256_srli_epi32(col, 8 * k), m255), cd = _mm256_and_si256(_mm256_srli_epi32(dst, 8 * k), m255), z = _mm256_setzero_si256();
                const __m256i A = sa == 0 ? cs : sa == 1 ? cd : z, B = sb == 0 ? cs : sb == 1 ? cd : z, D = sd == 0 ? cs : sd == 1 ? cd : z;
                __m256i v = _mm256_add_epi32(_mm256_srai_epi32(_mm256_mullo_epi32(_mm256_sub_epi32(A, B), cf), 7), D);
                v = d->colclamp ? _mm256_min_epi32(_mm256_max_epi32(v, z), m255) : _mm256_and_si256(v, m255);
                o = _mm256_or_si256(o, _mm256_slli_epi32(v, 8 * k));
            }
            if (pabe) {   /* PABE: blend only where the source alpha MSB is set */
                const __m256i on = _mm256_cmpeq_epi32(_mm256_and_si256(as, _mm256_set1_epi32(0x80)), _mm256_set1_epi32(0x80));
                o = _mm256_blendv_epi8(col, o, on);
            }
            out = o;
        }
        if (d->fba) out = _mm256_or_si256(out, _mm256_set1_epi32((int)0x80000000u));
        out = _mm256_or_si256(_mm256_andnot_si256(fbmsk, out), _mm256_and_si256(dst, fbmsk));
        uint32_t oa[8], aa[8]; _mm256_storeu_si256((__m256i *)oa, out); _mm256_storeu_si256((__m256i *)aa, fa);
        for (int i = 0; i < 8; i++) vm[aa[i]] = oa[i];
    }
    if (x < px1) {   /* tail */
        const int TX = TEX == 3 ? 2 : TEX;
        if (bl) { if (TX == 0) sprite_row(d, b, y, x, px1, x0, u0, du, v16, 0, 0, 0, 1); else if (TX == 1) sprite_row(d, b, y, x, px1, x0, u0, du, v16, 1, tfx, tcc, 1); else sprite_row(d, b, y, x, px1, x0, u0, du, v16, 2, tfx, tcc, 1); }
        else { if (TX == 0) sprite_row(d, b, y, x, px1, x0, u0, du, v16, 0, 0, 0, 0); else if (TX == 1) sprite_row(d, b, y, x, px1, x0, u0, du, v16, 1, tfx, tcc, 0); else sprite_row(d, b, y, x, px1, x0, u0, du, v16, 2, tfx, tcc, 0); }
    }
}
static int have_avx2 = -1;
#endif
/* the if-else keeps a following "else" bound to the caller's if */
#define SPRITE_ROWS(TEX, TFX, TCC, BL) for (int y = py0; y < py1; y++) if (!ROW_MINE(y)) ; else sprite_row(d, b, y, px0, px1, x0, u0, du, d->tme ? (int)(v0 + ((y << 4) - y0) * dv) : 0, TEX, TFX, TCC, BL)
static int rows_mine(int y0, int y1) { int n = 0; for (int y = y0; y < y1; y++) n += ROW_MINE(y); return n; }
static void sprite_spans(const Draw *d, const Vtx *b, int px0, int px1, int py0, int py1, int x0, int y0, float u0, float v0, float du, float dv) {
    const int tex = !d->tme ? 0 : (d->tex.psm == CT32 && !d->tex.bilinear) ? 1 : 2, tfx = d->tex.tfx, tcc = d->tex.tcc, bl = d->abe;
#if defined(__x86_64__)
    if (have_avx2 < 0) have_avx2 = __builtin_cpu_supports("avx2") && !(getenv("HG_GSSIMD") && !strcmp(getenv("HG_GSSIMD"), "0"));
    if (have_avx2 && !(d->zte && (d->zread || !d->zmsk)) && (tex != 2 || d->tex.psm == CT32)) {
        const int k = tex == 2 ? 3 : tex;
        for (int y = py0; y < py1; y++) if (ROW_MINE(y)) sprite_row_avx2(d, b, y, px0, px1, x0, u0, du, d->tme ? (int)(v0 + ((y << 4) - y0) * dv) : 0, k);
        uint64_t n = (uint64_t)(px1 > px0 ? px1 - px0 : 0) * (uint64_t)rows_mine(py0, py1);
        n_visits += n; n_pixels += n; return;
    }
#endif
    if (tex == 0) { if (bl) SPRITE_ROWS(0, 0, 0, 1); else SPRITE_ROWS(0, 0, 0, 0); }
    else if (tex == 1 && tfx == 0 && tcc) { if (bl) SPRITE_ROWS(1, 0, 1, 1); else SPRITE_ROWS(1, 0, 1, 0); }
    else if (tex == 2 && tfx == 0 && tcc) { if (bl) SPRITE_ROWS(2, 0, 1, 1); else SPRITE_ROWS(2, 0, 1, 0); }
    else if (tex == 1) { if (bl) SPRITE_ROWS(1, tfx, tcc, 1); else SPRITE_ROWS(1, tfx, tcc, 0); }
    else { if (bl) SPRITE_ROWS(2, tfx, tcc, 1); else SPRITE_ROWS(2, tfx, tcc, 0); }
    uint64_t n = (uint64_t)(px1 > px0 ? px1 - px0 : 0) * (uint64_t)rows_mine(py0, py1);
    n_visits += n; n_pixels += n;
}
#if defined(__x86_64__)
static int tri_simd_ok_ex(const Draw *d, int tex_checked);
static int sprite_tex_overlaps(const Draw *d, int px0, int px1, int py0, int py1, int x0, int y0, float u0, float v0, float du, float dv);
static void sprite_vec(const Draw *d, const Vtx *b, int px0, int px1, int py0, int py1, int x0, int y0, float u0, float v0, float du, float dv);
#endif
/* a sprite's pixel rectangle [px0,px1) x [py0,py1) inside the scissor and its texture mapping (u0, v0 at x0, y0 in
   1/16 pixels; du, dv per 1/16 pixel) */
typedef struct { int px0, px1, py0, py1, x0, y0; float u0, v0, du, dv; } SprGeo;
static void sprite_geo(const Draw *d, const Vtx *a, const Vtx *b, SprGeo *g) {
    int x0 = a->x, x1 = b->x, y0 = a->y, y1 = b->y;
    float u0 = 0, v0 = 0, u1 = 0, v1 = 0;
    if (d->tme) { vtx_uv(d, a, &u0, &v0); vtx_uv(d, b, &u1, &v1); }
    if (x0 > x1) { int t = x0; x0 = x1; x1 = t; float f = u0; u0 = u1; u1 = f; }
    if (y0 > y1) { int t = y0; y0 = y1; y1 = t; float f = v0; v0 = v1; v1 = f; }
    int px0 = (x0 + 15) >> 4, px1 = (x1 + 15) >> 4, py0 = (y0 + 15) >> 4, py1 = (y1 + 15) >> 4;
    if (px0 < d->scx0) px0 = d->scx0; if (px1 > d->scx1 + 1) px1 = d->scx1 + 1;
    if (py0 < d->scy0) py0 = d->scy0; if (py1 > d->scy1 + 1) py1 = d->scy1 + 1;
    g->du = x1 != x0 ? (u1 - u0) / (x1 - x0) : 0; g->dv = y1 != y0 ? (v1 - v0) / (y1 - y0) : 0;
    g->px0 = px0; g->px1 = px1; g->py0 = py0; g->py1 = py1; g->x0 = x0; g->y0 = y0; g->u0 = u0; g->v0 = v0;
}
static void draw_sprite(const Draw *d, const Vtx *a, const Vtx *b) {
    {   /* HG_SPRLOG=vb: log every sprite drawn during that VBlank */
        int x0 = a->x, x1 = b->x, y0 = a->y, y1 = b->y;
        static long svb = -2; if (svb == -2) svb = getenv("HG_SPRLOG") ? atol(getenv("HG_SPRLOG")) : -1;
        if (svb >= 0 && (long)cur_vb == svb)
            L("SPRITE fbp %u psm %02x fbmsk %08x zbp %u zpsm %02x zte %d:%d zmsk %d z %u (0x%x) tme %d tbp %u tpsm %02x abe %d(%d%d%d%d fix %d) rgba %08x x %d-%d y %d-%d\n",
              d->fbp, d->fpsm, d->fbmsk, d->zbp, d->zpsm, d->zte, d->ztst, d->zmsk, b->z, b->z, d->tme, d->tme ? d->tex.tbp : 0, d->tme ? d->tex.psm : 0,
              d->abe, d->a, d->b, d->cc, d->d, d->fix, b->rgba, x0 >> 4, x1 >> 4, y0 >> 4, y1 >> 4);
    }
    SprGeo g; sprite_geo(d, a, b, &g);
    const int px0 = g.px0, px1 = g.px1, py0 = g.py0, py1 = g.py1, x0 = g.x0, y0 = g.y0; const float u0 = g.u0, v0 = g.v0, du = g.du, dv = g.dv;
    if (sprite_span_ok(d)) { sprite_spans(d, b, px0, px1, py0, py1, x0, y0, u0, v0, du, dv); return; }
#if defined(__x86_64__)
    if (have_avx2 < 0) have_avx2 = __builtin_cpu_supports("avx2") && !(getenv("HG_GSSIMD") && !strcmp(getenv("HG_GSSIMD"), "0"));
    if (have_avx2 && tri_simd_ok_ex(d, d->tme && !sprite_tex_overlaps(d, px0, px1, py0, py1, x0, y0, u0, v0, du, dv))) { sprite_vec(d, b, px0, px1, py0, py1, x0, y0, u0, v0, du, dv); return; }
#endif
    for (int y = py0; y < py1; y++) {
        if (!ROW_MINE(y)) continue;
        float vv = v0 + ((y << 4) - y0) * dv;
        for (int x = px0; x < px1; x++) {
            float uu = u0 + ((x << 4) - x0) * du;
            pixel(d, x, y, b->z, shade(d, b->rgba, (int)uu, (int)vv, b->f));
        }
    }
}
static void draw_triangle(const Draw *d, const Vtx *v0, const Vtx *v1, const Vtx *v2) {
    float x0 = v0->x / 16.0f, y0 = v0->y / 16.0f, x1 = v1->x / 16.0f, y1 = v1->y / 16.0f, x2 = v2->x / 16.0f, y2 = v2->y / 16.0f;
    float area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);
    if (area == 0) return;
    int minx = (int)floorf(fminf(x0, fminf(x1, x2))), maxx = (int)ceilf(fmaxf(x0, fmaxf(x1, x2)));
    int miny = (int)floorf(fminf(y0, fminf(y1, y2))), maxy = (int)ceilf(fmaxf(y0, fmaxf(y1, y2)));
    if (minx < d->scx0) minx = d->scx0; if (maxx > d->scx1) maxx = d->scx1;
    if (miny < d->scy0) miny = d->scy0; if (maxy > d->scy1) maxy = d->scy1;
    float inv = 1.0f / area;
    float ua[3] = {0}, va[3] = {0}, qa[3] = {1, 1, 1};
    const Vtx *vs[3] = { v0, v1, v2 };
    for (int i = 0; i < 3; i++) {
        if (!d->tme) continue;
        if (d->fst) { ua[i] = vs[i]->u; va[i] = vs[i]->v; qa[i] = 1; }
        else { ua[i] = vs[i]->s * d->tex.tw * 16.0f; va[i] = vs[i]->t * d->tex.th * 16.0f; qa[i] = vs[i]->q != 0 ? vs[i]->q : 1.0f; }
    }
    for (int y = miny; y <= maxy; y++) if (ROW_MINE(y)) for (int x = minx; x <= maxx; x++) {
        float px = (float)x, py = (float)y;
        float w0 = ((x1 - px) * (y2 - py) - (x2 - px) * (y1 - py)) * inv;
        float w1 = ((x2 - px) * (y0 - py) - (x0 - px) * (y2 - py)) * inv;
        float w2 = 1.0f - w0 - w1;
        if (w0 < 0 || w1 < 0 || w2 < 0) {
            /* top-left rule approximation: accept tiny negative due to float error */
            if (w0 < -1e-6f || w1 < -1e-6f || w2 < -1e-6f) { if (x == watch_x && y == watch_y && (long)cur_vb >= watch_vb && w0 > -0.05f && w1 > -0.05f && w2 > -0.05f) L("PIXWATCH-MISS (%d,%d) fbp %u tbp %u z %u: w %.9g %.9g %.9g area %g v (%d,%d) (%d,%d) (%d,%d)\n", x, y, d->fbp, d->tme ? d->tex.tbp : 0, v0->z, w0, w1, w2, area, v0->x, v0->y, v1->x, v1->y, v2->x, v2->y); continue; }
        }
        uint32_t col;
        if (d->iip) {
            col = 0;
            for (int s = 0; s < 32; s += 8) {
                float c = ((v0->rgba >> s) & 255) * w0 + ((v1->rgba >> s) & 255) * w1 + ((v2->rgba >> s) & 255) * w2;
                int ci = (int)(c + 0.5f); col |= (uint32_t)(ci < 0 ? 0 : ci > 255 ? 255 : ci) << s;
            }
        } else col = v2->rgba;
        double zf = (double)v0->z * w0 + (double)v1->z * w1 + (double)v2->z * w2;
        uint32_t z = zf <= 0 ? 0 : zf >= 4294967295.0 ? 0xFFFFFFFFu : (uint32_t)zf;
        int u16 = 0, v16 = 0;
        if (d->tme) {
            float q = qa[0] * w0 + qa[1] * w1 + qa[2] * w2;
            float uu = ua[0] * w0 + ua[1] * w1 + ua[2] * w2, vv = va[0] * w0 + va[1] * w1 + va[2] * w2;
            if (!d->fst && q != 0) { uu /= q; vv /= q; }
            u16 = (int)uu; v16 = (int)vv;
        }
        int f = (int)(v0->f * w0 + v1->f * w1 + v2->f * w2);
        pixel(d, x, y, z, shade(d, col, u16, v16, f));
    }
}
#if defined(__x86_64__)
/* ---- AVX2 pixel pipeline for triangles: 8 pixels of one row at a time, lane for lane the same integer/float
   operations as texel()/sample()/shade()/pixel(). Covers 32-bit frame buffers, Z32/Z24, CT32/CT24/T8/T8H/T4/T4HL/T4HH
   textures (nearest or bilinear), all alpha tests and blend equations; anything else uses the scalar path. */
#define V8 __attribute__((target("avx2"))) static inline
static int32_t bt8_i[4][8], ct8_i[16][16], bt4_i[8][4], ct4_i[16][32], bt16_i[8][4], bt16s_i[8][4], bt16z_i[8][4], ct16_i[8][16];
V8 __m256i v_set(int32_t x) { return _mm256_set1_epi32(x); }
V8 __m256i v_addr32(uint32_t bp, uint32_t bw, __m256i x, __m256i y, const uint32_t (*tab)[2048]) {
    __m256i idx = _mm256_or_si256(_mm256_slli_epi32(_mm256_and_si256(y, v_set(31)), 11), _mm256_and_si256(x, v_set(2047)));
    __m256i o = _mm256_i32gather_epi32((const int *)tab, idx, 4);
    __m256i base = _mm256_add_epi32(v_set((int)(bp * 64)), _mm256_mullo_epi32(_mm256_srai_epi32(y, 5), v_set((int)(bw * 2048))));
    return _mm256_and_si256(_mm256_add_epi32(base, o), v_set(0xFFFFF));
}
V8 __m256i v_addr8(uint32_t bp, uint32_t bw, __m256i x, __m256i y) {
    __m256i page = _mm256_add_epi32(_mm256_srai_epi32(x, 7), _mm256_mullo_epi32(_mm256_srai_epi32(y, 6), v_set((int)((bw + 1) >> 1))));
    __m256i bi = _mm256_or_si256(_mm256_slli_epi32(_mm256_and_si256(_mm256_srai_epi32(y, 4), v_set(3)), 3), _mm256_and_si256(_mm256_srai_epi32(x, 4), v_set(7)));
    __m256i ci = _mm256_or_si256(_mm256_slli_epi32(_mm256_and_si256(y, v_set(15)), 4), _mm256_and_si256(x, v_set(15)));
    __m256i blk = _mm256_add_epi32(_mm256_add_epi32(v_set((int)bp), _mm256_slli_epi32(page, 5)), _mm256_i32gather_epi32((const int *)bt8_i, bi, 4));
    return _mm256_and_si256(_mm256_add_epi32(_mm256_slli_epi32(blk, 8), _mm256_i32gather_epi32((const int *)ct8_i, ci, 4)), v_set(0x3FFFFF));
}
V8 __m256i v_addr4(uint32_t bp, uint32_t bw, __m256i x, __m256i y) {
    __m256i page = _mm256_add_epi32(_mm256_srai_epi32(x, 7), _mm256_mullo_epi32(_mm256_srai_epi32(y, 7), v_set((int)((bw + 1) >> 1))));
    __m256i bi = _mm256_or_si256(_mm256_slli_epi32(_mm256_and_si256(_mm256_srai_epi32(y, 4), v_set(7)), 2), _mm256_and_si256(_mm256_srai_epi32(x, 5), v_set(3)));
    __m256i ci = _mm256_or_si256(_mm256_slli_epi32(_mm256_and_si256(y, v_set(15)), 5), _mm256_and_si256(x, v_set(31)));
    __m256i blk = _mm256_add_epi32(_mm256_add_epi32(v_set((int)bp), _mm256_slli_epi32(page, 5)), _mm256_i32gather_epi32((const int *)bt4_i, bi, 4));
    return _mm256_and_si256(_mm256_add_epi32(_mm256_slli_epi32(blk, 9), _mm256_i32gather_epi32((const int *)ct4_i, ci, 4)), v_set(0x7FFFFF));
}
/* halfword index of a 16-bit pixel, as addr16() */
V8 __m256i v_addr16(uint32_t bp, uint32_t bw, __m256i x, __m256i y, const int32_t (*bt)[4]) {
    __m256i page = _mm256_add_epi32(_mm256_srai_epi32(x, 6), _mm256_mullo_epi32(_mm256_srai_epi32(y, 6), v_set((int)bw)));
    __m256i bi = _mm256_or_si256(_mm256_slli_epi32(_mm256_and_si256(_mm256_srai_epi32(y, 3), v_set(7)), 2), _mm256_and_si256(_mm256_srai_epi32(x, 4), v_set(3)));
    __m256i ci = _mm256_or_si256(_mm256_slli_epi32(_mm256_and_si256(y, v_set(7)), 4), _mm256_and_si256(x, v_set(15)));
    __m256i blk = _mm256_add_epi32(_mm256_add_epi32(v_set((int)bp), _mm256_slli_epi32(page, 5)), _mm256_i32gather_epi32((const int *)bt, bi, 4));
    return _mm256_and_si256(_mm256_add_epi32(_mm256_slli_epi32(blk, 7), _mm256_i32gather_epi32((const int *)ct16_i, ci, 4)), v_set(0x1FFFFF));
}
V8 __m256i v_read16(__m256i hidx) { return _mm256_and_si256(_mm256_i32gather_epi32((const int *)VM16, hidx, 2), v_set(0xFFFF)); }
V8 __m256i v_c16to32(__m256i c, int aem, int ta0, int ta1) {   /* c16to32() */
    const __m256i f = v_set(31);
    __m256i o = _mm256_or_si256(_mm256_or_si256(_mm256_slli_epi32(_mm256_and_si256(c, f), 3), _mm256_slli_epi32(_mm256_and_si256(_mm256_srli_epi32(c, 5), f), 11)),
                                _mm256_slli_epi32(_mm256_and_si256(_mm256_srli_epi32(c, 10), f), 19));
    const __m256i hi = _mm256_cmpeq_epi32(_mm256_and_si256(c, v_set(0x8000)), v_set(0x8000));
    __m256i a = v_set(ta0);
    if (aem) a = _mm256_andnot_si256(_mm256_cmpeq_epi32(_mm256_and_si256(c, v_set(0x7FFF)), _mm256_setzero_si256()), a);
    a = _mm256_blendv_epi8(a, v_set(ta1), hi);
    return _mm256_or_si256(o, _mm256_slli_epi32(a, 24));
}
V8 __m256i v_c32to16(__m256i c) {   /* c32to16() */
    const __m256i f = v_set(31);
    return _mm256_or_si256(_mm256_or_si256(_mm256_and_si256(_mm256_srli_epi32(c, 3), f), _mm256_slli_epi32(_mm256_and_si256(_mm256_srli_epi32(c, 11), f), 5)),
                           _mm256_or_si256(_mm256_slli_epi32(_mm256_and_si256(_mm256_srli_epi32(c, 19), f), 10), _mm256_and_si256(_mm256_srli_epi32(c, 16), v_set(0x8000))));
}
V8 __m256i v_texel(const TexState *t, __m256i u, __m256i v) {
    u = wrap8(u, t->tw, t->wms, t->minu, t->maxu); v = wrap8(v, t->th, t->wmt, t->minv, t->maxv);
    __m256i c, idx;
    switch (t->psm) {
    case CT32: return _mm256_i32gather_epi32((const int *)vm, v_addr32(t->tbp, t->tbw, u, v, xoff32), 4);
    case CT24: {
        c = _mm256_and_si256(_mm256_i32gather_epi32((const int *)vm, v_addr32(t->tbp, t->tbw, u, v, xoff32), 4), v_set(0xFFFFFF));
        __m256i a = v_set((int)((uint32_t)t->ta0 << 24));
        if (t->aem) a = _mm256_andnot_si256(_mm256_cmpeq_epi32(c, _mm256_setzero_si256()), a);
        return _mm256_or_si256(c, a); }
    case CT16: return v_c16to32(v_read16(v_addr16(t->tbp, t->tbw, u, v, bt16_i)), t->aem, t->ta0, t->ta1);
    case CT16S: return v_c16to32(v_read16(v_addr16(t->tbp, t->tbw, u, v, bt16s_i)), t->aem, t->ta0, t->ta1);
    case T8: idx = _mm256_and_si256(_mm256_i32gather_epi32((const int *)VM8, v_addr8(t->tbp, t->tbw, u, v), 1), v_set(255)); break;
    case T8H: idx = _mm256_srli_epi32(_mm256_i32gather_epi32((const int *)vm, v_addr32(t->tbp, t->tbw, u, v, xoff32), 4), 24); break;
    case T4: { __m256i a = v_addr4(t->tbp, t->tbw, u, v);
        __m256i byte = _mm256_i32gather_epi32((const int *)VM8, _mm256_srli_epi32(a, 1), 1);
        idx = _mm256_and_si256(_mm256_srlv_epi32(byte, _mm256_slli_epi32(_mm256_and_si256(a, v_set(1)), 2)), v_set(15)); break; }
    case T4HL: idx = _mm256_and_si256(_mm256_srli_epi32(_mm256_i32gather_epi32((const int *)vm, v_addr32(t->tbp, t->tbw, u, v, xoff32), 4), 24), v_set(15)); break;
    default: /* T4HH */ idx = _mm256_srli_epi32(_mm256_i32gather_epi32((const int *)vm, v_addr32(t->tbp, t->tbw, u, v, xoff32), 4), 28); break;
    }
    if (t->psm == T4 || t->psm == T4HL || t->psm == T4HH)
        idx = _mm256_and_si256(_mm256_add_epi32(v_set((int)((t->cpsm == CT32 ? (t->csa & 15) : t->csa) * 16)), idx), v_set(255));
    return _mm256_i32gather_epi32((const int *)t->clut, idx, 4);
}
V8 __m256i v_sample(const TexState *t, __m256i u16, __m256i v16) {
    if (!t->bilinear) return v_texel(t, _mm256_srai_epi32(u16, 4), _mm256_srai_epi32(v16, 4));
    const __m256i uu = _mm256_sub_epi32(u16, v_set(8)), vv = _mm256_sub_epi32(v16, v_set(8));
    const __m256i fu = _mm256_and_si256(uu, v_set(15)), fv = _mm256_and_si256(vv, v_set(15)), ui = _mm256_srai_epi32(uu, 4), vi = _mm256_srai_epi32(vv, 4);
    const __m256i ui1 = _mm256_add_epi32(ui, v_set(1)), vi1 = _mm256_add_epi32(vi, v_set(1));
    const __m256i c00 = v_texel(t, ui, vi), c10 = v_texel(t, ui1, vi), c01 = v_texel(t, ui, vi1), c11 = v_texel(t, ui1, vi1);
    const __m256i gu = _mm256_sub_epi32(v_set(16), fu), gv = _mm256_sub_epi32(v_set(16), fv);
    const __m256i w00 = _mm256_mullo_epi32(gu, gv), w10 = _mm256_mullo_epi32(fu, gv), w01 = _mm256_mullo_epi32(gu, fv), w11 = _mm256_mullo_epi32(fu, fv), m = v_set(0xFF00FF);
#define WSUM(sh) _mm256_add_epi32(_mm256_add_epi32(_mm256_mullo_epi32(_mm256_and_si256(_mm256_srli_epi32(c00, sh), m), w00), _mm256_mullo_epi32(_mm256_and_si256(_mm256_srli_epi32(c10, sh), m), w10)), \
                                  _mm256_add_epi32(_mm256_mullo_epi32(_mm256_and_si256(_mm256_srli_epi32(c01, sh), m), w01), _mm256_mullo_epi32(_mm256_and_si256(_mm256_srli_epi32(c11, sh), m), w11)))
    const __m256i rb = WSUM(0), ag = WSUM(8);
#undef WSUM
    return _mm256_or_si256(_mm256_and_si256(_mm256_srli_epi32(rb, 8), m), _mm256_slli_epi32(_mm256_and_si256(_mm256_srli_epi32(ag, 8), m), 8));
}
V8 __m256i v_shade(const Draw *d, __m256i vcol, __m256i u16, __m256i v16, __m256i fog) {
    __m256i c = vcol;
    const __m256i m255 = v_set(255);
    if (d->tme) {
        const __m256i t = v_sample(&d->tex, u16, v16), ta = _mm256_srli_epi32(t, 24), fa = _mm256_srli_epi32(vcol, 24);
        const int tfx = d->tex.tfx, tcc = d->tex.tcc;
        __m256i out = _mm256_setzero_si256();
        for (int s = 0; s < 24; s += 8) {
            const __m256i ct = _mm256_and_si256(_mm256_srli_epi32(t, s), m255), cf = _mm256_and_si256(_mm256_srli_epi32(vcol, s), m255);
            __m256i v;
            if (tfx == 1) v = ct;
            else { v = _mm256_srai_epi32(_mm256_mullo_epi32(ct, cf), 7); if (tfx != 0) v = _mm256_add_epi32(v, fa); }
            out = _mm256_or_si256(out, _mm256_slli_epi32(_mm256_min_epi32(v, m255), s));
        }
        __m256i a;
        if (!tcc) a = fa; else if (tfx == 0) a = _mm256_srai_epi32(_mm256_mullo_epi32(ta, fa), 7); else if (tfx == 2) a = _mm256_add_epi32(ta, fa); else a = ta;
        c = _mm256_or_si256(out, _mm256_slli_epi32(_mm256_min_epi32(a, m255), 24));
    }
    if (d->fge) {   /* unsigned arithmetic as in shade() */
        const uint32_t fc = d->fogcol; const __m256i nf = _mm256_sub_epi32(m255, fog);
        __m256i out = _mm256_and_si256(c, v_set((int)0xFF000000u));
        for (int s = 0; s < 24; s += 8) {
            __m256i x = _mm256_add_epi32(_mm256_mullo_epi32(_mm256_and_si256(_mm256_srli_epi32(c, s), m255), fog), _mm256_mullo_epi32(v_set((int)((fc >> s) & 255)), nf));
            out = _mm256_or_si256(out, _mm256_slli_epi32(_mm256_srli_epi32(x, 8), s));
        }
        c = out;
    }
    return c;
}
V8 __m256i v_ucmplt(__m256i a, __m256i b) { const __m256i k = v_set((int)0x80000000u); return _mm256_cmpgt_epi32(_mm256_xor_si256(b, k), _mm256_xor_si256(a, k)); }
/* the per-pixel tests and writes of pixel() for the lanes in `live` (x = xb + lane, row y; scissor already applied) */
V8 void v_pixel(const Draw *d, int xb, int y, __m256i live, __m256i z, __m256i col) {
    const __m256i ones = _mm256_set1_epi32(-1), m255 = v_set(255), zero = _mm256_setzero_si256();
    const __m256i as = _mm256_srli_epi32(col, 24);
    __m256i fbok = ones, zok = ones, rgbonly = zero;
    if (d->ate) {
        const __m256i ar = v_set(d->aref); __m256i pass;
        switch (d->atst) {
        case 0: pass = zero; break; case 1: pass = ones; break;
        case 2: pass = _mm256_cmpgt_epi32(ar, as); break; case 3: pass = _mm256_xor_si256(_mm256_cmpgt_epi32(as, ar), ones); break;
        case 4: pass = _mm256_cmpeq_epi32(as, ar); break; case 5: pass = _mm256_xor_si256(_mm256_cmpgt_epi32(ar, as), ones); break;
        case 6: pass = _mm256_cmpgt_epi32(as, ar); break; default: pass = _mm256_xor_si256(_mm256_cmpeq_epi32(as, ar), ones);
        }
        switch (d->afail) {
        case 0: live = _mm256_and_si256(live, pass); break;
        case 1: zok = pass; break;
        case 2: fbok = pass; break;
        default: zok = pass; rgbonly = _mm256_xor_si256(pass, ones);
        }
    }
    if (_mm256_testz_si256(live, live)) return;
    __m256i fa, dst, dst16 = zero;
    if (d->fb16) {   /* fa: halfword index; dst: the pixel as 32-bit color (c16to32 with TA1 = 0x80) */
        fa = v_addr16(d->fbp, d->fbw, _mm256_add_epi32(v_set(xb), _mm256_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7)), v_set(y), d->fpsm == CT16S ? bt16s_i : bt16_i);
        dst16 = v_read16(fa); dst = v_c16to32(dst16, 0, 0, 0x80);
    } else {
        fa = _mm256_and_si256(_mm256_add_epi32(_mm256_loadu_si256((const __m256i *)(xoff32[y & 31] + (xb & 2047))), v_set((int)(d->fbp * 64 + (uint32_t)(y >> 5) * d->fbw * 2048))), v_set(0xFFFFF));
        dst = d->need_dst ? _mm256_i32gather_epi32((const int *)vm, fa, 4) : zero;
    }
    __m256i za = zero;
    if (d->zte) {
        za = _mm256_and_si256(_mm256_add_epi32(_mm256_loadu_si256((const __m256i *)(xoff32z[y & 31] + (xb & 2047))), v_set((int)(d->zbp * 64 + (uint32_t)(y >> 5) * d->fbw * 2048))), v_set(0xFFFFF));
        if (d->zread) {
            __m256i zd = _mm256_i32gather_epi32((const int *)vm, za, 4);
            if (d->zpsm == Z24) zd = _mm256_and_si256(zd, v_set(0xFFFFFF));
            const __m256i fail = d->ztst == 2 ? v_ucmplt(z, zd) : _mm256_xor_si256(v_ucmplt(zd, z), ones);   /* GEQUAL fails if z < zd; GREATER fails if z <= zd */
            live = _mm256_andnot_si256(fail, live);
        }
    } else zok = zero;
    __m256i out = col;
    if (d->abe) {
        const __m256i ad = _mm256_srli_epi32(dst, 24), cf = d->cc == 0 ? as : d->cc == 1 ? ad : v_set(d->fix);
        __m256i o = _mm256_and_si256(col, v_set((int)0xFF000000u));
        for (int s = 0; s < 24; s += 8) {
            const __m256i cs = _mm256_and_si256(_mm256_srli_epi32(col, s), m255), cd = _mm256_and_si256(_mm256_srli_epi32(dst, s), m255);
            const __m256i A = d->a == 0 ? cs : d->a == 1 ? cd : zero, B = d->b == 0 ? cs : d->b == 1 ? cd : zero, D = d->d == 0 ? cs : d->d == 1 ? cd : zero;
            __m256i v = _mm256_add_epi32(_mm256_srai_epi32(_mm256_mullo_epi32(_mm256_sub_epi32(A, B), cf), 7), D);
            v = d->colclamp ? _mm256_min_epi32(_mm256_max_epi32(v, zero), m255) : _mm256_and_si256(v, m255);
            o = _mm256_or_si256(o, _mm256_slli_epi32(v, s));
        }
        if (d->pabe) o = _mm256_blendv_epi8(col, o, _mm256_cmpeq_epi32(_mm256_and_si256(as, v_set(0x80)), v_set(0x80)));
        out = o;
    }
    if (d->fba) out = _mm256_or_si256(out, v_set((int)0x80000000u));
    out = _mm256_blendv_epi8(out, _mm256_or_si256(_mm256_and_si256(out, v_set(0xFFFFFF)), _mm256_and_si256(dst, v_set((int)0xFF000000u))), rgbonly);
    const __m256i fbm = v_set((int)d->fbmsk);
    if (d->fb16) out = _mm256_or_si256(_mm256_andnot_si256(fbm, v_c32to16(out)), _mm256_and_si256(dst16, _mm256_and_si256(fbm, v_set(0xFFFF))));
    else out = _mm256_or_si256(_mm256_andnot_si256(fbm, out), _mm256_and_si256(dst, fbm));
    if (d->fpsm == CT24) out = _mm256_or_si256(_mm256_and_si256(dst, v_set((int)0xFF000000u)), _mm256_and_si256(out, v_set(0xFFFFFF)));
    const int mfb = _mm256_movemask_ps(_mm256_castsi256_ps(_mm256_and_si256(live, fbok)));
    const int mz = (d->zte && !d->zmsk) ? _mm256_movemask_ps(_mm256_castsi256_ps(_mm256_and_si256(live, zok))) : 0;
    uint32_t oa[8], fa_[8], za_[8], zv[8];
    _mm256_storeu_si256((__m256i *)oa, out); _mm256_storeu_si256((__m256i *)fa_, fa); _mm256_storeu_si256((__m256i *)za_, za); _mm256_storeu_si256((__m256i *)zv, z);
    for (int i = 0; i < 8; i++) {   /* lane order: fb write then z write, as pixel() does per pixel */
        if (mfb >> i & 1) { if (d->fb16) VM16[fa_[i]] = (uint16_t)oa[i]; else vm[fa_[i]] = oa[i]; n_pixels++; }
        if (mz >> i & 1) { uint32_t *zp = &vm[za_[i]]; *zp = d->zpsm == Z24 ? (*zp & 0xFF000000u) | (zv[i] & 0xFFFFFF) : zv[i]; }
    }
}
static int ranges_overlap(uint32_t a0, uint32_t a1, uint32_t b0, uint32_t b1) { return a0 < b1 && b0 < a1; }
static uint64_t st_scalar[4];   /* draws left to the scalar path: format, frame range, Z overlap, texture overlap */
/* 8 KB pages (2048 words) touched by the pixel rectangle [x0,x1]x[y0,y1] of a buffer at block bp, width bw, format psm;
   a buffer not starting on a page boundary also touches the following page */
static void mark_pages(uint64_t pages[8], uint32_t bp, uint32_t bw, int psm, int x0, int y0, int x1, int y1) {
    int pw, ph; uint32_t stride;
    switch (psm) {
    case CT32: case CT24: case Z32: case Z24: case T8H: case T4HL: case T4HH: pw = 64; ph = 32; stride = bw; break;
    case CT16: case CT16S: case Z16: case Z16S: pw = 64; ph = 64; stride = bw; break;
    case T8: pw = 128; ph = 64; stride = (bw + 1) >> 1; break;
    default: pw = 128; ph = 128; stride = (bw + 1) >> 1; break;
    }
    const uint32_t base = bp >> 5, extra = (bp & 31) != 0;
    for (int py = y0 / ph; py <= y1 / ph; py++) for (int px = x0 / pw; px <= x1 / pw; px++)
        for (uint32_t k = 0; k <= extra; k++) { uint32_t p = (base + (uint32_t)py * stride + (uint32_t)px + k) & 511; pages[p >> 6] |= 1ull << (p & 63); }
}
static int pages_meet(const uint64_t a[8], const uint64_t b[8]) { for (int i = 0; i < 8; i++) if (a[i] & b[i]) return 1; return 0; }
/* texel range [*a,*b] after wrap(): exact for the clamp modes; returns 0 (use the whole texture) for repeat modes */
static int clamp_range(int *a, int *b, int size, int mode, int mn, int mx) {
    if (mode == 1) { *a = wrap(*a, size, 1, 0, 0); *b = wrap(*b, size, 1, 0, 0); return 1; }
    if (mode == 2 && mn <= mx) { *a = wrap(*a, size, 2, mn, mx); *b = wrap(*b, size, 2, mn, mx); return 1; }
    return 0;
}
/* Can the sprite read a texel that it also writes as a frame pixel? The texel bounds come from the same per-pixel
   expressions the renderers use (u at the first and last pixel column, v at the first and last row). Same buffer and
   format: exact pixel rectangles; otherwise 8 KB pages. Wrapping modes other than clamps use the whole texture. */
/* the texels [*ua,*ub] x [*va,*vb] a sprite can read (after wrap); 0 if it draws nothing */
static int sprite_texels(const Draw *d, int px0, int px1, int py0, int py1, int x0, int y0, float u0, float v0, float du, float dv, int *pua, int *pub, int *pva, int *pvb) {
    const TexState *t = &d->tex;
    if (px1 <= px0 || py1 <= py0) return 0;
    const int uf = (int)(u0 + ((px0 << 4) - x0) * du), ul = (int)(u0 + (((px1 - 1) << 4) - x0) * du);
    const int vf = (int)(v0 + ((py0 << 4) - y0) * dv), vl = (int)(v0 + (((py1 - 1) << 4) - y0) * dv);
    const int umn = uf < ul ? uf : ul, umx = uf < ul ? ul : uf, vmn = vf < vl ? vf : vl, vmx = vf < vl ? vl : vf;
    /* u(x) is monotonic in x (and v in y), so the end pixels bound every texel: nearest reads u >> 4; bilinear reads
       (u - 8) >> 4 and, when the fraction is non-zero, the next texel (sample() skips taps with zero weight) */
    int ua, ub, va, vb;
    if (t->bilinear) { ua = (umn - 8) >> 4; ub = ((umx - 8) >> 4) + (((umx - 8) & 15) != 0); va = (vmn - 8) >> 4; vb = ((vmx - 8) >> 4) + (((vmx - 8) & 15) != 0); }
    else { ua = umn >> 4; ub = umx >> 4; va = vmn >> 4; vb = vmx >> 4; }
    if (!clamp_range(&ua, &ub, t->tw, t->wms, t->minu, t->maxu) || !clamp_range(&va, &vb, t->th, t->wmt, t->minv, t->maxv)) { ua = 0; ub = t->tw - 1; va = 0; vb = t->th - 1; }
    if (ua > ub || va > vb) return 0;
    *pua = ua; *pub = ub; *pva = va; *pvb = vb; return 1;
}
static int sprite_tex_overlaps(const Draw *d, int px0, int px1, int py0, int py1, int x0, int y0, float u0, float v0, float du, float dv) {
    const TexState *t = &d->tex; int ua, ub, va, vb;
    if (!sprite_texels(d, px0, px1, py0, py1, x0, y0, u0, v0, du, dv, &ua, &ub, &va, &vb)) return 0;
    if (t->tbp == d->fbp && t->tbw == d->fbw && t->psm == d->fpsm) {
        return ua <= px1 - 1 && px0 <= ub && va <= py1 - 1 && py0 <= vb; }
    uint64_t fp[8] = {0}, tp[8] = {0};
    mark_pages(fp, d->fbp, d->fbw, d->fpsm, px0, py0, px1 - 1, py1 - 1);
    mark_pages(tp, t->tbp, t->tbw, t->psm, ua, va, ub, vb);
    return pages_meet(fp, tp);
}
static int tri_simd_ok_ex(const Draw *d, int tex_checked);
static int tri_simd_ok(const Draw *d) { return tri_simd_ok_ex(d, 0); }
static int tri_simd_ok_ex(const Draw *d, int tex_checked) {
    static int off = -1; if (off < 0) off = getenv("HG_TRISIMD") && !strcmp(getenv("HG_TRISIMD"), "0");
    if (off) return 0;
    if ((d->fpsm != CT32 && d->fpsm != CT24 && d->fpsm != CT16 && d->fpsm != CT16S) || d->date) { st_scalar[0]++; return 0; }
    if (d->zte && d->ztst != 0 && (d->zread || !d->zmsk) && d->zpsm != Z32 && d->zpsm != Z24) { st_scalar[0]++; return 0; }
    const uint32_t fb0 = d->fbp * 64, fb1 = fb0 + d->fbw * 2048 * ((uint32_t)(d->scy1 >> 5) + 1);
    if (fb1 > (1u << 20)) { st_scalar[1]++; return 0; }
    if (d->zte && d->ztst != 0 && ranges_overlap(fb0, fb1, d->zbp * 64, d->zbp * 64 + d->fbw * 2048 * ((uint32_t)(d->scy1 >> 5) + 1))) { st_scalar[2]++; return 0; }
    if (d->tme) {
        switch (d->tex.psm) { case CT32: case CT24: case CT16: case CT16S: case T8: case T8H: case T4: case T4HL: case T4HH: break; default: st_scalar[0]++; return 0; }
        const uint32_t t0 = d->tex.tbp * 64, t1 = t0 + (d->tex.tbw ? d->tex.tbw : 1) * 2048 * ((d->tex.th >> 5) + 2);
        if (!tex_checked && ranges_overlap(fb0, fb1, t0, t1)) { st_scalar[3]++; return 0; }
    }
    return 1;
}
/* draw_triangle with the coverage test done for 8 pixels at a time (same float operations per lane, no FMA) and
   chunks outside a conservative per-row span skipped; covered pixels are interpolated and drawn by the same scalar
   code as draw_triangle, so the output is identical. */
__attribute__((target("avx2"))) static void draw_triangle_avx2(const Draw *d, const Vtx *v0, const Vtx *v1, const Vtx *v2) {
    float x0 = v0->x / 16.0f, y0 = v0->y / 16.0f, x1 = v1->x / 16.0f, y1 = v1->y / 16.0f, x2 = v2->x / 16.0f, y2 = v2->y / 16.0f;
    float area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);
    if (area == 0) return;
    int minx = (int)floorf(fminf(x0, fminf(x1, x2))), maxx = (int)ceilf(fmaxf(x0, fmaxf(x1, x2)));
    int miny = (int)floorf(fminf(y0, fminf(y1, y2))), maxy = (int)ceilf(fmaxf(y0, fmaxf(y1, y2)));
    if (minx < d->scx0) minx = d->scx0; if (maxx > d->scx1) maxx = d->scx1;
    if (miny < d->scy0) miny = d->scy0; if (maxy > d->scy1) maxy = d->scy1;
    float inv = 1.0f / area;
    float ua[3] = {0}, va[3] = {0}, qa[3] = {1, 1, 1};
    const Vtx *vs[3] = { v0, v1, v2 };
    for (int i = 0; i < 3; i++) {
        if (!d->tme) continue;
        if (d->fst) { ua[i] = vs[i]->u; va[i] = vs[i]->v; qa[i] = 1; }
        else { ua[i] = vs[i]->s * d->tex.tw * 16.0f; va[i] = vs[i]->t * d->tex.th * 16.0f; qa[i] = vs[i]->q != 0 ? vs[i]->q : 1.0f; }
    }
    const __m256 X0 = _mm256_set1_ps(x0), X1 = _mm256_set1_ps(x1), X2 = _mm256_set1_ps(x2), INV = _mm256_set1_ps(inv), ONE = _mm256_set1_ps(1.0f), EPS = _mm256_set1_ps(-1e-6f);
    const __m256i lane = _mm256_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7), bitsel = _mm256_setr_epi32(1, 2, 4, 8, 16, 32, 64, 128);
    const int simd = tri_simd_ok(d), znever = d->zte && d->ztst == 0;
    /* barycentric i at (px, py) is (A_i + B_i * px) * inv in exact arithmetic: used only for the conservative span */
    const double B0 = (double)y1 - y2, B1 = (double)y2 - y0;
    for (int y = miny; y <= maxy; y++) {
        if (!ROW_MINE(y)) continue;
        const float py = (float)y;
        const __m256 PY = _mm256_set1_ps(py), Y0 = _mm256_set1_ps(y0 - py), Y1 = _mm256_set1_ps(y1 - py), Y2 = _mm256_set1_ps(y2 - py);
        double lo = minx, hi = maxx;
        const double A0 = (double)x1 * (y2 - (double)py) - (double)x2 * (y1 - (double)py), A1 = (double)x2 * (y0 - (double)py) - (double)x0 * (y2 - (double)py);
        const double As[3] = { A0, A1, (double)area - A0 - A1 }, Bs[3] = { B0, B1, -(B0 + B1) };   /* w2 = 1 - w0 - w1 */
        for (int i = 0; i < 3; i++) {   /* w_i >= 0  <=>  sign(area) * (A_i + B_i px) >= 0 */
            double a = area > 0 ? As[i] : -As[i], bb = area > 0 ? Bs[i] : -Bs[i];
            if (bb > 1e-12) { double t = -a / bb; if (t > lo) lo = t; }
            else if (bb < -1e-12) { double t = -a / bb; if (t < hi) hi = t; }
        }
        int xs = (int)floor(lo) - 2, xe = (int)ceil(hi) + 2;
        if (xs < minx) xs = minx; if (xe > maxx) xe = maxx;
        for (int xb = xs; xb <= xe; xb += 8) {
            const __m256 px = _mm256_cvtepi32_ps(_mm256_add_epi32(_mm256_set1_epi32(xb), lane));
            /* scalar: (x1 - px) * (y2 - py) - (x2 - px) * (y1 - py), where (y2 - py) etc. are per-row constants */
            const __m256 w0 = _mm256_mul_ps(_mm256_sub_ps(_mm256_mul_ps(_mm256_sub_ps(X1, px), Y2), _mm256_mul_ps(_mm256_sub_ps(X2, px), Y1)), INV);
            const __m256 w1 = _mm256_mul_ps(_mm256_sub_ps(_mm256_mul_ps(_mm256_sub_ps(X2, px), Y0), _mm256_mul_ps(_mm256_sub_ps(X0, px), Y2)), INV);
            const __m256 w2 = _mm256_sub_ps(_mm256_sub_ps(ONE, w0), w1);
            __m256 ok = _mm256_and_ps(_mm256_and_ps(_mm256_cmp_ps(w0, EPS, _CMP_NLT_UQ), _mm256_cmp_ps(w1, EPS, _CMP_NLT_UQ)), _mm256_cmp_ps(w2, EPS, _CMP_NLT_UQ));
            int m = _mm256_movemask_ps(ok);
            if (xe - xb < 7) m &= (1 << (xe - xb + 1)) - 1;
            if (!m) continue;
            if (simd) {
                const __m256i live = _mm256_cmpgt_epi32(_mm256_and_si256(_mm256_set1_epi32(m), bitsel), _mm256_setzero_si256());
                n_visits += __builtin_popcount(m);
                if (znever) continue;
                __m256i col;
                if (d->iip) {
                    col = _mm256_setzero_si256();
                    for (int s = 0; s < 32; s += 8) {
                        const __m256 c = _mm256_add_ps(_mm256_add_ps(_mm256_mul_ps(_mm256_set1_ps((float)((v0->rgba >> s) & 255)), w0), _mm256_mul_ps(_mm256_set1_ps((float)((v1->rgba >> s) & 255)), w1)),
                                                       _mm256_mul_ps(_mm256_set1_ps((float)((v2->rgba >> s) & 255)), w2));
                        __m256i ci = _mm256_cvttps_epi32(_mm256_add_ps(c, _mm256_set1_ps(0.5f)));
                        ci = _mm256_min_epi32(_mm256_max_epi32(ci, _mm256_setzero_si256()), _mm256_set1_epi32(255));
                        col = _mm256_or_si256(col, _mm256_slli_epi32(ci, s));
                    }
                } else col = _mm256_set1_epi32((int)v2->rgba);
                __m128i zh[2];
                for (int h = 0; h < 2; h++) {
                    const __m256d a0 = _mm256_cvtps_pd(h ? _mm256_extractf128_ps(w0, 1) : _mm256_castps256_ps128(w0));
                    const __m256d a1 = _mm256_cvtps_pd(h ? _mm256_extractf128_ps(w1, 1) : _mm256_castps256_ps128(w1));
                    const __m256d a2 = _mm256_cvtps_pd(h ? _mm256_extractf128_ps(w2, 1) : _mm256_castps256_ps128(w2));
                    const __m256d zf = _mm256_add_pd(_mm256_add_pd(_mm256_mul_pd(_mm256_set1_pd((double)v0->z), a0), _mm256_mul_pd(_mm256_set1_pd((double)v1->z), a1)), _mm256_mul_pd(_mm256_set1_pd((double)v2->z), a2));
                    const __m256d two31 = _mm256_set1_pd(2147483648.0);
                    const __m128i lo = _mm256_cvttpd_epi32(zf), hi = _mm_add_epi32(_mm256_cvttpd_epi32(_mm256_sub_pd(zf, two31)), _mm_set1_epi32((int)0x80000000u));
                    __m128i zi = _mm_blendv_epi8(lo, hi, _mm256_cvtpd_epi32(_mm256_and_pd(_mm256_cmp_pd(zf, two31, _CMP_GE_OQ), _mm256_set1_pd(-1.0))));
                    const __m128i le0 = _mm256_cvtpd_epi32(_mm256_and_pd(_mm256_cmp_pd(zf, _mm256_setzero_pd(), _CMP_LE_OQ), _mm256_set1_pd(-1.0)));
                    const __m128i gemax = _mm256_cvtpd_epi32(_mm256_and_pd(_mm256_cmp_pd(zf, _mm256_set1_pd(4294967295.0), _CMP_GE_OQ), _mm256_set1_pd(-1.0)));
                    zi = _mm_andnot_si128(le0, zi); zi = _mm_or_si128(zi, gemax);
                    zh[h] = zi;
                }
                const __m256i zv = _mm256_set_m128i(zh[1], zh[0]);
                __m256i u16 = _mm256_setzero_si256(), v16 = _mm256_setzero_si256();
                if (d->tme) {
                    const __m256 q = _mm256_add_ps(_mm256_add_ps(_mm256_mul_ps(_mm256_set1_ps(qa[0]), w0), _mm256_mul_ps(_mm256_set1_ps(qa[1]), w1)), _mm256_mul_ps(_mm256_set1_ps(qa[2]), w2));
                    __m256 uu = _mm256_add_ps(_mm256_add_ps(_mm256_mul_ps(_mm256_set1_ps(ua[0]), w0), _mm256_mul_ps(_mm256_set1_ps(ua[1]), w1)), _mm256_mul_ps(_mm256_set1_ps(ua[2]), w2));
                    __m256 vv = _mm256_add_ps(_mm256_add_ps(_mm256_mul_ps(_mm256_set1_ps(va[0]), w0), _mm256_mul_ps(_mm256_set1_ps(va[1]), w1)), _mm256_mul_ps(_mm256_set1_ps(va[2]), w2));
                    if (!d->fst) {
                        const __m256 nz = _mm256_cmp_ps(q, _mm256_setzero_ps(), _CMP_NEQ_UQ);
                        uu = _mm256_blendv_ps(uu, _mm256_div_ps(uu, q), nz); vv = _mm256_blendv_ps(vv, _mm256_div_ps(vv, q), nz);
                    }
                    u16 = _mm256_cvttps_epi32(uu); v16 = _mm256_cvttps_epi32(vv);
                }
                __m256i fog = _mm256_setzero_si256();
                if (d->fge) fog = _mm256_cvttps_epi32(_mm256_add_ps(_mm256_add_ps(_mm256_mul_ps(_mm256_set1_ps((float)v0->f), w0), _mm256_mul_ps(_mm256_set1_ps((float)v1->f), w1)), _mm256_mul_ps(_mm256_set1_ps((float)v2->f), w2)));
                v_pixel(d, xb, y, live, zv, v_shade(d, col, u16, v16, fog));
                continue;
            }
            float W0[8], W1[8], W2[8]; _mm256_storeu_ps(W0, w0); _mm256_storeu_ps(W1, w1); _mm256_storeu_ps(W2, w2);
            while (m) {
                const int i = __builtin_ctz(m); m &= m - 1;
                const int x = xb + i; const float fw0 = W0[i], fw1 = W1[i], fw2 = W2[i];
                uint32_t col;
                if (d->iip) {
                    col = 0;
                    for (int s = 0; s < 32; s += 8) {
                        float c = ((v0->rgba >> s) & 255) * fw0 + ((v1->rgba >> s) & 255) * fw1 + ((v2->rgba >> s) & 255) * fw2;
                        int ci = (int)(c + 0.5f); col |= (uint32_t)(ci < 0 ? 0 : ci > 255 ? 255 : ci) << s;
                    }
                } else col = v2->rgba;
                double zf = (double)v0->z * fw0 + (double)v1->z * fw1 + (double)v2->z * fw2;
                uint32_t z = zf <= 0 ? 0 : zf >= 4294967295.0 ? 0xFFFFFFFFu : (uint32_t)zf;
                int u16 = 0, v16 = 0;
                if (d->tme) {
                    float q = qa[0] * fw0 + qa[1] * fw1 + qa[2] * fw2;
                    float uu = ua[0] * fw0 + ua[1] * fw1 + ua[2] * fw2, vv = va[0] * fw0 + va[1] * fw1 + va[2] * fw2;
                    if (!d->fst && q != 0) { uu /= q; vv /= q; }
                    u16 = (int)uu; v16 = (int)vv;
                }
                int f = (int)(v0->f * fw0 + v1->f * fw1 + v2->f * fw2);
                pixel(d, x, y, z, shade(d, col, u16, v16, f));
            }
        }
    }
}
/* Sprites the specialized paths do not take: the same vector shading and pixel pipeline as triangles, with the
   sprite's per-pixel texture coordinate computed as in draw_sprite(). */
__attribute__((target("avx2"))) static void sprite_vec(const Draw *d, const Vtx *b, int px0, int px1, int py0, int py1, int x0, int y0, float u0, float v0, float du, float dv) {
    const __m256i lane = _mm256_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7), col = _mm256_set1_epi32((int)b->rgba), fog = _mm256_set1_epi32(b->f);
    const __m256i zv = _mm256_set1_epi32((int)b->z);
    const int znever = d->zte && d->ztst == 0;
    for (int y = py0; y < py1; y++) {
        if (!ROW_MINE(y)) continue;
        const __m256i v16 = _mm256_set1_epi32(d->tme ? (int)(v0 + ((y << 4) - y0) * dv) : 0);
        for (int xb = px0; xb < px1; xb += 8) {
            const int n = px1 - xb < 8 ? px1 - xb : 8;
            n_visits += n;
            if (znever) continue;
            const __m256i xv = _mm256_add_epi32(_mm256_set1_epi32(xb), lane);
            const __m256i live = _mm256_cmpgt_epi32(_mm256_set1_epi32(n), lane);
            __m256i u16 = _mm256_setzero_si256();
            if (d->tme) u16 = _mm256_cvttps_epi32(_mm256_add_ps(_mm256_set1_ps(u0), _mm256_mul_ps(_mm256_cvtepi32_ps(_mm256_sub_epi32(_mm256_slli_epi32(xv, 4), _mm256_set1_epi32(x0))), _mm256_set1_ps(du))));
            v_pixel(d, xb, y, live, zv, v_shade(d, col, u16, v16, fog));
        }
    }
}
#endif
static void draw_line(const Draw *d, const Vtx *a, const Vtx *b) {
    int x0 = a->x >> 4, y0 = a->y >> 4, x1 = b->x >> 4, y1 = b->y >> 4;
    int dx = abs(x1 - x0), dy = abs(y1 - y0), n = dx > dy ? dx : dy;
    for (int i = 0; i <= n; i++) {
        float t = n ? (float)i / n : 0;
        int x = x0 + (int)lroundf((x1 - x0) * t), y = y0 + (int)lroundf((y1 - y0) * t);
        pixel(d, x, y, b->z, shade(d, b->rgba, 0, 0, b->f));
    }
}
/* HG_GSDRAW=0 skips rasterizing (sound/logic runs; emulated time is unaffected); HG_GSDRAW=N starts drawing at VBlank N */
static int gs_nodraw; static long gs_draw_from;

/* HG_GSVKCHECK=FROM[:TO]: every sprite the GPU rasterizer supports, drawn in those VBlanks, is also drawn on the GPU
   (gsvk.c) from the same local memory, and the two results are compared word for word. Drawing is serial then. */
#ifdef HAVE_VULKAN
#include "gsvk.h"
static long vkchk_from = -2, vkchk_to; static uint64_t vkchk_n, vkchk_exact, vkchk_words, vkchk_skipped;
/* buffers that share memory make the result depend on the order of the pixels: frame and Z pixels [x0,x1]x[y0,y1],
   texels [ua,ub]x[va,vb] (tex: whether the texture is read) */
static int gsvk_buffers_meet(const Draw *d, int x0, int y0, int x1, int y1, int tex, int ua, int va, int ub, int vb, int ft_checked) {
    uint64_t fp[8] = {0}, zp[8] = {0}, tp[8] = {0};
    mark_pages(fp, d->fbp, d->fbw, d->fpsm, x0, y0, x1, y1);
    if (tex) { mark_pages(tp, d->tex.tbp, d->tex.tbw, d->tex.psm, ua, va, ub, vb); if (!ft_checked && pages_meet(fp, tp)) return 1; }
    if (d->zte && d->ztst != 0 && (d->zread || !d->zmsk)) {
        mark_pages(zp, d->zbp, d->fbw, d->zpsm, x0, y0, x1, y1);
        if (pages_meet(fp, zp) || (tex && !d->zmsk && pages_meet(zp, tp))) return 1;
    }
    return 0;
}
static int gsvk_fpsm_ok(const Draw *d) {
    switch (d->fpsm) { case CT32: case CT24: case CT16: case CT16S: case Z32: case Z24: case Z16: case Z16S: return 1; }
    return 0;
}
static int gsvk_sprite_supported(const Draw *d, const SprGeo *g) {
    if (g->px1 <= g->px0 || g->py1 <= g->py0 || !gsvk_fpsm_ok(d)) return 0;
    if (d->tme && sprite_tex_overlaps(d, g->px0, g->px1, g->py0, g->py1, g->x0, g->y0, g->u0, g->v0, g->du, g->dv)) return 0;
    int ua = 0, ub = 0, va = 0, vb = 0, tex = d->tme && sprite_texels(d, g->px0, g->px1, g->py0, g->py1, g->x0, g->y0, g->u0, g->v0, g->du, g->dv, &ua, &ub, &va, &vb);
    return !gsvk_buffers_meet(d, g->px0, g->py0, g->px1 - 1, g->py1 - 1, tex, ua, va, ub, vb, 1);
}
static void gsvk_tables(int32_t *t) {   /* the shader's BT8, CT8, BT4, CT4, BT16, BT16S, BT16Z, CT16 */
    int k = 0;
    for (int i = 0; i < 32; i++) t[k++] = blockTable8[i / 8][i % 8];
    for (int i = 0; i < 256; i++) t[k++] = columnTable8[i / 16][i % 16];
    for (int i = 0; i < 32; i++) t[k++] = blockTable4[i / 4][i % 4];
    for (int i = 0; i < 512; i++) t[k++] = columnTable4[i / 32][i % 32];
    for (int i = 0; i < 32; i++) t[k++] = blockTable16[i / 4][i % 4];
    for (int i = 0; i < 32; i++) t[k++] = blockTable16S[i / 4][i % 4];
    for (int i = 0; i < 32; i++) t[k++] = blockTable16Z[i / 4][i % 4];
    for (int i = 0; i < 128; i++) t[k++] = columnTable16[i / 16][i % 16];
}
static void gsvk_check_sprite(const Draw *d, const Vtx *a, const Vtx *b) {
    SprGeo g; sprite_geo(d, a, b, &g);
    if (!gsvk_sprite_supported(d, &g)) { vkchk_skipped++; draw_sprite(d, a, b); return; }
    const TexState *t = &d->tex;
    GsVkSprite s = { g.px0, g.px1, g.py0, g.py1, g.x0, g.y0, g.u0, g.v0, g.du, g.dv,
        b->rgba, (uint32_t)b->f, d->fbp, d->fbw, d->fpsm, d->fbmsk, d->scx0, d->scx1, d->scy0, d->scy1,
        d->tme, (int)t->tbp, (int)t->tbw, (int)t->psm, (int)t->tw, (int)t->th, (int)t->tcc, (int)t->tfx, t->wms, t->wmt, t->minu, t->maxu, t->minv, t->maxv,
        t->bilinear, t->aem, t->ta0, t->ta1, (int)t->csa, (int)t->cpsm,
        d->ate, d->atst, d->aref, d->afail, d->abe, d->a, d->b, d->cc, d->d, d->fix, d->pabe, d->fba, d->colclamp, d->fge, d->need_dst, d->zte, d->ztst, d->fogcol,
        b->z, d->zbp, d->zpsm, d->zmsk, d->zread, d->date, d->datm, d->fb16, 0 };
    static const uint32_t noclut[256];
    const uint32_t *gpu = gsvk_draw(&s, d->tme && t->clut ? t->clut : noclut, vm);
    draw_sprite(d, a, b);
    if (!gpu) return;
    uint64_t bad = 0; long first = -1;
    for (uint32_t i = 0; i < (1u << 20); i++) if (gpu[i] != vm[i]) { bad++; if (first < 0) first = i; }
    vkchk_n++; if (!bad) vkchk_exact++; vkchk_words += bad;
    static int nlog; if (bad && nlog++ < 40)
        L("GSVKCHECK: VB %llu sprite fbp %u psm %02x tex %d:%02x bil %d tfx %d abe %d(%d%d%d%d) ate %d:%d fge %d rect %d-%d,%d-%d: %llu words differ, first at %lu (cpu %08x gpu %08x)\n",
          (unsigned long long)cur_vb, d->fbp, d->fpsm, d->tme, d->tme ? t->psm : 0, t->bilinear, t->tfx, d->abe, d->a, d->b, d->cc, d->d, d->ate, d->atst, d->fge,
          g.px0, g.px1, g.py0, g.py1, (unsigned long long)bad, first, vm[first], gpu[first]);
}
/* the same for triangles: the set-up of draw_triangle() is done here (same float operations), the pixels on the GPU */
static uint64_t vkt_n, vkt_exact, vkt_words, vkt_skipped;
static void draw_triangle_cpu(const Draw *d, const Vtx *v0, const Vtx *v1, const Vtx *v2);
static void gsvk_check_triangle(const Draw *d, const Vtx *v0, const Vtx *v1, const Vtx *v2) {
    float x0 = v0->x / 16.0f, y0 = v0->y / 16.0f, x1 = v1->x / 16.0f, y1 = v1->y / 16.0f, x2 = v2->x / 16.0f, y2 = v2->y / 16.0f;
    float area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);
    int minx = (int)floorf(fminf(x0, fminf(x1, x2))), maxx = (int)ceilf(fmaxf(x0, fmaxf(x1, x2)));
    int miny = (int)floorf(fminf(y0, fminf(y1, y2))), maxy = (int)ceilf(fmaxf(y0, fmaxf(y1, y2)));
    if (minx < d->scx0) minx = d->scx0; if (maxx > d->scx1) maxx = d->scx1;
    if (miny < d->scy0) miny = d->scy0; if (maxy > d->scy1) maxy = d->scy1;
    const TexState *t = &d->tex;
    int tub = t->tw - 1, tvb = t->th - 1;   /* every texel the wrap modes can reach */
    if ((t->minu | t->maxu) > tub) tub = t->minu | t->maxu; if ((t->minv | t->maxv) > tvb) tvb = t->minv | t->maxv;
    if (area == 0 || minx > maxx || miny > maxy) { draw_triangle_cpu(d, v0, v1, v2); return; }   /* draws nothing */
    if (!gsvk_fpsm_ok(d) || gsvk_buffers_meet(d, minx, miny, maxx, maxy, d->tme, 0, 0, tub, tvb, 0)) {
        vkt_skipped++; draw_triangle_cpu(d, v0, v1, v2);
        static uint64_t why[3]; why[!gsvk_fpsm_ok(d) ? 0 : d->tme && d->tex.tbp == d->fbp ? 1 : 2]++;
        if (vkt_skipped % 200 == 0) L("GSVK: triangles not supported: frame format %llu, texture is the frame %llu, buffers share pages %llu\n",
            (unsigned long long)why[0], (unsigned long long)why[1], (unsigned long long)why[2]);
        return; }
    float ua[3] = {0}, va[3] = {0}, qa[3] = {1, 1, 1};
    const Vtx *vs[3] = { v0, v1, v2 };
    for (int i = 0; i < 3; i++) {
        if (!d->tme) continue;
        if (d->fst) { ua[i] = vs[i]->u; va[i] = vs[i]->v; qa[i] = 1; }
        else { ua[i] = vs[i]->s * d->tex.tw * 16.0f; va[i] = vs[i]->t * d->tex.th * 16.0f; qa[i] = vs[i]->q != 0 ? vs[i]->q : 1.0f; }
    }
    GsVkSprite s = { minx, maxx + 1, miny, maxy + 1, 0, 0, 0, 0, 0, 0,
        0, 0, d->fbp, d->fbw, d->fpsm, d->fbmsk, d->scx0, d->scx1, d->scy0, d->scy1,
        d->tme, (int)t->tbp, (int)t->tbw, (int)t->psm, (int)t->tw, (int)t->th, (int)t->tcc, (int)t->tfx, t->wms, t->wmt, t->minu, t->maxu, t->minv, t->maxv,
        t->bilinear, t->aem, t->ta0, t->ta1, (int)t->csa, (int)t->cpsm,
        d->ate, d->atst, d->aref, d->afail, d->abe, d->a, d->b, d->cc, d->d, d->fix, d->pabe, d->fba, d->colclamp, d->fge, d->need_dst, d->zte, d->ztst, d->fogcol,
        0, d->zbp, d->zpsm, d->zmsk, d->zread, d->date, d->datm, d->fb16,
        1, d->iip, d->fst, x0, y0, x1, y1, x2, y2, 1.0f / area, ua[0], ua[1], ua[2], va[0], va[1], va[2], qa[0], qa[1], qa[2],
        v0->rgba, v1->rgba, v2->rgba, v0->z, v1->z, v2->z, v0->f, v1->f, v2->f };
    static const uint32_t noclut[256];
    const uint32_t *gpu = gsvk_draw(&s, d->tme && t->clut ? t->clut : noclut, vm);
    draw_triangle_cpu(d, v0, v1, v2);
    if (!gpu) return;
    uint64_t bad = 0; long first = -1;
    for (uint32_t i = 0; i < (1u << 20); i++) if (gpu[i] != vm[i]) { bad++; if (first < 0) first = i; }
    vkt_n++; if (!bad) vkt_exact++; vkt_words += bad;
    static int nlog; if (bad && nlog++ < 40)
        L("GSVKCHECK: VB %llu triangle fbp %u psm %02x zpsm %02x zte %d:%d tex %d:%02x fst %d bil %d tfx %d iip %d abe %d fge %d box %d-%d,%d-%d: %llu words differ, first at %lu (cpu %08x gpu %08x)\n",
          (unsigned long long)cur_vb, d->fbp, d->fpsm, d->zpsm, d->zte, d->ztst, d->tme, d->tme ? t->psm : 0, d->fst, t->bilinear, t->tfx, d->iip, d->abe, d->fge,
          minx, maxx, miny, maxy, (unsigned long long)bad, first, vm[first], gpu[first]);
}
#endif
#ifdef HAVE_VULKAN
static int gsvk_checking(void) {
    if (vkchk_from == -2) { const char *e = getenv("HG_GSVKCHECK"); vkchk_from = -1;
        if (e) { vkchk_from = atol(e); const char *c = strchr(e, ':'); vkchk_to = c ? atol(c + 1) : vkchk_from;
                 static int32_t tabs[GSVK_TABLES]; gsvk_tables(tabs);
                 if (!gsvk_init(&xoff32[0][0], &xoff32z[0][0], tabs)) vkchk_from = -1; } }
    return vkchk_from >= 0 && (long)cur_vb >= vkchk_from && (long)cur_vb <= vkchk_to;
}
#endif
static void draw_triangle_cpu(const Draw *d, const Vtx *v0, const Vtx *v1, const Vtx *v2) {
#if defined(__x86_64__)
    if (have_avx2) { draw_triangle_avx2(d, v0, v1, v2); return; }
#endif
    draw_triangle(d, v0, v1, v2);
}
static void rasterize(const Draw *d, int prim, const Vtx *w) {
    switch (prim) {
    case 0: pixel(d, w[0].x >> 4, w[0].y >> 4, w[0].z, shade(d, w[0].rgba, w[0].u, w[0].v, w[0].f)); break;
    case 1: case 2: draw_line(d, &w[0], &w[1]); break;
    case 3: case 4: case 5:
#ifdef HAVE_VULKAN
        if (gsvk_checking()) { gsvk_check_triangle(d, &w[0], &w[1], &w[2]); break; }
#endif
        draw_triangle_cpu(d, &w[0], &w[1], &w[2]); break;
    case 6:
#ifdef HAVE_VULKAN
        if (gsvk_checking()) { gsvk_check_sprite(d, &w[0], &w[1]); break; }
#endif
        draw_sprite(d, &w[0], &w[1]); break;
    }
}

/* ================================================================== GS workers
   With HG_GSTHREADS=n (default: half the CPUs, at most 8; 0 turns them off) the GS thread parses GIF and sets draws
   up, and n worker threads rasterize them. Every worker takes every draw but draws only its own rows: the 8-row bands
   with (y >> 3) % n == k. A pixel is thus always written by the same worker in submission order, and the output is
   the same as drawing serially, given these rules (pages of 8 KB, tracked until the workers have all caught up):
   - a draw reading texture pages that queued draws write, or writing pages that queued draws read as texture, waits
     for the workers: texture reads cross bands;
   - a draw writing pages that queued draws write with another address layout (base, width or format) waits too;
   - a draw whose texture or Z buffer shares pages with its own frame buffer depends on the pixel order inside the
     draw: it is drawn here, serially, once the workers have caught up;
   - local memory accessed outside draws (CLUT loads, transfers, the display at VBlank) waits as needed. */
#if defined(__x86_64__)
typedef struct { Draw d; Vtx w[3]; int prim; } GsCmd;
#define GSQ (1u << 13)
static GsCmd *gsq; static _Atomic uint64_t gsq_head;
static struct { _Atomic uint64_t tail; char pad[56]; } gsw_tail[GS_MAXW];
static pthread_t gsw_thr[GS_MAXW];
static pthread_mutex_t gsw_mx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gsw_cv_work = PTHREAD_COND_INITIALIZER, gsw_cv_done = PTHREAD_COND_INITIALIZER;
static _Atomic int gsw_sleepers, gsw_front_waiting;
static uint64_t pend_w[8], pend_r[8]; static uint32_t page_key[512]; static int pend_any;
static uint64_t n_gsw_issued, n_gsw_drains, n_gsw_serial;
static void *gsw_main(void *arg) {
    const int k = (int)(intptr_t)arg; band_k = k; band_n = gsw_n; gcnt = &gs_cnt[k + 1];
    char nm[16]; snprintf(nm, sizeof nm, "gsw%d", k); host_prof_thread(nm);
    uint64_t t = 0;
    for (;;) {
        uint64_t h = atomic_load_explicit(&gsq_head, memory_order_acquire);
        for (int spin = 0; t == h && spin < 20000; spin++) { _mm_pause(); h = atomic_load_explicit(&gsq_head, memory_order_acquire); }
        if (t == h) {
            pthread_mutex_lock(&gsw_mx); atomic_fetch_add(&gsw_sleepers, 1);
            while (t == atomic_load(&gsq_head)) pthread_cond_wait(&gsw_cv_work, &gsw_mx);
            atomic_fetch_sub(&gsw_sleepers, 1); pthread_mutex_unlock(&gsw_mx);
            continue;
        }
        for (; t != h; t++) {
            const GsCmd *c = &gsq[t & (GSQ - 1)];
            rasterize(&c->d, c->prim, c->w);
            atomic_store_explicit(&gsw_tail[k].tail, t + 1, memory_order_release);
        }
        if (atomic_load(&gsw_front_waiting)) { pthread_mutex_lock(&gsw_mx); pthread_cond_broadcast(&gsw_cv_done); pthread_mutex_unlock(&gsw_mx); }
    }
    return 0;
}
static uint64_t gsw_min_tail(void) {
    uint64_t m = UINT64_MAX;
    for (int k = 0; k < gsw_n; k++) { const uint64_t t = atomic_load_explicit(&gsw_tail[k].tail, memory_order_acquire); if (t < m) m = t; }
    return m;
}
/* wait until every worker has finished the draws before seq */
static void gsw_wait_seq(uint64_t seq) {
    if (!gsw_n || gsw_min_tail() >= seq) return;
    for (int spin = 0; spin < 20000; spin++) { _mm_pause(); if (gsw_min_tail() >= seq) return; }
    pthread_mutex_lock(&gsw_mx); atomic_store(&gsw_front_waiting, 1);
    while (gsw_min_tail() < seq) {
        struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); ts.tv_nsec += 1000000; if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
        pthread_cond_timedwait(&gsw_cv_done, &gsw_mx, &ts);
    }
    atomic_store(&gsw_front_waiting, 0); pthread_mutex_unlock(&gsw_mx);
}
static void pend_clear(void) { memset(pend_w, 0, sizeof pend_w); memset(pend_r, 0, sizeof pend_r); pend_any = 0; }
/* all queued draws done: local memory is current for this thread */
static void gsw_drain(void) {
    if (!gsw_n || !pend_any) return;
    const uint64_t h = atomic_load_explicit(&gsq_head, memory_order_relaxed);
    if (gsw_min_tail() < h) { n_gsw_drains++; gsw_wait_seq(h); }
    pend_clear();
}
/* pages this thread is about to read (write = 0) or write outside a draw */
static void gsw_access(const uint64_t pages[8], int write) {
    if (!gsw_n || !pend_any) return;
    if (pages_meet(pages, pend_w) || (write && pages_meet(pages, pend_r))) gsw_drain();
}
/* address layout of a frame or Z buffer: draws with equal keys map a pixel (x, y) to the same address */
static uint32_t lay_key(uint32_t bp, uint32_t bw, int psm) {
    int cls;
    switch (psm) { case CT32: case CT24: cls = 1; break; case CT16: cls = 2; break; case CT16S: cls = 3; break;
        case Z32: case Z24: cls = 4; break; case Z16: case Z16S: cls = 5; break; default: cls = 8 + psm; }
    return 0x80000000u | (uint32_t)cls << 20 | (bw & 63) << 14 | (bp & 0x3FFF);
}
static int key_clash(const uint64_t pg[8], uint32_t key) {   /* a page of pg written by a queued draw with another layout */
    for (int i = 0; i < 8; i++) for (uint64_t m = pg[i] & pend_w[i]; m; m &= m - 1) if (page_key[i * 64 + __builtin_ctzll(m)] != key) return 1;
    return 0;
}
static void key_set(const uint64_t pg[8], uint32_t key) {
    for (int i = 0; i < 8; i++) for (uint64_t m = pg[i]; m; m &= m - 1) page_key[i * 64 + __builtin_ctzll(m)] = key;
}
static int xfer_busy(void);
static void gsw_issue(const Draw *d, int prim, const Vtx *w) {
    /* the pixels the draw can touch, generous by a pixel or two, inside the scissor (every path clips to it) */
    static const int nv[8] = { 1, 2, 2, 3, 3, 3, 2, 1 };
    int x0 = w[0].x >> 4, y0 = w[0].y >> 4, x1 = x0, y1 = y0;
    for (int i = 1; i < nv[prim]; i++) { const int x = w[i].x >> 4, y = w[i].y >> 4; if (x < x0) x0 = x; if (x > x1) x1 = x; if (y < y0) y0 = y; if (y > y1) y1 = y; }
    x0 -= 1; y0 -= 1; x1 += 2; y1 += 2;
    if (x0 < d->scx0) x0 = d->scx0; if (x1 > d->scx1) x1 = d->scx1; if (y0 < d->scy0) y0 = d->scy0; if (y1 > d->scy1) y1 = d->scy1;
    if (x0 > x1 || y0 > y1) return;
    uint64_t F[8] = {0}, Z[8] = {0}, T[8] = {0};
    mark_pages(F, d->fbp, d->fbw, d->fpsm, x0, y0, x1, y1);
    const int zuse = d->zte && d->ztst != 0 && (d->zread || !d->zmsk);   /* Z memory read or written */
    if (zuse) mark_pages(Z, d->zbp, d->fbw, d->zpsm, x0, y0, x1, y1);
    int self = 0;
    if (d->tme && prim == 6) {   /* sprites: the texels they read, from the renderer's own expressions */
        SprGeo g; sprite_geo(d, &w[0], &w[1], &g); int ua, ub, va, vb;
        if (sprite_texels(d, g.px0, g.px1, g.py0, g.py1, g.x0, g.y0, g.u0, g.v0, g.du, g.dv, &ua, &ub, &va, &vb)) {
            mark_pages(T, d->tex.tbp, d->tex.tbw, d->tex.psm, ua, va, ub, vb);
            self = sprite_tex_overlaps(d, g.px0, g.px1, g.py0, g.py1, g.x0, g.y0, g.u0, g.v0, g.du, g.dv);
        }
    } else if (d->tme) {   /* every texel wrap() can return */
        const TexState *t = &d->tex; int tw = t->tw, th = t->th;
        if (t->wms >= 2 && (t->minu | t->maxu) + 1 > tw) tw = (t->minu | t->maxu) + 1;
        if (t->wmt >= 2 && (t->minv | t->maxv) + 1 > th) th = (t->minv | t->maxv) + 1;
        mark_pages(T, t->tbp, t->tbw, t->psm, 0, 0, tw - 1, th - 1);
    }
    const uint32_t kf = lay_key(d->fbp, d->fbw, d->fpsm), kz = lay_key(d->zbp, d->fbw, d->zpsm);
    if (prim != 6) self = pages_meet(T, F);
    if (self || pages_meet(T, Z) || (zuse && pages_meet(F, Z) && kf != kz) || xfer_busy()) {
        gsw_drain(); n_gsw_serial++; rasterize(d, prim, w); return;
    }
    const uint64_t h = atomic_load_explicit(&gsq_head, memory_order_relaxed);
    if (pend_any && gsw_min_tail() == h) pend_clear();
    if (pend_any && (pages_meet(T, pend_w) || pages_meet(F, pend_r) || pages_meet(Z, pend_r) || key_clash(F, kf) || (zuse && key_clash(Z, kz)))) gsw_drain();
    for (int i = 0; i < 8; i++) { pend_w[i] |= F[i] | Z[i]; pend_r[i] |= T[i]; }
    key_set(F, kf); if (zuse) key_set(Z, kz); pend_any = 1;
    if (h - gsw_min_tail() >= GSQ) gsw_wait_seq(h - GSQ + 1);
    GsCmd *c = &gsq[h & (GSQ - 1)]; c->d = *d; memcpy(c->w, w, sizeof c->w); c->prim = prim;
    clut_slot_seq[clut_slot] = h + 1;
    atomic_store(&gsq_head, h + 1);
    if (atomic_load(&gsw_sleepers)) { pthread_mutex_lock(&gsw_mx); pthread_cond_broadcast(&gsw_cv_work); pthread_mutex_unlock(&gsw_mx); }
    n_gsw_issued++;
}
static void gsw_init(void) {
    const char *e = getenv("HG_GSTHREADS"); int n = e ? atoi(e) : host_ncpus() / 2;
    if (!e && n > 8) n = 8;
    if (n > GS_MAXW) n = GS_MAXW;
    if (getenv("HG_PIXWATCH") || getenv("HG_SPRLOG") || getenv("HG_SPRSTAT") || getenv("HG_GSVKCHECK")) n = 0;   /* per-pixel debugging: one drawing thread */
    if (n < 1 || !(gsq = calloc(GSQ, sizeof *gsq))) { L("GS: drawing on the GS thread (HG_GSTHREADS=0)\n"); return; }
    gsw_n = n;
    for (int k = 0; k < n; k++) if (pthread_create(&gsw_thr[k], 0, gsw_main, (void *)(intptr_t)k)) { L("GS: worker %d failed to start\n", k); exit(1); }
    L("GS: %d drawing threads (HG_GSTHREADS)\n", n);
}
#else
static void gsw_drain(void) {}
static void gsw_wait_seq(uint64_t seq) { (void)seq; }
static void gsw_access(const uint64_t pages[8], int write) { (void)pages; (void)write; }
static void gsw_issue(const Draw *d, int prim, const Vtx *w) { rasterize(d, prim, w); }
static void gsw_init(void) {}
static void mark_pages(uint64_t pages[8], uint32_t bp, uint32_t bw, int psm, int x0, int y0, int x1, int y1) { (void)pages; (void)bp; (void)bw; (void)psm; (void)x0; (void)y0; (void)x1; (void)y1; }
#endif

static void vertex_kick(Vtx v, int draw) {
    Draw d; int prim = BITS(r_prim, 0, 3);
    {   /* HG_VTXLOG=path:vb writes every vertex kicked during that VBlank and the next (x, y in 1/16 pixel with offset, z, drawing kick) */
        static FILE *vf; static long vvb = -2;
        if (vvb == -2) { const char *e = getenv("HG_VTXLOG"); vvb = -1; if (e) { char buf[512]; snprintf(buf, sizeof buf, "%s", e); char *c = strrchr(buf, ':'); if (c) { *c = 0; vvb = atol(c + 1); vf = fopen(buf, "w"); } } }
        if (vf && (long)cur_vb >= vvb && (long)cur_vb < vvb + 2) fprintf(vf, "%d %d %u %d %d\n", v.x, v.y, v.z, draw, prim);
        if (vf && (long)cur_vb >= vvb + 2) { fclose(vf); vf = 0; }
    }
    static const int need[8] = { 1, 2, 2, 3, 3, 3, 2, 1 };
    vq[nvq++] = v;
    if (nvq < need[prim]) return;
    if (draw && !gs_nodraw) {
        draw_setup(&d);
        uint64_t off = d.c->xyoffset; int ox = BITS(off, 0, 16), oy = BITS(off, 32, 16);
        Vtx w[3]; for (int i = 0; i < nvq; i++) { w[i] = vq[i]; w[i].x -= ox; w[i].y -= oy; }
        n_prims++; n_kicks_frame++;
        if (watch_x >= 0 && (long)cur_vb >= watch_vb) snprintf(watch_prim, sizeof watch_prim, "prim %d tme %d psm %02x tbp %u abe %d(%d%d%d%d) ate %d:%d:%d zte %d:%d zbuf %u:%02x:%d fge %d fst %d v0 (%d,%d z%u rgba %08x st %g,%g,%g uv %d,%d) v1 (%d,%d z%u rgba %08x st %g,%g,%g) v2 (%d,%d z%u rgba %08x st %g,%g,%g)",
            prim, d.tme, d.tex.psm, d.tex.tbp, d.abe, d.a, d.b, d.cc, d.d, d.ate, d.atst, d.afail, d.zte, d.ztst, d.zbp, d.zpsm, d.zmsk, d.fge, d.fst, w[0].x >> 4, w[0].y >> 4, w[0].z, w[0].rgba, w[0].s, w[0].t, w[0].q, w[0].u, w[0].v,
            w[1].x >> 4, w[1].y >> 4, w[1].z, w[1].rgba, w[1].s, w[1].t, w[1].q, nvq > 2 ? w[2].x >> 4 : 0, nvq > 2 ? w[2].y >> 4 : 0, nvq > 2 ? w[2].z : 0, nvq > 2 ? w[2].rgba : 0, nvq > 2 ? w[2].s : 0, nvq > 2 ? w[2].t : 0, nvq > 2 ? w[2].q : 0);
        double t0 = host_now();
        uint64_t px0 = n_pixels, vis0 = n_visits;
        int skip = 0;
        if (prim >= 3 && prim <= 5) {
            if (GETENV_ONCE("HG_BIGTRI")) {   /* debugging: report triangles far larger than the screen */
                int xs[3] = { w[0].x >> 4, w[1].x >> 4, w[2].x >> 4 }, ys[3] = { w[0].y >> 4, w[1].y >> 4, w[2].y >> 4 };
                int mnx = xs[0], mxx = xs[0], mny = ys[0], mxy = ys[0];
                for (int i = 1; i < 3; i++) { if (xs[i] < mnx) mnx = xs[i]; if (xs[i] > mxx) mxx = xs[i]; if (ys[i] < mny) mny = ys[i]; if (ys[i] > mxy) mxy = ys[i]; }
                if ((mxx - mnx > 1500 || mxy - mny > 1500) && GETENV_ONCE("HG_BIGTRI")[0] == 's') skip = 1;   /* =s: skip them */
                static int nb; if ((mxx - mnx > 1500 || mxy - mny > 1500) && nb++ < 30)
                    L("BIGTRI prim %d tme %d: (%d,%d z%u q%g) (%d,%d z%u q%g) (%d,%d z%u q%g) scissor %d-%d,%d-%d\n", prim, d.tme, xs[0], ys[0], w[0].z, w[0].q, xs[1], ys[1], w[1].z, w[1].q, xs[2], ys[2], w[2].z, w[2].q, d.scx0, d.scx1, d.scy0, d.scy1);
            }
        }
        if (!skip) { if (gsw_n) gsw_issue(&d, prim, w); else rasterize(&d, prim, w); }
        int k = prim == 6 ? 0 : (prim >= 3 && prim <= 5) ? 1 : 2;
        st_prims[k]++; st_px[k] += n_pixels - px0; st_visits[k] += n_visits - vis0; st_time[k] += host_now() - t0;
        if (d.tme) st_tex[k]++; if (d.abe) st_blend[k]++; if (d.zte) st_z[k]++;
        static int sprstat = -1; if (sprstat < 0) sprstat = !getenv("HG_SPRSTAT") ? 3 : getenv("HG_SPRSTAT")[0] == 't' ? 1 : 0;
        if (k == sprstat) {   /* configuration histogram weighted by pixels (HG_SPRSTAT: sprites, =t: triangles) */
            char key[160]; snprintf(key, sizeof key, "fb%02x tex%d:%02x bil%d tfx%d tcc%d abe%d(%d%d%d%d fix%d) ate%d:%d zte%d:%d zmsk%d fbmsk%08x iip%d fge%d fst%d z%02x",
                d.fpsm, d.tme, d.tme ? d.tex.psm : 0, d.tme ? d.tex.bilinear : 0, d.tme ? d.tex.tfx : 0, d.tme ? d.tex.tcc : 0, d.abe, d.a, d.b, d.cc, d.d, d.fix, d.ate, d.atst, d.zte, d.ztst, d.zmsk, d.fbmsk,
                d.iip, d.fge, d.fst, d.zpsm);
            static struct { char k[160]; uint64_t px, n; double t; } h[256]; static int nh;
            int j = 0; for (; j < nh; j++) if (!strcmp(h[j].k, key)) break;
            if (j == nh && nh < 256) { strcpy(h[nh].k, key); nh++; }
            if (j < 256) { h[j].px += n_visits - vis0; h[j].n++; h[j].t += host_now() - t0; }
            gs_sprhist_ptr = (void *)h; gs_sprhist_n = nh;
        }
        tprof[1] += host_now() - t0;
    }
    if (!draw && watch_x >= 0 && (long)cur_vb >= watch_vb && prim >= 3 && prim <= 5) {   /* triangle kicked with ADC (not drawn) over the watched pixel? */
        Draw d; draw_setup(&d); uint64_t off = d.c->xyoffset; int ox = BITS(off, 0, 16), oy = BITS(off, 32, 16);
        float X[3], Y[3]; for (int i = 0; i < 3; i++) { X[i] = (vq[i].x - ox) / 16.0f; Y[i] = (vq[i].y - oy) / 16.0f; }
        float px = watch_x, py = watch_y, a = (X[1]-X[0])*(Y[2]-Y[0]) - (X[2]-X[0])*(Y[1]-Y[0]);
        float w0 = ((X[1]-px)*(Y[2]-py) - (X[2]-px)*(Y[1]-py)) / a, w1 = ((X[2]-px)*(Y[0]-py) - (X[0]-px)*(Y[2]-py)) / a, w2 = 1 - w0 - w1;
        if (a != 0 && w0 >= 0 && w1 >= 0 && w2 >= 0) L("PIXWATCH-ADC (%d,%d) fbp %u: triangle not drawn (ADC): (%.0f,%.0f q%g) (%.0f,%.0f q%g) (%.0f,%.0f q%g)\n", watch_x, watch_y, d.fbp,
            X[0], Y[0], vq[0].q, X[1], Y[1], vq[1].q, X[2], Y[2], vq[2].q);
    }
    switch (prim) {
    case 2: vq[0] = vq[1]; nvq = 1; break;
    case 4: vq[0] = vq[1]; vq[1] = vq[2]; nvq = 2; break;
    case 5: vq[1] = vq[2]; nvq = 2; break;
    default: nvq = 0;
    }
}

/* ================================================================== transfers */
static struct { int active, dir; uint32_t bp, bw, psm; int sx, sy, w, h, x, y; uint64_t carry; int ncarry; } xfer;
static uint64_t st_xfer_psm[64];
static void xfer_put_px(uint32_t v) {
    if (xfer.y >= xfer.h) return;
    write_px(xfer.bp, xfer.bw, xfer.psm, xfer.sx + xfer.x, xfer.sy + xfer.y, v);
    if (++xfer.x >= xfer.w) { xfer.x = 0; xfer.y++; }
}
static void hwreg_write(uint64_t v) {
    if (!xfer.active) return;
    switch (xfer.psm) {
    case CT32: case Z32: xfer_put_px((uint32_t)v); xfer_put_px((uint32_t)(v >> 32)); break;
    case CT24: case Z24: {   /* 3 bytes per pixel, stream may straddle 64-bit words */
        for (int i = 0; i < 8; i++) {
            xfer.carry |= ((v >> (8 * i)) & 255) << (8 * xfer.ncarry);
            if (++xfer.ncarry == 3) { xfer_put_px((uint32_t)xfer.carry); xfer.carry = 0; xfer.ncarry = 0; }
        }
        break; }
    case CT16: case CT16S: case Z16: case Z16S: for (int i = 0; i < 4; i++) xfer_put_px((v >> (16 * i)) & 0xFFFF); break;
    case T8: case T8H: for (int i = 0; i < 8; i++) xfer_put_px((v >> (8 * i)) & 255); break;
    case T4: case T4HL: case T4HH: for (int i = 0; i < 16; i++) xfer_put_px((v >> (4 * i)) & 15); break;
    default: xfer_put_px((uint32_t)v); xfer_put_px((uint32_t)(v >> 32));
    }
    if (xfer.y >= xfer.h) xfer.active = 0;
}
/* One qword of image data: whole-qword fast paths when the qword covers an aligned run of pixels inside one row
   (PSMT8: 16 pixels in one block row, PSMT4: 32, PSMCT32: 4); same addresses as write_px(). */
static void xfer_qword(const uint64_t q[2]) {
    if (!xfer.active) return;
    const int X = xfer.sx + xfer.x, Y = xfer.sy + xfer.y;
    const uint8_t *b = (const uint8_t *)q;
    if (xfer.y < xfer.h) switch (xfer.psm) {
    case T8:
        if (!(X & 15) && xfer.x + 16 <= xfer.w) {
            const uint32_t page = (X >> 7) + (Y >> 6) * ((xfer.bw + 1) >> 1);
            const uint32_t blk = (xfer.bp + page * 32 + blockTable8[(Y >> 4) & 3][(X >> 4) & 7]) * 256;
            const uint8_t *ct = columnTable8[Y & 15];
            for (int i = 0; i < 16; i++) VM8[(blk + ct[i]) & 0x3FFFFF] = b[i];
            goto advance16;
        }
        break;
    case T4:
        if (!(X & 31) && xfer.x + 32 <= xfer.w) {
            const uint32_t page = (X >> 7) + (Y >> 7) * ((xfer.bw + 1) >> 1);
            const uint32_t blk = (xfer.bp + page * 32 + blockTable4[(Y >> 4) & 7][(X >> 5) & 3]) * 512;
            const uint16_t *ct = columnTable4[Y & 15];
            for (int i = 0; i < 32; i++) {
                const uint32_t a = (blk + ct[i]) & 0x7FFFFF; const uint8_t v = (b[i >> 1] >> ((i & 1) * 4)) & 15;
                uint8_t *p = &VM8[a >> 1]; *p = (a & 1) ? (*p & 0x0F) | (v << 4) : (*p & 0xF0) | v;
            }
            xfer.x += 32; goto rowend;
        }
        break;
    case CT32:
        if (xfer.x + 4 <= xfer.w) {
            const uint32_t base = xfer.bp * 64 + (uint32_t)(Y >> 5) * xfer.bw * 2048; const uint32_t *xo = xoff32[Y & 31];
            for (int i = 0; i < 4; i++) { uint32_t v; memcpy(&v, b + 4 * i, 4); vm[(base + xo[(X + i) & 2047]) & 0xFFFFF] = v; }
            xfer.x += 4; goto rowend;
        }
        break;
    }
    hwreg_write(q[0]); hwreg_write(q[1]);
    return;
advance16:
    xfer.x += 16;
rowend:
    if (xfer.x >= xfer.w) { xfer.x = 0; xfer.y++; }
    if (xfer.y >= xfer.h) xfer.active = 0;
}
static int xfer_busy(void) { return xfer.active; }   /* draws during a transfer are drawn serially (GS workers) */
static void trxdir_write(uint64_t v) {
    int xdir = v & 3;
    if (xdir == 0) {
        xfer.active = 1; xfer.bp = BITS(r_bitbltbuf, 32, 14); xfer.bw = BITS(r_bitbltbuf, 48, 6); xfer.psm = BITS(r_bitbltbuf, 56, 6);
        xfer.sx = BITS(r_trxpos, 32, 11); xfer.sy = BITS(r_trxpos, 48, 11); xfer.w = BITS(r_trxreg, 0, 12); xfer.h = BITS(r_trxreg, 32, 12);
        xfer.x = xfer.y = 0; xfer.carry = 0; xfer.ncarry = 0;
        if (!xfer.w || !xfer.h) xfer.active = 0;
        if (xfer.active && gsw_n) {   /* queued draws must be done with the pages the image goes to */
            uint64_t pg[8] = {0}; mark_pages(pg, xfer.bp, xfer.bw, xfer.psm, xfer.sx, xfer.sy, xfer.sx + xfer.w - 1, xfer.sy + xfer.h - 1); gsw_access(pg, 1);
        }
    } else if (xdir == 2) {        /* local -> local */
        gsw_drain();
        uint32_t sbp = BITS(r_bitbltbuf, 0, 14), sbw = BITS(r_bitbltbuf, 16, 6), spsm = BITS(r_bitbltbuf, 24, 6);
        uint32_t dbp = BITS(r_bitbltbuf, 32, 14), dbw = BITS(r_bitbltbuf, 48, 6), dpsm = BITS(r_bitbltbuf, 56, 6);
        int sx = BITS(r_trxpos, 0, 11), sy = BITS(r_trxpos, 16, 11), dx = BITS(r_trxpos, 32, 11), dy = BITS(r_trxpos, 48, 11);
        int w = BITS(r_trxreg, 0, 12), h = BITS(r_trxreg, 32, 12);
        uint32_t *tmp = malloc((size_t)w * h * 4); if (!tmp) return;
        for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) tmp[y * w + x] = read_px(sbp, sbw, spsm, (sx + x) & 2047, (sy + y) & 2047);
        for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) write_px(dbp, dbw, dpsm, (dx + x) & 2047, (dy + y) & 2047, tmp[y * w + x]);
        free(tmp);
    }
}

/* ================================================================== register writes */
static Vtx cur_vtx(void) {
    Vtx v; memset(&v, 0, sizeof v);
    v.rgba = (uint32_t)r_rgbaq; float q; uint32_t qb = (uint32_t)(r_rgbaq >> 32); memcpy(&q, &qb, 4); v.q = q;
    uint32_t sb = (uint32_t)r_st, tb = (uint32_t)(r_st >> 32); memcpy(&v.s, &sb, 4); memcpy(&v.t, &tb, 4);
    v.u = BITS(r_uv, 0, 14); v.v = BITS(r_uv, 16, 14); v.f = BITS(r_fog, 56, 8);
    return v;
}
void gs_write_reg(int reg, uint64_t v) {
    switch (reg) {
    case 0x00: r_prim = v; nvq = 0; break;
    case 0x01: r_rgbaq = v; break;
    case 0x02: r_st = v; break;
    case 0x03: r_uv = v; break;
    case 0x04: case 0x0C: { Vtx x = cur_vtx(); x.x = BITS(v, 0, 16); x.y = BITS(v, 16, 16); x.z = BITS(v, 32, 24); x.f = BITS(v, 56, 8); vertex_kick(x, reg == 0x04); break; }
    case 0x05: case 0x0D: { Vtx x = cur_vtx(); x.x = BITS(v, 0, 16); x.y = BITS(v, 16, 16); x.z = (uint32_t)(v >> 32); vertex_kick(x, reg == 0x05); break; }
    case 0x06: case 0x07: gc[reg - 6].tex0 = v; clut_load(v); break;
    case 0x08: case 0x09: gc[reg - 8].clamp = v; break;
    case 0x0A: r_fog = v; break;
    case 0x14: case 0x15: gc[reg - 0x14].tex1 = v; break;
    case 0x16: case 0x17: {  /* TEX2: only PSM and CLUT fields */
        GsCtx *c = &gc[reg - 0x16]; uint64_t m = (0x3Full << 20) | (0x7FFFFFFFFull << 37);
        c->tex0 = (c->tex0 & ~m) | (v & m); clut_load(c->tex0); break; }
    case 0x18: case 0x19: gc[reg - 0x18].xyoffset = v; break;
    case 0x1A: r_prmodecont = v & 1; break;
    case 0x1B: r_prmode = v; break;
    case 0x1C: r_texclut = v; break;
    case 0x3B: r_texa = v; break;
    case 0x3D: r_fogcol = v; break;
    case 0x40: case 0x41: gc[reg - 0x40].scissor = v; break;
    case 0x42: case 0x43: gc[reg - 0x42].alpha = v; break;
    case 0x44: r_dimx = v; break;
    case 0x45: r_dthe = v; break;
    case 0x46: r_colclamp = v; break;
    case 0x47: case 0x48: gc[reg - 0x47].test = v; break;
    case 0x49: r_pabe = v; break;
    case 0x4A: case 0x4B: gc[reg - 0x4A].fba = v; break;
    case 0x4C: case 0x4D: gc[reg - 0x4C].frame = v; break;
    case 0x4E: case 0x4F: gc[reg - 0x4E].zbuf = v; break;
    case 0x50: r_bitbltbuf = v; break;
    case 0x51: r_trxpos = v; break;
    case 0x52: r_trxreg = v; break;
    case 0x53: trxdir_write(v); break;
    case 0x54: hwreg_write(v); break;
    case 0x61: if (gspriv && !mtgs) { uint32_t csr;   /* reads of CSR report FINISH anyway (hle_gs_read) */ memcpy(&csr, gspriv + 0x1000, 4); csr |= 2; memcpy(gspriv + 0x1000, &csr, 4); } break;   /* FINISH */
    case 0x60: case 0x62: break;   /* SIGNAL, LABEL: not modelled */
    default: break;
    }
}

/* ================================================================== GIF packet parser (one state per path) */
typedef struct { int nloop, flg, nreg, reg_i; uint64_t regs; int eop; int in_packet; } GifPath;
static GifPath gpath[4];
static void gif_tag(GifPath *g, const uint64_t q[2]) {
    g->nloop = BITS(q[0], 0, 15); g->eop = BITS(q[0], 15, 1);
    g->flg = BITS(q[0], 58, 2); g->nreg = BITS(q[0], 60, 4); if (!g->nreg) g->nreg = 16;
    g->regs = q[1]; g->reg_i = 0; g->in_packet = g->nloop > 0;
    if (BITS(q[0], 46, 1)) gs_write_reg(0x00, BITS(q[0], 47, 11));
}
static void packed(int r, const uint64_t q[2]) {
    switch (r) {
    case 0x0: gs_write_reg(0x00, q[0] & 0x7FF); break;
    case 0x1: { uint64_t rgba = (q[0] & 0xFF) | ((q[0] >> 24) & 0xFF00) | ((q[1] & 0xFF) << 16) | (((q[1] >> 32) & 0xFF) << 24);
        float qf = st_q; uint32_t qb; memcpy(&qb, &qf, 4); gs_write_reg(0x01, rgba | (uint64_t)qb << 32); break; }
    case 0x2: { uint32_t qb = (uint32_t)q[1]; memcpy(&st_q, &qb, 4); gs_write_reg(0x02, q[0]); break; }
    case 0x3: gs_write_reg(0x03, (q[0] & 0x3FFF) | (((q[0] >> 32) & 0x3FFF) << 16)); break;
    case 0x4: { uint64_t v = (q[0] & 0xFFFF) | (((q[0] >> 32) & 0xFFFF) << 16) | (((q[1] >> 4) & 0xFFFFFF) << 32) | (((q[1] >> 36) & 0xFF) << 56);
        gs_write_reg((q[1] >> 47) & 1 ? 0x0C : 0x04, v); break; }
    case 0x5: { uint64_t v = (q[0] & 0xFFFF) | (((q[0] >> 32) & 0xFFFF) << 16) | ((q[1] & 0xFFFFFFFF) << 32);
        gs_write_reg((q[1] >> 47) & 1 ? 0x0D : 0x05, v); break; }
    case 0xA: gs_write_reg(0x0A, ((q[1] >> 36) & 0xFF) << 56); break;
    case 0xE: gs_write_reg((int)(q[1] & 0xFF), q[0]); break;
    case 0xF: break;
    default: gs_write_reg(r, q[0]); break;    /* 6..9, C, D: register value in the low 64 bits */
    }
}
static uint64_t n_gif_tags;
static void gif_qword_exec(int path, const uint64_t q[2]) {
    GifPath *g = &gpath[path & 3];
    if (!g->in_packet) { gif_tag(g, q); n_gif_tags++; return; }
    switch (g->flg) {
    case 0:
        packed((g->regs >> (4 * g->reg_i)) & 15, q);
        if (++g->reg_i == g->nreg) { g->reg_i = 0; if (--g->nloop == 0) g->in_packet = 0; }
        break;
    case 1:
        for (int k = 0; k < 2 && g->in_packet; k++) {
            gs_write_reg((int)((g->regs >> (4 * g->reg_i)) & 15), q[k]);
            if (++g->reg_i == g->nreg) { g->reg_i = 0; if (--g->nloop == 0) g->in_packet = 0; }
        }
        break;
    default: {
        static uint64_t ncall; const int timed = (++ncall & 63) == 0; double t0 = timed ? host_now() : 0;
        xfer_qword(q); n_xfer_qw++;
        if (timed) tprof[5] += (host_now() - t0) * 64;
        st_xfer_psm[xfer.psm & 63]++; }
        if (--g->nloop == 0) g->in_packet = 0;
    }
}

/* ================================================================== display output */
static uint64_t frames_written;
/* Copy the displayed picture (read circuit from PMODE, DISPFB/DISPLAY) into out as 0x00BBGGRR words.
   Returns 0 when nothing displayable is set up. */
static int display_from(const uint8_t *priv, uint32_t *out, int maxw, int maxh, int *pw, int *ph) {
    if (!priv) return 0;
    uint64_t pmode, dispfb, display;
    memcpy(&pmode, priv + 0x00, 8);
    int c2 = (pmode >> 1) & 1;
    memcpy(&dispfb, priv + (c2 ? 0x90 : 0x70), 8); memcpy(&display, priv + (c2 ? 0xa0 : 0x80), 8);
    uint32_t fbp = BITS(dispfb, 0, 9) * 32, fbw = BITS(dispfb, 9, 6), psm = BITS(dispfb, 15, 5), dbx = BITS(dispfb, 32, 11), dby = BITS(dispfb, 43, 11);
    int magh = BITS(display, 23, 4) + 1, dw = BITS(display, 32, 12) + 1, dh = BITS(display, 44, 11) + 1;
    int w = dw / magh, h = dh; if (w > maxw) w = maxw; if (h > maxh) h = maxh;
    if (w <= 1 || h <= 1) return 0;
    int is16 = psm == 2 || psm == 10;
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
        uint32_t c = read_px(fbp, fbw, is16 ? (int)psm : CT32, dbx + x, dby + y);
        if (is16) c = c16to32(c, 0, 0, 0x80);
        out[y * w + x] = c & 0xFFFFFF;
    }
    *pw = w; *ph = h; return 1;
}
static void dump_frame(uint64_t vb, const uint8_t *priv) {
    const char *dir = getenv("HG_FRAMES"); if (!dir) return;
    static uint32_t buf[1024 * 1024]; int w, h;
    if (!display_from(priv, buf, 1024, 1024, &w, &h)) return;
    char path[1024]; snprintf(path, sizeof path, "%s/frame_%06llu.ppm", dir, (unsigned long long)vb);
    FILE *f = fopen(path, "wb"); if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    uint8_t *row = malloc((size_t)w * 3);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) { uint32_t c = buf[y * w + x]; row[x * 3] = c; row[x * 3 + 1] = c >> 8; row[x * 3 + 2] = c >> 16; }
        fwrite(row, 1, (size_t)w * 3, f);
    }
    free(row); fclose(f); frames_written++;
}
/* ================================================================== GS thread
   With HG_MTGS (default on with 2+ CPUs) the GS runs on its own thread: GIF qwords and VBlank markers go through a
   ring buffer and are executed in the same order as in single-threaded mode, so the output is the same. The
   producer tracks GIF packet boundaries itself to answer gif_path_done() (XGKICK), snapshots the privileged display
   registers at each VBlank, and stays at most two VBlanks ahead. Frames are dumped and handed to the window by
   the GS thread when it reaches the VBlank marker. */
enum { E_PATH0 = 0, E_VBLANK = 4 };
typedef struct { uint64_t q[2]; uint64_t kind; } GsEnt;
#define RING (1u << 18)
static GsEnt *ring; static _Atomic uint64_t r_head, r_tail;      /* head: written by producer, tail: by consumer */
static pthread_t gs_thread;
static pthread_mutex_t gs_mx = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t gs_cv_data = PTHREAD_COND_INITIALIZER, gs_cv_space = PTHREAD_COND_INITIALIZER;
static _Atomic int cons_sleeping, prod_sleeping; static _Atomic uint64_t vb_sent, vb_done;
static uint8_t priv_snap[4][0xB0];
static GifPath shadow[4];                                        /* producer-side packet tracking */
static uint32_t front[1024 * 1024]; static int front_w, front_h, front_ok; static pthread_mutex_t front_mx = PTHREAD_MUTEX_INITIALIZER;
static int want_front;
static void gs_vblank_exec(uint64_t vb, const uint8_t *priv);
static void *gs_main(void *arg) {
    (void)arg; host_prof_thread("gs"); host_thread_boost();
    for (;;) {
        uint64_t t = atomic_load_explicit(&r_tail, memory_order_relaxed), h = atomic_load_explicit(&r_head, memory_order_acquire);
        if (t == h) {
            pthread_mutex_lock(&gs_mx); atomic_store(&cons_sleeping, 1);
            while (atomic_load(&r_tail) == atomic_load(&r_head)) pthread_cond_wait(&gs_cv_data, &gs_mx);
            atomic_store(&cons_sleeping, 0); pthread_mutex_unlock(&gs_mx);
            continue;
        }
        for (; t != h; t++) {
            const GsEnt *e = &ring[t & (RING - 1)];
            if (e->kind < 4) gif_qword_exec((int)e->kind, e->q);
            else { gs_vblank_exec(e->q[0], priv_snap[e->q[0] & 3]); atomic_store_explicit(&vb_done, e->q[0], memory_order_release); }
            if (((t + 1) & 1023) == 0) {   /* hand space back regularly */
                atomic_store_explicit(&r_tail, t + 1, memory_order_release);
                if (atomic_load(&prod_sleeping)) { pthread_mutex_lock(&gs_mx); pthread_cond_signal(&gs_cv_space); pthread_mutex_unlock(&gs_mx); }
            }
        }
        atomic_store_explicit(&r_tail, t, memory_order_release);
        if (atomic_load(&prod_sleeping)) { pthread_mutex_lock(&gs_mx); pthread_cond_signal(&gs_cv_space); pthread_mutex_unlock(&gs_mx); }
    }
    return 0;
}
static void wake_consumer(void) {
    if (atomic_load(&cons_sleeping)) { pthread_mutex_lock(&gs_mx); pthread_cond_signal(&gs_cv_data); pthread_mutex_unlock(&gs_mx); }
}
/* wait until cond() holds, sleeping on the space condition (signalled by the consumer as it makes progress) */
static void prod_wait(int (*cond)(void)) {
    wake_consumer();
    pthread_mutex_lock(&gs_mx); atomic_store(&prod_sleeping, 1);
    while (!cond()) { struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); ts.tv_nsec += 1000000; if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
        pthread_cond_timedwait(&gs_cv_space, &gs_mx, &ts); }
    atomic_store(&prod_sleeping, 0); pthread_mutex_unlock(&gs_mx);
}
static int ring_has_space(void) { return atomic_load(&r_head) - atomic_load(&r_tail) < RING - 1; }
static int ring_empty(void) { return atomic_load(&r_head) == atomic_load(&r_tail); }
static int lag_ok(void) { return atomic_load(&vb_sent) - atomic_load(&vb_done) <= 2; }
static inline void push(uint64_t kind, const uint64_t q[2]) {
    uint64_t h = atomic_load_explicit(&r_head, memory_order_relaxed);
    if (h - atomic_load_explicit(&r_tail, memory_order_acquire) >= RING - 1) prod_wait(ring_has_space);
    GsEnt *e = &ring[h & (RING - 1)]; e->q[0] = q[0]; e->q[1] = q[1]; e->kind = kind;
    atomic_store_explicit(&r_head, h + 1, memory_order_release);
    if (((h + 1) & 255) == 0) wake_consumer();
}
static void gs_sync(void) { if (mtgs) prod_wait(ring_empty); }
static void shadow_qword(int path, const uint64_t q[2]) {
    GifPath *g = &shadow[path & 3];
    if (!g->in_packet) { g->nloop = BITS(q[0], 0, 15); g->eop = BITS(q[0], 15, 1); g->flg = BITS(q[0], 58, 2); g->nreg = BITS(q[0], 60, 4); if (!g->nreg) g->nreg = 16;
        g->reg_i = 0; g->in_packet = g->nloop > 0; return; }
    switch (g->flg) {
    case 0: if (++g->reg_i == g->nreg) { g->reg_i = 0; if (--g->nloop == 0) g->in_packet = 0; } break;
    case 1: for (int k = 0; k < 2 && g->in_packet; k++) if (++g->reg_i == g->nreg) { g->reg_i = 0; if (--g->nloop == 0) g->in_packet = 0; } break;
    default: if (--g->nloop == 0) g->in_packet = 0;
    }
}
int gif_path_done(int path) { const GifPath *g = mtgs ? &shadow[path & 3] : &gpath[path & 3]; return !g->in_packet && g->eop; }
void gif_qword(int path, const uint64_t q[2]) {
    if (!mtgs) { gif_qword_exec(path, q); return; }
    shadow_qword(path, q); push((uint64_t)(path & 3), q);
}
int gs_display(uint32_t *out, int maxw, int maxh, int *pw, int *ph) {
    if (!mtgs) { gsw_drain(); return display_from(gspriv, out, maxw, maxh, pw, ph); }
    want_front = 1;
    pthread_mutex_lock(&front_mx);
    int ok = front_ok && front_w <= maxw && front_h <= maxh;
    if (ok) { for (int y = 0; y < front_h; y++) memcpy(out + y * front_w, front + y * front_w, (size_t)front_w * 4); *pw = front_w; *ph = front_h; }
    pthread_mutex_unlock(&front_mx);
    return ok;
}
void gs_vblank(uint64_t vb) {
    if (!mtgs) { gs_vblank_exec(vb, gspriv); return; }
    memcpy(priv_snap[vb & 3], gspriv, 0xB0);
    atomic_store(&vb_sent, vb);
    const uint64_t q[2] = { vb, 0 }; push(E_VBLANK, q); wake_consumer();
    if (!lag_ok()) prod_wait(lag_ok);
}
static void gs_vblank_exec(uint64_t vb, const uint8_t *priv) {
    gsw_drain();   /* the display and frame dumps read local memory */
    gs_nodraw = gs_draw_from < 0 || (uint64_t)gs_draw_from > vb;
    cur_vb = vb;
    { static int init; if (!init) { init = 1; const char *p = getenv("HG_PIXWATCH"); if (p) sscanf(p, "%d,%d,%ld", &watch_x, &watch_y, &watch_vb); } }
    if (mtgs && want_front) {
        static uint32_t tmp[1024 * 1024]; int w, h, ok = display_from(priv, tmp, 1024, 1024, &w, &h);
        pthread_mutex_lock(&front_mx); front_ok = ok; if (ok) { memcpy(front, tmp, (size_t)w * h * 4); front_w = w; front_h = h; } pthread_mutex_unlock(&front_mx);
    }
    static long every = -1; if (every < 0) every = getenv("HG_FRAME_EVERY") ? atol(getenv("HG_FRAME_EVERY")) : 60;
    if (every > 0 && vb % every == 0) dump_frame(vb, priv);
    static uint64_t last_log; if (vb - last_log >= 600) { last_log = vb; L("GS: VBlank %llu: %llu prims, %llu pixels, %llu GIF tags, %llu image qwords so far\n",
        (unsigned long long)vb, (unsigned long long)n_prims, (unsigned long long)gs_count(0), (unsigned long long)n_gif_tags, (unsigned long long)n_xfer_qw); }
    n_kicks_frame = 0;
}
#if defined(__x86_64__)
static void build_simd_tables(void) {
    for (int i = 0; i < 4; i++) for (int j = 0; j < 8; j++) bt8_i[i][j] = blockTable8[i][j];
    for (int i = 0; i < 16; i++) for (int j = 0; j < 16; j++) ct8_i[i][j] = columnTable8[i][j];
    for (int i = 0; i < 8; i++) for (int j = 0; j < 4; j++) bt4_i[i][j] = blockTable4[i][j];
    for (int i = 0; i < 16; i++) for (int j = 0; j < 32; j++) ct4_i[i][j] = columnTable4[i][j];
    for (int i = 0; i < 8; i++) for (int j = 0; j < 4; j++) { bt16_i[i][j] = blockTable16[i][j]; bt16s_i[i][j] = blockTable16S[i][j]; bt16z_i[i][j] = blockTable16Z[i][j]; }
    for (int i = 0; i < 8; i++) for (int j = 0; j < 16; j++) ct16_i[i][j] = columnTable16[i][j];
}
#endif
void gs_init(uint8_t *priv) {
    gspriv = priv; build_t4_table(); build_xoff();
#if defined(__x86_64__)
    build_simd_tables();
#endif
    if (getenv("HG_GSDRAW")) { gs_draw_from = atol(getenv("HG_GSDRAW")); if (gs_draw_from == 0) gs_draw_from = -1; gs_nodraw = 1; }
    const char *m = getenv("HG_MTGS");
    mtgs = m ? strcmp(m, "0") != 0 : host_ncpus() >= 2;
    if (mtgs) {
        ring = calloc(RING, sizeof *ring);
        if (!ring || pthread_create(&gs_thread, 0, gs_main, 0)) { L("GS: no GS thread, running single-threaded\n"); mtgs = 0; }
        else L("GS: running on its own thread (HG_MTGS=0 to disable)\n");
    }
    int bad = check_bijective(); if (bad) L("GS: WARNING: T%d column table is not a bijection\n", bad);
#if defined(__x86_64__)
    have_avx2 = __builtin_cpu_supports("avx2") && !(getenv("HG_GSSIMD") && !strcmp(getenv("HG_GSSIMD"), "0"));
#endif
    gsw_init();
}
void gs_dump_stats(void) {
    gs_sync();
#ifdef HAVE_VULKAN
    if (vkchk_from >= 0) L("GSVKCHECK: %llu sprites drawn on the GPU too, %llu identical, %llu differing words in all; %llu sprites not supported yet\n",
        (unsigned long long)vkchk_n, (unsigned long long)vkchk_exact, (unsigned long long)vkchk_words, (unsigned long long)vkchk_skipped);
    if (vkchk_from >= 0) L("GSVKCHECK: %llu triangles drawn on the GPU too, %llu identical, %llu differing words in all; %llu triangles not supported yet\n",
        (unsigned long long)vkt_n, (unsigned long long)vkt_exact, (unsigned long long)vkt_words, (unsigned long long)vkt_skipped);
#endif
#if defined(__x86_64__)
    if (gsw_n) { gsw_wait_seq(atomic_load(&gsq_head));
        L("GS workers: %d threads, %llu draws queued, %llu drawn serially (texture or Z on their own frame buffer), %llu waits for the workers; "
          "the per-primitive pixel counts below cover only the serial draws\n", gsw_n, (unsigned long long)n_gsw_issued, (unsigned long long)n_gsw_serial, (unsigned long long)n_gsw_drains); }
#endif
#if defined(__x86_64__)
    L("GS scalar-path draws (vector pipeline not used): format %llu, frame range %llu, Z overlaps frame %llu, texture overlaps frame %llu\n",
      (unsigned long long)st_scalar[0], (unsigned long long)st_scalar[1], (unsigned long long)st_scalar[2], (unsigned long long)st_scalar[3]);
#endif
    L("GS image transfers: ~%.1fs (sampled);", tprof[5]); for (int p = 0; p < 64; p++) if (st_xfer_psm[p]) L(" psm%02x %llu qw", p, (unsigned long long)st_xfer_psm[p]); L("\n");
    if (gs_sprhist_ptr) { struct { char k[160]; uint64_t px, n; double t; } *h = gs_sprhist_ptr;
        for (int r = 0; r < 12; r++) { int b = -1; for (int j = 0; j < gs_sprhist_n; j++) if (h[j].px && (b < 0 || h[j].px > h[b].px)) b = j;
            if (b < 0) break; L("  sprite cfg %s: %llu px in %llu sprites, %.2fs (%.1f ns/px)\n", h[b].k, (unsigned long long)h[b].px, (unsigned long long)h[b].n, h[b].t, h[b].t * 1e9 / (h[b].px ? h[b].px : 1)); h[b].px = 0; } }
    static const char *nm[3] = { "sprites", "triangles", "points/lines" };
    for (int k = 0; k < 3; k++) if (st_prims[k]) L("GS %s: %llu prims (%llu textured, %llu blended, %llu z-tested), %llu shaded pixels, %llu written, %.1fs\n", nm[k],
        (unsigned long long)st_prims[k], (unsigned long long)st_tex[k], (unsigned long long)st_blend[k], (unsigned long long)st_z[k], (unsigned long long)st_visits[k], (unsigned long long)st_px[k], st_time[k]);
    L("GS: %llu prims, %llu pixels, %llu GIF tags, %llu image qwords, %llu frames written\n", (unsigned long long)n_prims, (unsigned long long)gs_count(0),
      (unsigned long long)n_gif_tags, (unsigned long long)n_xfer_qw, (unsigned long long)frames_written);
}
