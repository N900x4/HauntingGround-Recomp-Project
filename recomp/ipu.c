/* IPU (MPEG decoder unit) and its DMA channels 3 (IPU_FROM, output) and 4 (IPU_TO, input), written from the
   EE hardware documentation and ISO/IEC 11172-2 / 13818-2. The game's own CRI Sofdec player (recompiled EE code)
   drives it: BCLR, FDEC, VDEC (the four VLC tables), BDEC, SETIQ, SETVQ, SETTH and CSC.

   Model: the input FIFO (8 qwords) plus the 2-qword bit buffer are one queue of qwords with a bit pointer into the
   first one; IPU_BP/IPU_CTRL report FP = min(2, queued) and IFC = the rest (at most 8), so the "available bits"
   that the player computes (IFC*128 + FP*128 - BP) are exact. DMA channel 4 keeps the queue topped up as the
   hardware FIFO would, and is pulled further while a command needs more bits. A command that runs out of data
   is rolled back and stays BUSY until more data arrives (a new channel 4 transfer or FIFO writes).
   Output goes to an output queue that channel 3 drains to memory; OFC reports min(8, queued).
   Commands complete instantly otherwise (no IPU timing). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "ee_runtime.h"
#include "hle.h"

void hle_dma_complete(int ch);

/* ------------------------------------------------------------------ state */
#define INQ 4096                 /* qwords */
#define OUTQ 8192
static uint8_t inq[INQ][16]; static uint32_t in_head, in_count, bp;   /* bp: bit offset into inq[in_head] */
static uint8_t outq[OUTQ][16]; static uint32_t out_head, out_count;
static uint32_t ctrl_bits;       /* stored IPU_CTRL fields: IDP 16-17, AS 20, IVF 21, QST 22, MP1 23, PCT 24-26 */
static uint32_t cbp_reg, ecd, scd;
static uint32_t cmd_data, pending_cmd; static int has_pending;
static uint8_t iq_intra[64], iq_nonintra[64]; static uint16_t vqclut[16]; static uint32_t th0, th1;
static int dc_pred[3];
static uint8_t *hwr;             /* EE hardware register block (DMA registers live there) */
static uint64_t n_cmds[16], n_starved;
static int ipu_log = -1;
/* last events, printed at exit (debugging) */
static char evring[8192][120]; static unsigned evn;
#define EV(...) do { snprintf(evring[evn++ & 8191], 120, __VA_ARGS__); } while (0)
static void log_init(void) { if (ipu_log < 0) ipu_log = getenv("HG_IPULOG") ? atoi(getenv("HG_IPULOG")) : 0; }

/* ------------------------------------------------------------------ DMA channels 4 (input) and 3 (output) */
typedef struct { int active, chain, done_after, sp; uint32_t madr, qwc, tadr, asr[2]; } Dch;
static Dch ch4, ch3;
static uint32_t R32h(uint32_t off) { uint32_t v; memcpy(&v, hwr + off, 4); return v; }
static void W32h(uint32_t off, uint32_t v) { memcpy(hwr + off, &v, 4); }
static uint8_t *dmaptr(uint32_t a) { return (a & 0x80000000u) ? gptr(0x70000000u | (a & 0x3FF0)) : gptr(a & 0x01FFFFF0u); }
static void ch4_writeback(void) {
    W32h(0xB410, ch4.madr); W32h(0xB420, ch4.qwc); W32h(0xB430, ch4.tadr);
    uint32_t chcr = R32h(0xB400); chcr = ch4.active ? (chcr | 0x100) : (chcr & ~0x100u); W32h(0xB400, chcr);
}
static void ch3_writeback(void) {
    W32h(0xB010, ch3.madr); W32h(0xB020, ch3.qwc);
    uint32_t chcr = R32h(0xB000); chcr = ch3.active ? (chcr | 0x100) : (chcr & ~0x100u); W32h(0xB000, chcr);
}
/* next qword from channel 4, or 0 when it has finished */
static int ch4_pull(uint8_t q[16]) {
    while (ch4.active) {
        if (ch4.qwc) {
            memcpy(q, dmaptr(ch4.madr), 16); ch4.madr += 16; ch4.qwc--;
            if (!ch4.qwc && (!ch4.chain || ch4.done_after)) { ch4.active = 0; hle_dma_complete(4); }
            return 1;
        }
        if (!ch4.chain || ch4.done_after) { ch4.active = 0; hle_dma_complete(4); break; }
        uint64_t tag[2]; memcpy(tag, dmaptr(ch4.tadr), 16);
        uint32_t tq = tag[0] & 0xFFFF, id = (tag[0] >> 28) & 7, addr = (uint32_t)(tag[0] >> 32) & 0x7FFFFFF0u;
        if ((tag[0] >> 63) & 1) addr |= 0x80000000u;
        uint32_t chcr = R32h(0xB400); W32h(0xB400, (chcr & 0xFFFF) | (uint32_t)(tag[0] & 0xFFFF0000u));
        ch4.qwc = tq;
        switch (id) {
        case 0: ch4.madr = addr; ch4.tadr += 16; ch4.done_after = 1; break;                                /* refe */
        case 1: ch4.madr = ch4.tadr + 16; ch4.tadr = ch4.madr + tq * 16; break;                           /* cnt */
        case 2: ch4.madr = ch4.tadr + 16; ch4.tadr = addr; break;                                         /* next */
        case 3: case 4: ch4.madr = addr; ch4.tadr += 16; break;                                           /* ref, refs */
        case 5: ch4.madr = ch4.tadr + 16; if (ch4.sp < 2) ch4.asr[ch4.sp++] = ch4.madr + tq * 16; ch4.tadr = addr; break;   /* call */
        case 6: ch4.madr = ch4.tadr + 16; if (ch4.sp > 0) ch4.tadr = ch4.asr[--ch4.sp]; else ch4.done_after = 1; break;   /* ret */
        default: ch4.madr = ch4.tadr + 16; ch4.done_after = 1; break;                                     /* end */
        }
        if (((tag[0] >> 31) & 1) && ((R32h(0xB400) >> 7) & 1)) ch4.done_after = 1;                         /* IRQ with TIE */
        if (!ch4.qwc && ch4.done_after) { ch4.active = 0; hle_dma_complete(4); break; }
    }
    return 0;
}
static int in_push_from_dma(void) {
    if (in_count >= INQ) return 0;
    uint8_t q[16]; if (!ch4_pull(q)) return 0;
    memcpy(inq[(in_head + in_count) % INQ], q, 16); in_count++;
    return 1;
}
static void topup(void) { while (in_count < 10 && in_push_from_dma()) {} ch4_writeback(); }

