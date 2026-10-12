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
#include "ee_runtime.h"
#include "hle.h"

/* ================================================================== local memory and swizzling */
static uint32_t vm[1 << 20];                     /* 4 MB = 1M words */
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
static uint32_t xoff32[32][2048], xoff32z[32][2048];
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
static uint32_t clut[256];
static uint32_t cbp0, cbp1;
static uint8_t *gspriv;
static uint64_t n_prims, n_pixels, n_xfer_qw, n_kicks_frame, n_visits;
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
typedef struct { uint32_t tbp, tbw, psm, tw, th, tcc, tfx, csa, cpsm; int wms, wmt, minu, maxu, minv, maxv; int bilinear; int aem; uint8_t ta0, ta1; } TexState;
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
    case T8: case T8H: return clut[read_px(t->tbp, t->tbw, t->psm, u, v) & 255];
    case T4: case T4HL: case T4HH: return clut[((t->cpsm == CT32 ? (t->csa & 15) : t->csa) * 16 + read_px(t->tbp, t->tbw, t->psm, u, v)) & 255];
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
    d->fb16 = d->fpsm == CT16 || d->fpsm == CT16S;
    d->need_dst = d->abe || d->date || d->fbmsk || (d->ate && d->afail == 3) || d->fpsm == CT24 || d->fb16;
    d->zread = d->zte && d->ztst >= 2;
}
static inline int clamp255(int v, int cc) { return cc ? (v < 0 ? 0 : v > 255 ? 255 : v) : (v & 255); }
static inline __attribute__((always_inline)) void pixel(const Draw *d, int x, int y, uint32_t z, uint32_t col /* rgba8, a 0x80 = 1.0 */) {
    n_visits++;
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
    if (!d->fb16) fa = addr32(d->fbp, d->fbw, x, y);
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
        else if (d->fpsm == CT24) vm[fa] = (dst & 0xFF000000u) | (((out & ~d->fbmsk) | (dst & d->fbmsk)) & 0xFFFFFF);
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
        uint32_t fc = (uint32_t)r_fogcol, out = c & 0xFF000000u;
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
static void draw_sprite(const Draw *d, const Vtx *a, const Vtx *b) {
    int x0 = a->x, x1 = b->x, y0 = a->y, y1 = b->y;
    float u0 = 0, v0 = 0, u1 = 0, v1 = 0;
    if (d->tme) { vtx_uv(d, a, &u0, &v0); vtx_uv(d, b, &u1, &v1); }
    if (x0 > x1) { int t = x0; x0 = x1; x1 = t; float f = u0; u0 = u1; u1 = f; }
    if (y0 > y1) { int t = y0; y0 = y1; y1 = t; float f = v0; v0 = v1; v1 = f; }
    int px0 = (x0 + 15) >> 4, px1 = (x1 + 15) >> 4, py0 = (y0 + 15) >> 4, py1 = (y1 + 15) >> 4;
    if (px0 < d->scx0) px0 = d->scx0; if (px1 > d->scx1 + 1) px1 = d->scx1 + 1;
    if (py0 < d->scy0) py0 = d->scy0; if (py1 > d->scy1 + 1) py1 = d->scy1 + 1;
    float du = x1 != x0 ? (u1 - u0) / (x1 - x0) : 0, dv = y1 != y0 ? (v1 - v0) / (y1 - y0) : 0;
    for (int y = py0; y < py1; y++) {
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
    for (int y = miny; y <= maxy; y++) for (int x = minx; x <= maxx; x++) {
        float px = (float)x, py = (float)y;
        float w0 = ((x1 - px) * (y2 - py) - (x2 - px) * (y1 - py)) * inv;
        float w1 = ((x2 - px) * (y0 - py) - (x0 - px) * (y2 - py)) * inv;
        float w2 = 1.0f - w0 - w1;
        if (w0 < 0 || w1 < 0 || w2 < 0) {
            /* top-left rule approximation: accept tiny negative due to float error */
            if (w0 < -1e-6f || w1 < -1e-6f || w2 < -1e-6f) continue;
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
static void draw_line(const Draw *d, const Vtx *a, const Vtx *b) {
    int x0 = a->x >> 4, y0 = a->y >> 4, x1 = b->x >> 4, y1 = b->y >> 4;
    int dx = abs(x1 - x0), dy = abs(y1 - y0), n = dx > dy ? dx : dy;
    for (int i = 0; i <= n; i++) {
        float t = n ? (float)i / n : 0;
        int x = x0 + (int)lroundf((x1 - x0) * t), y = y0 + (int)lroundf((y1 - y0) * t);
        pixel(d, x, y, b->z, shade(d, b->rgba, 0, 0, b->f));
    }
}
static void vertex_kick(Vtx v, int draw) {
    Draw d; int prim = BITS(r_prim, 0, 3);
    static const int need[8] = { 1, 2, 2, 3, 3, 3, 2, 1 };
    vq[nvq++] = v;
    if (nvq < need[prim]) return;
    if (draw) {
        draw_setup(&d);
        uint64_t off = d.c->xyoffset; int ox = BITS(off, 0, 16), oy = BITS(off, 32, 16);
        Vtx w[3]; for (int i = 0; i < nvq; i++) { w[i] = vq[i]; w[i].x -= ox; w[i].y -= oy; }
        n_prims++; n_kicks_frame++;
        double t0 = host_now();
        uint64_t px0 = n_pixels, vis0 = n_visits;
        switch (prim) {
        case 0: pixel(&d, w[0].x >> 4, w[0].y >> 4, w[0].z, shade(&d, w[0].rgba, w[0].u, w[0].v, w[0].f)); break;
        case 1: case 2: draw_line(&d, &w[0], &w[1]); break;
        case 3: case 4: case 5: draw_triangle(&d, &w[0], &w[1], &w[2]); break;
        case 6: draw_sprite(&d, &w[0], &w[1]); break;
        }
        int k = prim == 6 ? 0 : (prim >= 3 && prim <= 5) ? 1 : 2;
        st_prims[k]++; st_px[k] += n_pixels - px0; st_visits[k] += n_visits - vis0; st_time[k] += host_now() - t0;
        if (d.tme) st_tex[k]++; if (d.abe) st_blend[k]++; if (d.zte) st_z[k]++;
        if (k == 0 && getenv("HG_SPRSTAT")) {   /* configuration histogram weighted by pixels */
            char key[160]; snprintf(key, sizeof key, "fb%02x tex%d:%02x bil%d tfx%d tcc%d abe%d(%d%d%d%d fix%d) ate%d:%d zte%d:%d zmsk%d fbmsk%08x",
                d.fpsm, d.tme, d.tme ? d.tex.psm : 0, d.tme ? d.tex.bilinear : 0, d.tme ? d.tex.tfx : 0, d.tme ? d.tex.tcc : 0, d.abe, d.a, d.b, d.cc, d.d, d.fix, d.ate, d.atst, d.zte, d.ztst, d.zmsk, d.fbmsk);
            static struct { char k[160]; uint64_t px, n; } h[256]; static int nh;
            int j = 0; for (; j < nh; j++) if (!strcmp(h[j].k, key)) break;
            if (j == nh && nh < 256) { strcpy(h[nh].k, key); nh++; }
            if (j < 256) { h[j].px += n_visits - vis0; h[j].n++; }
            static uint64_t calls; if (++calls % 100000 == 0 || getenv("HG_SPRSTAT")[0] == 'x') {}
            gs_sprhist_ptr = (void *)h; gs_sprhist_n = nh;
        }
        tprof[1] += host_now() - t0;
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
static void trxdir_write(uint64_t v) {
    int xdir = v & 3;
    if (xdir == 0) {
        xfer.active = 1; xfer.bp = BITS(r_bitbltbuf, 32, 14); xfer.bw = BITS(r_bitbltbuf, 48, 6); xfer.psm = BITS(r_bitbltbuf, 56, 6);
        xfer.sx = BITS(r_trxpos, 32, 11); xfer.sy = BITS(r_trxpos, 48, 11); xfer.w = BITS(r_trxreg, 0, 12); xfer.h = BITS(r_trxreg, 32, 12);
        xfer.x = xfer.y = 0; xfer.carry = 0; xfer.ncarry = 0;
        if (!xfer.w || !xfer.h) xfer.active = 0;
    } else if (xdir == 2) {        /* local -> local */
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
    case 0x61: if (gspriv) { uint32_t csr; memcpy(&csr, gspriv + 0x1000, 4); csr |= 2; memcpy(gspriv + 0x1000, &csr, 4); } break;   /* FINISH */
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
int gif_path_done(int path) { GifPath *g = &gpath[path & 3]; return !g->in_packet && g->eop; }
void gif_qword(int path, const uint64_t q[2]) {
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
    default:
        hwreg_write(q[0]); hwreg_write(q[1]); n_xfer_qw++;
        if (--g->nloop == 0) g->in_packet = 0;
    }
}

/* ================================================================== display output */
static uint64_t frames_written;
/* Copy the displayed picture (read circuit from PMODE, DISPFB/DISPLAY) into out as 0x00BBGGRR words.
   Returns 0 when nothing displayable is set up. */
int gs_display(uint32_t *out, int maxw, int maxh, int *pw, int *ph) {
    if (!gspriv) return 0;
    uint64_t pmode, dispfb, display;
    memcpy(&pmode, gspriv + 0x00, 8);
    int c2 = (pmode >> 1) & 1;
    memcpy(&dispfb, gspriv + (c2 ? 0x90 : 0x70), 8); memcpy(&display, gspriv + (c2 ? 0xa0 : 0x80), 8);
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
static void dump_frame(uint64_t vb) {
    const char *dir = getenv("HG_FRAMES"); if (!dir) return;
    static uint32_t buf[1024 * 1024]; int w, h;
    if (!gs_display(buf, 1024, 1024, &w, &h)) return;
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
void gs_vblank(uint64_t vb) {
    static long every = -1; if (every < 0) every = getenv("HG_FRAME_EVERY") ? atol(getenv("HG_FRAME_EVERY")) : 60;
    if (every > 0 && vb % every == 0) dump_frame(vb);
    static uint64_t last_log; if (vb - last_log >= 600) { last_log = vb; L("GS: VBlank %llu: %llu prims, %llu pixels, %llu GIF tags, %llu image qwords so far\n",
        (unsigned long long)vb, (unsigned long long)n_prims, (unsigned long long)n_pixels, (unsigned long long)n_gif_tags, (unsigned long long)n_xfer_qw); }
    n_kicks_frame = 0;
}
void gs_init(uint8_t *priv) {
    gspriv = priv; build_t4_table(); build_xoff();
    int bad = check_bijective(); if (bad) L("GS: WARNING: T%d column table is not a bijection\n", bad);
}
void gs_dump_stats(void) {
    if (gs_sprhist_ptr) { struct { char k[160]; uint64_t px, n; } *h = gs_sprhist_ptr;
        for (int r = 0; r < 12; r++) { int b = -1; for (int j = 0; j < gs_sprhist_n; j++) if (h[j].px && (b < 0 || h[j].px > h[b].px)) b = j;
            if (b < 0) break; L("  sprite cfg %s: %llu px in %llu sprites\n", h[b].k, (unsigned long long)h[b].px, (unsigned long long)h[b].n); h[b].px = 0; } }
    static const char *nm[3] = { "sprites", "triangles", "points/lines" };
    for (int k = 0; k < 3; k++) if (st_prims[k]) L("GS %s: %llu prims (%llu textured, %llu blended, %llu z-tested), %llu shaded pixels, %llu written, %.1fs\n", nm[k],
        (unsigned long long)st_prims[k], (unsigned long long)st_tex[k], (unsigned long long)st_blend[k], (unsigned long long)st_z[k], (unsigned long long)st_visits[k], (unsigned long long)st_px[k], st_time[k]);
    L("GS: %llu prims, %llu pixels, %llu GIF tags, %llu image qwords, %llu frames written\n", (unsigned long long)n_prims, (unsigned long long)n_pixels,
      (unsigned long long)n_gif_tags, (unsigned long long)n_xfer_qw, (unsigned long long)frames_written);
}