static void out_push(const void *data, uint32_t nq) {
    const uint8_t *p = data;
    for (uint32_t i = 0; i < nq && out_count < OUTQ; i++) { memcpy(outq[(out_head + out_count) % OUTQ], p + 16 * i, 16); out_count++; }
}
static void drain_out(void) {
    while (ch3.active && out_count) {
        memcpy(dmaptr(ch3.madr), outq[out_head], 16); ch3.madr += 16; out_head = (out_head + 1) % OUTQ; out_count--;
        if (--ch3.qwc == 0) { ch3.active = 0; hle_dma_complete(3); }
    }
    if (ch3.active && !ch3.qwc) { ch3.active = 0; hle_dma_complete(3); }
    ch3_writeback();
}
static int in_retry;

/* ------------------------------------------------------------------ bit reader (with rollback) */
static uint32_t rb_head, rb_count, rb_bp;
static void save(void) { rb_head = in_head; rb_count = in_count; rb_bp = bp; }
static void restore(void) {   /* qwords pulled from DMA meanwhile stay queued after the restored ones */
    uint32_t consumed = (in_head + INQ - rb_head) % INQ;
    in_count += consumed; in_head = rb_head; bp = rb_bp; (void)rb_count;
}
static int ensure(uint32_t nbits) {   /* make nbits available from the current position */
    while (in_count * 128 < bp + nbits) if (!in_push_from_dma()) return 0;
    return 1;
}
static uint32_t avail_bits(void) { return in_count * 128 - bp; }
static int bit_at(uint32_t pos) {   /* pos counted from the start of inq[in_head] */
    const uint8_t *q = inq[(in_head + pos / 128) % INQ];
    uint32_t b = pos % 128;
    return (q[b >> 3] >> (7 - (b & 7))) & 1;
}
static int peek(uint32_t n, uint32_t *v) {   /* n <= 32 */
    if (!ensure(n)) return 0;
    uint32_t x = 0; for (uint32_t i = 0; i < n; i++) x = (x << 1) | bit_at(bp + i);
    *v = x; return 1;
}
static void skip(uint32_t n) {
    bp += n;
    while (bp >= 128 && in_count) { bp -= 128; in_head = (in_head + 1) % INQ; in_count--; }
}
static int getbits(uint32_t n, uint32_t *v) { if (!peek(n, v)) return 0; skip(n); return 1; }
static uint32_t peek_top(int *ok) { uint32_t v = 0; *ok = 0; if (avail_bits() >= 32) { peek(32, &v); *ok = 1; } else if (avail_bits()) { uint32_t n = avail_bits(); peek(n, &v); v <<= 32 - n; } return v; }

/* ------------------------------------------------------------------ VLC tables (ISO/IEC 11172-2 Annex B / 13818-2 Annex B) */
typedef struct { const char *code; int v1, v2; } Vlc;
#define VLC_END { 0, 0, 0 }
static const Vlc t_mbai[] = {
    {"1",1,0},{"011",2,0},{"010",3,0},{"0011",4,0},{"0010",5,0},{"00011",6,0},{"00010",7,0},{"0000111",8,0},{"0000110",9,0},
    {"00001011",10,0},{"00001010",11,0},{"00001001",12,0},{"00001000",13,0},{"00000111",14,0},{"00000110",15,0},
    {"0000010111",16,0},{"0000010110",17,0},{"0000010101",18,0},{"0000010100",19,0},{"0000010011",20,0},{"0000010010",21,0},
    {"00000100011",22,0},{"00000100010",23,0},{"00000100001",24,0},{"00000100000",25,0},{"00000011111",26,0},{"00000011110",27,0},
    {"00000011101",28,0},{"00000011100",29,0},{"00000011011",30,0},{"00000011010",31,0},{"00000011001",32,0},{"00000011000",33,0},
    {"00000001111",0x22,0},   /* macroblock_stuffing (MPEG-1) */
    {"00000001000",0x23,0},   /* macroblock_escape */
    VLC_END };
/* macroblock_type: 0x10 quant, 0x08 motion forward, 0x04 motion backward, 0x02 pattern, 0x01 intra */
static const Vlc t_mbt_i[] = { {"1",0x01,0},{"01",0x11,0}, VLC_END };
static const Vlc t_mbt_p[] = { {"1",0x0A,0},{"01",0x02,0},{"001",0x08,0},{"00011",0x01,0},{"00010",0x1A,0},{"00001",0x12,0},{"000001",0x11,0}, VLC_END };
static const Vlc t_mbt_b[] = { {"10",0x0C,0},{"11",0x0E,0},{"010",0x04,0},{"011",0x06,0},{"0010",0x08,0},{"0011",0x0A,0},{"00011",0x01,0},
    {"00010",0x1E,0},{"000011",0x1A,0},{"000010",0x16,0},{"000001",0x11,0}, VLC_END };
static const Vlc t_mbt_d[] = { {"1",0x01,0}, VLC_END };
static const Vlc t_mc[] = {   /* motion_code magnitude, sign bit follows (except 0) */
    {"1",0,0},{"01",1,0},{"001",2,0},{"0001",3,0},{"000011",4,0},{"0000101",5,0},{"0000100",6,0},{"0000011",7,0},
    {"000001011",8,0},{"000001010",9,0},{"000001001",10,0},{"0000010001",11,0},{"0000010000",12,0},{"0000001111",13,0},
    {"0000001110",14,0},{"0000001101",15,0},{"0000001100",16,0}, VLC_END };
static const Vlc t_dmv[] = { {"0",0,0},{"10",1,0},{"11",-1,0}, VLC_END };
static const Vlc t_cbp[] = {
    {"111",60,0},{"1101",4,0},{"1100",8,0},{"1011",16,0},{"1010",32,0},{"10011",12,0},{"10010",48,0},{"10001",20,0},{"10000",40,0},
    {"01111",28,0},{"01110",44,0},{"01101",52,0},{"01100",56,0},{"01011",1,0},{"01010",61,0},{"01001",2,0},{"01000",62,0},
    {"001111",24,0},{"001110",36,0},{"001101",3,0},{"001100",63,0},{"0010111",5,0},{"0010110",9,0},{"0010101",17,0},{"0010100",33,0},
    {"0010011",6,0},{"0010010",10,0},{"0010001",18,0},{"0010000",34,0},{"00011111",7,0},{"00011110",11,0},{"00011101",19,0},
    {"00011100",35,0},{"00011011",13,0},{"00011010",49,0},{"00011001",21,0},{"00011000",41,0},{"00010111",14,0},{"00010110",50,0},
    {"00010101",22,0},{"00010100",42,0},{"00010011",15,0},{"00010010",51,0},{"00010001",23,0},{"00010000",43,0},{"00001111",25,0},
    {"00001110",37,0},{"00001101",26,0},{"00001100",38,0},{"00001011",29,0},{"00001010",45,0},{"00001001",53,0},{"00001000",57,0},
    {"00000111",30,0},{"00000110",46,0},{"00000101",54,0},{"00000100",58,0},{"000000111",31,0},{"000000110",47,0},{"000000101",55,0},
    {"000000100",59,0},{"000000011",27,0},{"000000010",39,0},{"000000001",0,0}, VLC_END };
static const Vlc t_dcl[] = { {"100",0,0},{"00",1,0},{"01",2,0},{"101",3,0},{"110",4,0},{"1110",5,0},{"11110",6,0},{"111110",7,0},{"1111110",8,0},
    {"11111110",9,0},{"111111110",10,0},{"111111111",11,0}, VLC_END };
static const Vlc t_dcc[] = { {"00",0,0},{"01",1,0},{"10",2,0},{"110",3,0},{"1110",4,0},{"11110",5,0},{"111110",6,0},{"1111110",7,0},{"11111110",8,0},
    {"111111110",9,0},{"1111111110",10,0},{"1111111111",11,0}, VLC_END };
/* DCT coefficients, table B.14 (run, level); sign bit follows. "10" is end of block (v1 = -1), "000001" escape (v1 = -2).
   The first coefficient of a non-intra block uses "1" for (0,1) instead (handled in the block decoder). */
static const Vlc t_dct0[] = {
    {"10",-1,0},{"000001",-2,0},{"11",0,1},{"011",1,1},{"0100",0,2},{"0101",2,1},{"00101",0,3},{"00111",3,1},{"00110",4,1},
    {"000110",1,2},{"000111",5,1},{"000101",6,1},{"000100",7,1},{"0000110",0,4},{"0000100",2,2},{"0000111",8,1},{"0000101",9,1},
    {"00100110",0,5},{"00100001",0,6},{"00100101",1,3},{"00100100",3,2},{"00100111",10,1},{"00100011",11,1},{"00100010",12,1},{"00100000",13,1},
    {"0000001010",0,7},{"0000001100",1,4},{"0000001011",2,3},{"0000001111",4,2},{"0000001001",5,2},{"0000001110",14,1},{"0000001101",15,1},{"0000001000",16,1},
    {"000000011101",0,8},{"000000011000",0,9},{"000000010011",0,10},{"000000010000",0,11},{"000000011011",1,5},{"000000010100",2,4},
    {"000000011100",3,3},{"000000010010",4,3},{"000000011110",6,2},{"000000010101",7,2},{"000000010001",8,2},{"000000011111",17,1},
    {"000000011010",18,1},{"000000011001",19,1},{"000000010111",20,1},{"000000010110",21,1},
    {"0000000011010",0,12},{"0000000011001",0,13},{"0000000011000",0,14},{"0000000010111",0,15},{"0000000010110",1,6},{"0000000010101",1,7},
    {"0000000010100",2,5},{"0000000010011",3,4},{"0000000010010",5,3},{"0000000010001",9,2},{"0000000010000",10,2},{"0000000011111",22,1},
    {"0000000011110",23,1},{"0000000011101",24,1},{"0000000011100",25,1},{"0000000011011",26,1},
    {"00000000011111",0,16},{"00000000011110",0,17},{"00000000011101",0,18},{"00000000011100",0,19},{"00000000011011",0,20},{"00000000011010",0,21},
    {"00000000011001",0,22},{"00000000011000",0,23},{"00000000010111",0,24},{"00000000010110",0,25},{"00000000010101",0,26},{"00000000010100",0,27},
    {"00000000010011",0,28},{"00000000010010",0,29},{"00000000010001",0,30},{"00000000010000",0,31},
    {"000000000011000",0,32},{"000000000010111",0,33},{"000000000010110",0,34},{"000000000010101",0,35},{"000000000010100",0,36},
    {"000000000010011",0,37},{"000000000010010",0,38},{"000000000010001",0,39},{"000000000010000",0,40},{"000000000011111",1,8},
    {"000000000011110",1,9},{"000000000011101",1,10},{"000000000011100",1,11},{"000000000011011",1,12},{"000000000011010",1,13},{"000000000011001",1,14},
    {"0000000000010011",1,15},{"0000000000010010",1,16},{"0000000000010001",1,17},{"0000000000010000",1,18},{"0000000000010100",6,3},
    {"0000000000011010",11,2},{"0000000000011001",12,2},{"0000000000011000",13,2},{"0000000000010111",14,2},{"0000000000010110",15,2},
    {"0000000000010101",16,2},{"0000000000011111",27,1},{"0000000000011110",28,1},{"0000000000011101",29,1},{"0000000000011100",30,1},
    {"0000000000011011",31,1}, VLC_END };

/* DCT coefficients, table B.15 (MPEG-2 intra blocks with intra_vlc_format = 1): EOB is "0110"; the codes of
   14 bits and more are those of table B.14 */
static const Vlc t_dct1_short[] = {
    {"0110",-1,0},{"000001",-2,0},{"10",0,1},{"010",1,1},{"110",0,2},{"00101",2,1},{"0111",0,3},{"00111",3,1},{"000110",4,1},
    {"00110",1,2},{"000111",5,1},{"0000110",6,1},{"0000100",7,1},{"11100",0,4},{"0000111",2,2},{"0000101",8,1},{"1111000",9,1},
    {"11101",0,5},{"000101",0,6},{"1111001",1,3},{"00100110",3,2},{"1111010",10,1},{"00100001",11,1},{"00100101",12,1},{"00100100",13,1},
    {"000100",0,7},{"00100111",1,4},{"11111100",2,3},{"11111101",4,2},{"000000100",5,2},{"000000101",14,1},{"000000111",15,1},{"0000001101",16,1},
    {"1111011",0,8},{"1111100",0,9},{"00100011",0,10},{"00100010",0,11},{"00100000",1,5},{"0000001100",2,4},
    {"000000011100",3,3},{"000000010010",4,3},{"000000011110",6,2},{"000000010101",7,2},{"000000010001",8,2},{"000000011111",17,1},
    {"000000011010",18,1},{"000000011001",19,1},{"000000010111",20,1},{"000000010110",21,1},
    {"11111010",0,12},{"11111011",0,13},{"11111110",0,14},{"11111111",0,15},
    {"0000000010110",1,6},{"0000000010101",1,7},{"0000000010100",2,5},{"0000000010011",3,4},{"0000000010010",5,3},{"0000000010001",9,2},
    {"0000000010000",10,2},{"0000000011111",22,1},{"0000000011110",23,1},{"0000000011101",24,1},{"0000000011100",25,1},{"0000000011011",26,1},
    VLC_END };

/* compiled form: lookup by (length, code) */
typedef struct { uint16_t code; uint8_t len; int16_t v1, v2; } VlcE;
typedef struct { VlcE e[160]; int n, maxlen; } VlcT;
static VlcT c_mbai, c_mbt[5], c_mc, c_dmv, c_cbp, c_dcl, c_dcc, c_dct0, c_dct1;
static void compile(VlcT *t, const Vlc *src) {
    t->n = 0; t->maxlen = 0;
    for (; src->code; src++) {
        VlcE *e = &t->e[t->n++]; e->len = (uint8_t)strlen(src->code); e->code = 0;
        for (int i = 0; i < e->len; i++) e->code = (uint16_t)((e->code << 1) | (src->code[i] == '1'));
        e->v1 = (int16_t)src->v1; e->v2 = (int16_t)src->v2; if (e->len > t->maxlen) t->maxlen = e->len;
    }
}
/* decode one code: 1 ok, 0 out of data, -1 no match */
static int vlc(const VlcT *t, const VlcE **out) {
    uint32_t w;
    if (!peek((uint32_t)t->maxlen, &w)) {   /* near the end of the data: try the codes that fit */
        uint32_t a = avail_bits(); if (!a) return 0;
        if (a > 32) a = 32;
        peek(a, &w); w <<= (t->maxlen > (int)a) ? t->maxlen - a : 0;
        for (int i = 0; i < t->n; i++) { const VlcE *e = &t->e[i]; if (e->len <= (int)a && (w >> (t->maxlen - e->len)) == e->code) { skip(e->len); *out = e; return 1; } }
        return 0;
    }
    for (int i = 0; i < t->n; i++) { const VlcE *e = &t->e[i]; if ((w >> (t->maxlen - e->len)) == e->code) { skip(e->len); *out = e; return 1; } }
    return -1;
}

/* ------------------------------------------------------------------ block decoding */
static const uint8_t zigzag[64] = { 0,1,8,16,9,2,3,10,17,24,32,25,18,11,4,5,12,19,26,33,40,48,41,34,27,20,13,6,7,14,21,28,35,42,49,56,57,50,43,36,29,22,15,23,30,37,44,51,58,59,52,45,38,31,39,46,53,60,61,54,47,55,62,63 };
static const uint8_t altscan[64] = { 0,8,16,24,1,9,2,10,17,25,32,40,48,56,57,49,41,33,26,18,3,11,4,12,19,27,34,42,50,58,35,43,51,59,20,28,5,13,6,14,21,29,36,44,52,60,37,45,53,61,22,30,7,15,23,31,38,46,54,62,39,47,55,63 };
static const uint8_t nonlinear_q[32] = { 0,1,2,3,4,5,6,7,8,10,12,14,16,18,20,22,24,28,32,36,40,44,48,52,56,64,72,80,88,96,104,112 };
static double idct_c[8][8];
static void idct(const int32_t in[64], int16_t out[64], int lo, int hi) {   /* reference IDCT (double precision, rounded), output saturated */
    double tmp[64];
    for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++) { double s = 0; for (int u = 0; u < 8; u++) s += idct_c[x][u] * in[y * 8 + u]; tmp[y * 8 + x] = s; }
    for (int x = 0; x < 8; x++) for (int y = 0; y < 8; y++) {
        double s = 0; for (int v = 0; v < 8; v++) s += idct_c[y][v] * tmp[v * 8 + x];
        long r = lround(s); if (r < lo) r = lo; if (r > hi) r = hi; out[y * 8 + x] = (int16_t)r;
    }
}
/* inverse quantisation; qscale is quantiser_scale (2*q for the linear scale). Intra: level*Q*W/16, non-intra:
   (2*level + sign)*Q*W/32, both truncated towards zero; MPEG-1 makes non-zero values odd; result saturated */
static int dequant(int level, int intra, int qscale, int w, int mp1) {
    int v = intra ? (level * qscale * w) / 16 : ((2 * level + (level > 0 ? 1 : -1)) * qscale * w) / 32;
    if (mp1 && v != 0 && !(v & 1)) v -= v > 0 ? 1 : -1;
    return v < -2048 ? -2048 : v > 2047 ? 2047 : v;
}
/* returns 1 ok, 0 out of data, -1 bitstream error */
static int decode_block(int intra, int cc, int qscale, int32_t coef[64]) {
    const int mp1 = (ctrl_bits >> 23) & 1, idp = (ctrl_bits >> 16) & 3;
    const uint8_t *scan = ((ctrl_bits >> 20) & 1) ? altscan : zigzag;
    memset(coef, 0, 64 * sizeof coef[0]);
    int i = 0;
    if (intra) {
        const VlcE *e; int r = vlc(cc ? &c_dcc : &c_dcl, &e); if (r <= 0) return r;
        int size = e->v1, diff = 0;
        if (size) { uint32_t b; if (!getbits(size, &b)) return 0; diff = (b >> (size - 1)) ? (int)b : (int)b - (1 << size) + 1; }
        dc_pred[cc] += diff;
        coef[0] = dc_pred[cc] << (3 - idp);
        i = 1;
    } else {   /* first coefficient: "1s" is (0, 1) */
        uint32_t b; if (!peek(1, &b)) return 0;
        if (b) { uint32_t s; if (!getbits(2, &s)) return 0; int lv = (s & 1) ? -1 : 1;
            int v = dequant(lv, 0, qscale, iq_nonintra[0], mp1);
            coef[scan[0]] = v; i = 1; }
    }
    int sum = coef[0];
    for (;;) {
        const VlcE *e; int r = vlc(intra && ((ctrl_bits >> 21) & 1) ? &c_dct1 : &c_dct0, &e); if (r <= 0) return r;
        int run, level;
        if (e->v1 == -1) break;                                   /* end of block */
        if (e->v1 == -2) {                                        /* escape */
            uint32_t rr; if (!getbits(6, &rr)) return 0; run = (int)rr;
            if (mp1) {
                uint32_t l8; if (!getbits(8, &l8)) return 0;
                if (l8 == 0) { uint32_t l; if (!getbits(8, &l)) return 0; level = (int)l; }
                else if (l8 == 128) { uint32_t l; if (!getbits(8, &l)) return 0; level = (int)l - 256; }
                else level = (int8_t)l8;
            } else { uint32_t l; if (!getbits(12, &l)) return 0; level = (l & 0x800) ? (int)l - 4096 : (int)l; }
        } else { uint32_t s; if (!getbits(1, &s)) return 0; run = e->v1; level = s ? -e->v2 : e->v2; }
        i += run;
        if (i > 63) { if (ipu_log) L("IPU: block coefficient index %d > 63 (run %d level %d)\n", i, run, level); return -1; }
        const int pos = scan[i];
        const int v = dequant(level, intra, qscale, intra ? iq_intra[i] : iq_nonintra[i], mp1);   /* matrices are kept in scan order */
        coef[pos] = v; sum += v;
        i++;
    }
    if (!mp1 && !(sum & 1)) coef[63] ^= 1;                        /* MPEG-2 mismatch control */
    return 1;
}

/* ------------------------------------------------------------------ commands */
static void set_scd(void) {   /* start code prefix (23 zero bits) ahead */
    uint32_t v; scd = 0;
    if (avail_bits() >= 24 && peek(24, &v) && v == 1) scd = 1;
}
static int qscale_of(uint32_t qsc) { return ((ctrl_bits >> 22) & 1) ? nonlinear_q[qsc & 31] : (int)(qsc & 31) * 2; }

static int do_bdec(uint32_t c) {
    if (ipu_log && !((c >> 27) & 1)) { static int m; if (m++ < 3) { L("IPU: non-intra BDEC 0x%08x bits:", c); for (uint32_t k = 0; k < 512 && k + 32 <= avail_bits(); k += 32) { uint32_t w = 0; for (int i = 0; i < 32; i++) w = (w << 1) | bit_at(bp + k + i); L(" %08x", w); } L("\n"); } }
    if (ipu_log) { static int n; if (n++ < 2) { L("IPU: BDEC 0x%08x bits:", c); for (uint32_t k = 0; k < 256 && k + 32 <= avail_bits(); k += 32) { uint32_t w = 0; for (int i = 0; i < 32; i++) w = (w << 1) | bit_at(bp + k + i); L(" %08x", w); } L("\n"); } }
    const int intra = (c >> 27) & 1, dcr = (c >> 26) & 1, dt = (c >> 25) & 1, qs = qscale_of((c >> 16) & 31);
    int saved_pred[3]; memcpy(saved_pred, dc_pred, sizeof dc_pred);
    skip(c & 63);
    if (dcr) { int idp = (ctrl_bits >> 16) & 3; dc_pred[0] = dc_pred[1] = dc_pred[2] = 128 << idp; }
    int cbp = 0x3F;
    if (!intra) { const VlcE *e; int r = vlc(&c_cbp, &e); if (r <= 0) { memcpy(dc_pred, saved_pred, sizeof dc_pred); return r; } cbp = e->v1; }
    static int16_t mb[384]; memset(mb, 0, sizeof mb);
    for (int b = 0; b < 6; b++) {
        if (!(cbp & (1 << (5 - b)))) continue;
        int32_t coef[64]; int16_t pix[64];
        int r = decode_block(intra, b < 4 ? 0 : b - 3, qs, coef);
        if (r <= 0) { memcpy(dc_pred, saved_pred, sizeof dc_pred); return r; }
        if (intra) idct(coef, pix, 0, 255); else idct(coef, pix, -256, 255);
        if (b < 4) {
            int x0 = (b & 1) * 8, y0 = dt ? (b >> 1) : (b >> 1) * 8, ys = dt ? 2 : 1;
            for (int y = 0; y < 8; y++) memcpy(&mb[(y0 + y * ys) * 16 + x0], &pix[y * 8], 16);
        } else memcpy(&mb[256 + (b - 4) * 64], pix, 128);
    }
    cbp_reg = (uint32_t)cbp;
    if (ipu_log) { static int n; if (n++ == 300) { L("IPU: BDEC 0x%08x output (intra %d cbp %02x) Y row0:", c, intra, cbp); for (int i = 0; i < 16; i++) L(" %d", mb[i]); L(" | Cb0: %d %d | Cr0: %d %d\n", mb[256], mb[257], mb[320], mb[321]); } }
    out_push(mb, 48);
    return 1;
}
static int do_vdec(uint32_t c) {
    skip(c & 63);
    const int tbl = (c >> 26) & 3;
    const VlcE *e; int r;
    switch (tbl) {
    case 0: r = vlc(&c_mbai, &e); if (r < 0) { cmd_data = 0; return 2; } if (!r) return 0; cmd_data = (uint32_t)e->v1 | ((uint32_t)e->len << 16); return 1;
    case 1: { int pct = (ctrl_bits >> 24) & 7; const VlcT *t = &c_mbt[pct >= 1 && pct <= 4 ? pct : 1];
        r = vlc(t, &e); if (r < 0) { cmd_data = 0; return 2; } if (!r) return 0; cmd_data = (uint32_t)e->v1 | ((uint32_t)e->len << 16); return 1; }
    case 2: { r = vlc(&c_mc, &e); if (r < 0) { cmd_data = 0; return 2; } if (!r) return 0;
        int v = e->v1, len = e->len;
        if (v) { uint32_t s; if (!getbits(1, &s)) return 0; if (s) v = -v; len++; }
        cmd_data = ((uint32_t)v & 0xFFFF) | ((uint32_t)len << 16); return 1; }
    default: r = vlc(&c_dmv, &e); if (r < 0) { cmd_data = 0; return 2; } if (!r) return 0; cmd_data = ((uint32_t)e->v1 & 0xFFFF) | ((uint32_t)e->len << 16); return 1;
    }
}
/* CSC runs macroblock by macroblock and stalls (stays BUSY) when the output queue is full or the input runs dry,
   like the hardware with its FIFOs; it resumes when channel 3 drains output or more input arrives */
static int csc_left = -1;
static int do_csc(uint32_t c) {
    const int ofm = (c >> 27) & 1;
    if (csc_left < 0) csc_left = c & 0x7FF;
    for (; csc_left > 0; csc_left--) {
        if (out_count + 64 > 1024) { drain_out(); if (out_count + 64 > 1024) return 0; }
        if (!ensure(384 * 8)) return 0;
        uint8_t raw[384]; for (int i = 0; i < 384; i++) { uint32_t v; getbits(8, &v); raw[i] = (uint8_t)v; }
        {   /* HG_IPUDUMP=dir: the YCbCr macroblocks of each CSC command, one file per command (debugging) */
            static const char *dd; static int init; static FILE *df; static int ncsc;
            if (!init) { init = 1; dd = getenv("HG_IPUDUMP"); }
            if (dd) {
                if (csc_left == (int)(c & 0x7FF)) { if (df) fclose(df); char p[512]; snprintf(p, sizeof p, "%s/csc_%04d.raw", dd, ncsc++); df = fopen(p, "wb"); }
                if (df) fwrite(raw, 1, 384, df);
            }
        }
        if (ipu_log) { static uint64_t nmb, nzero_c, nzero_y; nmb++; int zc = 1, zy = 1; for (int i = 256; i < 384; i++) if (raw[i]) zc = 0; for (int i = 0; i < 256; i++) if (raw[i]) zy = 0;
            nzero_c += zc; nzero_y += zy; if ((nmb % 20000) == 0) L("IPU: CSC input stats: %llu MBs, %llu zero chroma, %llu zero luma\n", (unsigned long long)nmb, (unsigned long long)nzero_c, (unsigned long long)nzero_y); }
        if (ipu_log) { static int n; if (n++ == 3000) { L("IPU: CSC input Y row0:"); for (int i = 0; i < 16; i++) L(" %d", raw[i]); L(" | Cb %d %d Cr %d %d\n", raw[256], raw[257], raw[320], raw[321]); } }
        uint32_t px[256];
        for (int y = 0; y < 16; y++) for (int x = 0; x < 16; x++) {
            double Y = 1.164 * (raw[y * 16 + x] - 16), cb = raw[256 + (y / 2) * 8 + x / 2] - 128.0, cr = raw[320 + (y / 2) * 8 + x / 2] - 128.0;
            long r = lround(Y + 1.596 * cr), g = lround(Y - 0.813 * cr - 0.391 * cb), b = lround(Y + 2.018 * cb);
            r = r < 0 ? 0 : r > 255 ? 255 : r; g = g < 0 ? 0 : g > 255 ? 255 : g; b = b < 0 ? 0 : b > 255 ? 255 : b;
            uint32_t a = 0x80;
            if ((uint32_t)r < th0 && (uint32_t)g < th0 && (uint32_t)b < th0) { a = 0; r = g = b = 0; }
            else if ((uint32_t)r < th1 && (uint32_t)g < th1 && (uint32_t)b < th1) a = 0x40;
            px[y * 16 + x] = (uint32_t)r | (uint32_t)g << 8 | (uint32_t)b << 16 | a << 24;
        }
        if (!ofm) out_push(px, 64);
        else { uint16_t p16[256]; for (int i = 0; i < 256; i++) { uint32_t v = px[i]; p16[i] = (uint16_t)(((v >> 3) & 31) | (((v >> 11) & 31) << 5) | (((v >> 19) & 31) << 10) | ((v >> 31) << 15) | (((v >> 30) & 1) << 15)); } out_push(p16, 32); }
        save();
    }
    csc_left = -1;
    return 1;
}
/* run the pending command; returns 1 when finished */
static int exec(void) {
    const uint32_t c = pending_cmd; const int code = c >> 28;
    save();
    int r = 1;
    switch (code) {
    case 0: in_count = 0; bp = c & 0x7F; topup(); break;                                                 /* BCLR */
    case 2: r = do_bdec(c); break;                                                                       /* BDEC */
    case 3: r = do_vdec(c); break;                                                                       /* VDEC */
    case 4: { uint32_t v; if (!ensure((c & 63) + 32)) { r = 0; break; } skip(c & 63); peek(32, &v); cmd_data = v; break; }   /* FDEC */
    case 5: { skip(c & 63); if (!ensure(512)) { r = 0; break; }                                          /* SETIQ */
        uint8_t *m = (c >> 27) & 1 ? iq_nonintra : iq_intra; for (int i = 0; i < 64; i++) { uint32_t v; getbits(8, &v); m[i] = (uint8_t)v; } break; }
    case 6: { if (!ensure(256)) { r = 0; break; }                                                         /* SETVQ */
        for (int i = 0; i < 16; i++) { uint32_t lo, hi; getbits(8, &lo); getbits(8, &hi); vqclut[i] = (uint16_t)(lo | hi << 8); } break; }
    case 7: r = do_csc(c); break;                                                                        /* CSC */
    case 9: th0 = c & 0x1FF; th1 = (c >> 16) & 0x1FF; break;                                             /* SETTH */
    case 1: case 8: default: { static int warned; if (warned++ < 10) L("IPU: unsupported command 0x%08x\n", c); break; }   /* IDEC, PACK */
    }
    if (r == 0) { if (code != 7) restore(); n_starved++; return 0; }
    ecd = r == 2 || r < 0;
    if (ecd && ipu_log) { uint32_t w = 0; peek(32, &w); L("IPU: command 0x%08x: bitstream error (r=%d) at bit %u of the queue, next bits %08x; from the command start:", c, r, bp, w);
        uint32_t h = in_head, cnt = in_count, b0 = bp; restore(); for (uint32_t k = 0; k < 384 && k + 32 <= avail_bits(); k += 32) { uint32_t x = 0; for (int i = 0; i < 32; i++) x = (x << 1) | bit_at(bp + k + i); L(" %08x", x); }
        L("\n"); in_head = h; in_count = cnt; bp = b0; }
    if (r < 0) { restore(); cmd_data = 0; }
    has_pending = 0; n_cmds[code & 15]++;
    set_scd(); topup(); drain_out();
    return 1;
}
static void retry(void) { if (has_pending && !in_retry) { in_retry = 1; topup(); exec(); in_retry = 0; } }

/* ------------------------------------------------------------------ registers */
static uint32_t ctrl_value(void) {
    uint32_t fp = in_count < 2 ? in_count : 2, ifc = in_count - fp; if (ifc > 8) ifc = 8;
    uint32_t ofc = out_count > 8 ? 8 : out_count;
    return ifc | ofc << 4 | (cbp_reg & 63) << 8 | ecd << 14 | scd << 15 | ctrl_bits | (uint32_t)has_pending << 31;
}
uint64_t ipu_read(uint32_t pa, int size) {
    (void)size;
    retry();
    switch (pa) {
    case 0x10002000: return (uint64_t)cmd_data | (uint64_t)has_pending << 63;
    case 0x10002010: return ctrl_value();
    case 0x10002020: { uint32_t fp = in_count < 2 ? in_count : 2, ifc = in_count - fp; if (ifc > 8) ifc = 8; return (bp & 127) | ifc << 8 | fp << 16; }
    case 0x10002030: { int ok; uint32_t v = peek_top(&ok); return (uint64_t)v | (uint64_t)(!ok || has_pending) << 63; }
    case 0x10007000: case 0x10007008: {   /* output FIFO (CPU read) */
        static uint8_t cur[16]; if ((pa & 8) == 0) { if (out_count) { memcpy(cur, outq[out_head], 16); out_head = (out_head + 1) % OUTQ; out_count--; } else memset(cur, 0, 16); }
        uint64_t v; memcpy(&v, cur + (pa & 8), 8); return v; }
    }
    return 0;
}
void ipu_write(uint32_t pa, uint64_t v, int size) {
    (void)size;
    if (ipu_log < 0) ipu_log = getenv("HG_IPULOG") ? atoi(getenv("HG_IPULOG")) : 0;
    switch (pa) {
    case 0x10002000:
        if (ipu_log) { static int n; const int code = (uint32_t)v >> 28; if ((code != 2 && code != 3 && code != 4) || ipu_log > 1) if (n++ < 4000) L("IPU cmd %08x (avail %u bits, out %u qw, in 0x%x)\n", (uint32_t)v, avail_bits(), out_count, rec_cur); }
        if (has_pending) { static int w; if (w++ < 10) L("IPU: command 0x%08x while 0x%08x is pending (dropped)\n", (uint32_t)v, pending_cmd); }
        EV("cmd %08x avail %u out %u ch3 %d/%x", (uint32_t)v, avail_bits(), out_count, ch3.active, ch3.qwc);
        pending_cmd = (uint32_t)v; has_pending = 1; topup(); exec();
        EV("  -> data %08x pending %d ecd %u out %u ch3 %d/%x", cmd_data, has_pending, ecd, out_count, ch3.active, ch3.qwc);
        if (ipu_log > 1) { static int n; if (n++ < 2000) L("   -> data %08x busy %d bp %u\n", cmd_data, has_pending, bp); }
        break;
    case 0x10002010:
        if (v & 0x40000000) { in_count = 0; bp = 0; out_count = 0; has_pending = 0; ecd = scd = 0; cbp_reg = 0; cmd_data = 0; csc_left = -1; }
        ctrl_bits = (uint32_t)v & 0x07F30000u;
        if (ipu_log) { static int n; if (n++ < 200) L("IPU ctrl write %08x\n", (uint32_t)v); }
        break;
    case 0x10007010: case 0x10007018: {   /* input FIFO (CPU write) */
        static uint8_t cur[16]; memcpy(cur + (pa & 8), &v, 8);
        if (pa & 8) { if (in_count < INQ) { memcpy(inq[(in_head + in_count) % INQ], cur, 16); in_count++; } retry(); }
        break; }
    }
}
/* channel 3/4 start (CHCR written with STR); hw = EE hardware register block */
void ipu_dma_start(int ch, uint8_t *hw) {
    log_init();
    EV("dma ch%d start madr %08x qwc %x (out %u, pending %d)", ch, R32h(ch == 4 ? 0xB410 : 0xB010), R32h(ch == 4 ? 0xB420 : 0xB020), out_count, has_pending);
    hwr = hw;
    if (ipu_log > 0 && (ch == 4 || ipu_log > 1)) { static int n; if (n++ < 400) L("IPU DMA ch%d start: chcr %08x madr %08x qwc %x tadr %08x (in 0x%x)\n", ch, R32h(ch == 4 ? 0xB400 : 0xB000), R32h(ch == 4 ? 0xB410 : 0xB010), R32h(ch == 4 ? 0xB420 : 0xB020), R32h(0xB430), rec_cur); }
    if (ch == 4) {
        uint32_t chcr = R32h(0xB400);
        ch4.active = 1; ch4.chain = ((chcr >> 2) & 3) == 1; ch4.done_after = 0; ch4.sp = (chcr >> 4) & 3; if (ch4.sp > 2) ch4.sp = 2;
        ch4.madr = R32h(0xB410); ch4.qwc = R32h(0xB420) & 0xFFFF; ch4.tadr = R32h(0xB430);
        topup(); retry();
    } else {
        ch3.active = 1; ch3.madr = R32h(0xB010); ch3.qwc = R32h(0xB020) & 0xFFFF;
        drain_out(); retry();
    }
}
/* CHCR written with STR clear: the channel stops where it is (its registers keep the position for a restart) */
void ipu_dma_stop(int ch) {
    log_init();
    EV("dma ch%d stop", ch);
    if (ipu_log > 0) { static int n; if (n++ < 200) L("IPU DMA ch%d stop (was %s, qwc %x)\n", ch, (ch == 4 ? ch4.active : ch3.active) ? "running" : "idle", ch == 4 ? ch4.qwc : ch3.qwc); }
    if (ch == 4) { if (ch4.active) { ch4.active = 0; ch4_writeback(); } }
    else if (ch3.active) { ch3.active = 0; ch3_writeback(); }
}
int ipu_dma_active(int ch) { return ch == 4 ? ch4.active : ch3.active; }
void ipu_init(uint8_t *hw) {
    hwr = hw;
    compile(&c_mbai, t_mbai); compile(&c_mbt[1], t_mbt_i); compile(&c_mbt[2], t_mbt_p); compile(&c_mbt[3], t_mbt_b); compile(&c_mbt[4], t_mbt_d);
    compile(&c_mc, t_mc); compile(&c_dmv, t_dmv); compile(&c_cbp, t_cbp); compile(&c_dcl, t_dcl); compile(&c_dcc, t_dcc); compile(&c_dct0, t_dct0);
    compile(&c_dct1, t_dct1_short);
    for (const Vlc *v = t_dct0; v->code; v++) if (strlen(v->code) >= 14) {   /* the long codes are shared with table B.14 */
        VlcE *e = &c_dct1.e[c_dct1.n++]; e->len = (uint8_t)strlen(v->code); e->code = 0;
        for (int i = 0; i < e->len; i++) e->code = (uint16_t)((e->code << 1) | (v->code[i] == '1'));
        e->v1 = (int16_t)v->v1; e->v2 = (int16_t)v->v2; if (e->len > c_dct1.maxlen) c_dct1.maxlen = e->len;
    }
    for (int x = 0; x < 8; x++) for (int u = 0; u < 8; u++) idct_c[x][u] = (u ? sqrt(2.0 / 8) : sqrt(1.0 / 8)) * cos((2 * x + 1) * u * M_PI / 16);
}
uint64_t ipu_bdec_count(void) { return n_cmds[2]; }   /* macroblocks decoded so far (a movie is playing while it grows) */
void ipu_dump_stats(void) {
    static const char *nm[16] = { "BCLR", "IDEC", "BDEC", "VDEC", "FDEC", "SETIQ", "SETVQ", "CSC", "PACK", "SETTH" };
    int any = 0; for (int i = 0; i < 16; i++) any |= n_cmds[i] != 0; if (!any) return;
    L("IPU:"); for (int i = 0; i < 16; i++) if (n_cmds[i]) L(" %s %llu", nm[i] ? nm[i] : "?", (unsigned long long)n_cmds[i]);
    L("; %llu starved retries, queue in %u / out %u qw\n", (unsigned long long)n_starved, in_count, out_count);
    if (ipu_log > 0) for (unsigned i = evn > 8192 ? evn - 8192 : 0; i < evn; i++) L("  ipu ev %u: %s\n", i, evring[i & 8191]);
}
