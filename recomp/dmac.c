/* DMA controller (channels 1 = VIF1 and 2 = GIF) and the VIF1 command stream.
   Transfers run synchronously when the game sets CHCR.STR; completion interrupts are still raised by the
   VBlank code in hle.c (dma_pending). VIF1 handles DIRECT/DIRECTHL (to GIF path 2), MPG (VU1 micro memory),
   UNPACK (all formats, masking, offset/difference modes, CL/WL skip and fill, TOPS double buffering) and
   MSCAL/MSCALF/MSCNT, which run the VU1 interpreter (vu1.c) synchronously; XGKICK feeds GIF path 1. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "ee_runtime.h"
#include "hle.h"

void gif_qword(int path, const uint64_t q[2]);

extern uint8_t vu1_mem[16384], vu1_code[16384];
extern uint32_t vu1_tpc, vu1_top, vu1_itop, vu1_code_gen;
void vu1_run(uint32_t start);
static uint64_t n_mscal, n_direct_qw, n_unpack, n_mpg, n_dma_qw[10], n_tags;
static int vu1_exec = -1;   /* HG_VU1=0 disables execution */

static const uint8_t *dma_src(uint32_t a) { return (a & 0x80000000u) ? gptr(0x70000000u | (a & 0x3FF0)) : gptr(a & 0x01FFFFF0u); }

/* ------------------------------------------------------------------ VIF1 */
static struct {
    uint32_t left;        /* payload words left for the current command */
    int kind;             /* 1 DIRECT, 2 UNPACK, 3 MPG, 4 STMASK, 5 STROW, 6 STCOL */
    uint32_t q[4]; int nq;
    uint32_t addr;        /* MPG byte address */
    uint32_t cmd;         /* current UNPACK command word */
    uint32_t buf[1100]; int nbuf;
    int cl, wl, mode;
    uint32_t mask, row[4], col[4]; int nrc;
    uint32_t base, ofst, tops, top, itops, itop; int dbf;
} vif = { .cl = 1, .wl = 1 };
static uint64_t vif_words;

static void vu1_start(uint32_t pc) {
    vif.top = vif.tops; vif.itop = vif.itops;
    vif.dbf ^= 1; vif.tops = vif.base + (vif.dbf ? vif.ofst : 0);
    vu1_top = vif.top; vu1_itop = vif.itop;
    n_mscal++;
    if (vu1_exec < 0) vu1_exec = !(getenv("HG_VU1") && !strcmp(getenv("HG_VU1"), "0"));
    if (vu1_exec) vu1_run(pc);
}

/* UNPACK: vn 0..3 = S/V2/V3/V4, vl 0..3 = 32/16/8/5-bit; m = masked; usn = unsigned; flg = add TOPS.
   unpack_ref() handles every case; do_unpack() below takes a fast path for the common one. */
static void unpack_ref(void) {
    uint32_t c = vif.cmd, op = (c >> 24) & 0x7F, imm = c & 0xFFFF;
    int vn = (op >> 2) & 3, vl = op & 3, masked = (op >> 4) & 1, num = (c >> 16) & 0xFF; if (!num) num = 256;
    int usn = (imm >> 14) & 1, flg = (imm >> 15) & 1;
    uint32_t addr = (imm & 0x3FF) + (flg ? vif.tops : 0);
    int cl = vif.cl ? vif.cl : 1, wl = vif.wl ? vif.wl : 1;
    const uint8_t *src = (const uint8_t *)vif.buf; int pos = 0;    /* byte position in the payload */
    int ncomp = vn + 1, bits = vl == 3 ? 16 : (32 >> vl);
    int cycle = 0;
    for (int v = 0; v < num; v++) {
        int use_data = wl <= cl || cycle < cl;          /* in fill mode (WL > CL) the extra rows take no data */
        uint32_t d[4] = { 0, 0, 0, 0 };
        if (use_data) {
            if (vl == 3) {   /* V4-5: RGBA 5551 */
                uint16_t x; memcpy(&x, src + pos, 2); pos += 2;
                d[0] = (x & 31) << 3; d[1] = ((x >> 5) & 31) << 3; d[2] = ((x >> 10) & 31) << 3; d[3] = (x >> 15) << 7;
            } else for (int k = 0; k < ncomp; k++) {
                uint32_t x;
                if (bits == 32) { memcpy(&x, src + pos, 4); pos += 4; }
                else if (bits == 16) { uint16_t h; memcpy(&h, src + pos, 2); pos += 2; x = usn ? h : (uint32_t)(int32_t)(int16_t)h; }
                else { uint8_t b = src[pos++]; x = usn ? b : (uint32_t)(int32_t)(int8_t)b; }
                d[k] = x;
            }
            if (vn == 0) d[1] = d[2] = d[3] = d[0];                 /* S: broadcast */
        }
        uint32_t *m = (uint32_t *)(vu1_mem + ((addr & 0x3FF) << 4));
        int crow = cycle < 3 ? cycle : 3;
        for (int k = 0; k < 4; k++) {
            int sel = masked ? (vif.mask >> (crow * 8 + k * 2)) & 3 : 0;
            int have = use_data && (vl == 3 || vn == 0 || k < ncomp);
            if (sel == 0 && !have) sel = (wl > cl && !use_data) ? 1 : 3;   /* no data for this field: fill from ROW in fill mode, else keep */
            switch (sel) {
            case 0: {
                uint32_t x = d[k];
                if (vif.mode == 1) x += vif.row[k];
                else if (vif.mode == 2) { vif.row[k] += x; x = vif.row[k]; }
                m[k] = x; break; }
            case 1: m[k] = vif.row[k]; break;
            case 2: m[k] = vif.col[crow]; break;
            default: break;
            }
        }
        addr++;
        if (++cycle == wl) { cycle = 0; if (cl > wl) addr += cl - wl; }
    }
}
/* Every UNPACK except fill mode (WL > CL) and V4-5, which unpack_ref() keeps: with WL <= CL every vector takes data,
   so per field only the mask, ROW (STMOD offset/difference), COL and keep cases remain, as in unpack_ref(). The data
   is decoded per format outside the field logic, and the common unmasked STMOD 0 case writes the fields directly. */
static int unpack_fast(void) {
    uint32_t c = vif.cmd, op = (c >> 24) & 0x7F, imm = c & 0xFFFF;
    int vn = (op >> 2) & 3, vl = op & 3, masked = (op >> 4) & 1, num = (c >> 16) & 0xFF; if (!num) num = 256;
    int cl = vif.cl ? vif.cl : 1, wl = vif.wl ? vif.wl : 1;
    if (vl == 3 || wl > cl) return 0;
    int usn = (imm >> 14) & 1, flg = (imm >> 15) & 1;
    uint32_t addr = (imm & 0x3FF) + (flg ? vif.tops : 0);
    const int ncomp = vn + 1, plain = !masked && !vif.mode;
    const uint32_t *w32 = vif.buf; const uint16_t *w16 = (const uint16_t *)vif.buf; const uint8_t *w8 = (const uint8_t *)vif.buf;
    int cycle = 0;
    for (int v = 0; v < num; v++) {
        uint32_t d[4];
        for (int k = 0; k < ncomp; k++)
            d[k] = vl == 0 ? *w32++ : vl == 1 ? (usn ? (uint32_t)*w16++ : (uint32_t)(int32_t)(int16_t)*w16++) : (usn ? (uint32_t)*w8++ : (uint32_t)(int32_t)(int8_t)*w8++);
        if (vn == 0) d[1] = d[2] = d[3] = d[0];
        uint32_t *m = (uint32_t *)(vu1_mem + ((addr & 0x3FF) << 4));
        if (plain) { if (vn == 0) { m[0] = m[1] = m[2] = m[3] = d[0]; } else for (int k = 0; k < ncomp; k++) m[k] = d[k]; }
        else {
            const int crow = cycle < 3 ? cycle : 3;
            for (int k = 0; k < 4; k++) {
                int sel = masked ? (vif.mask >> (crow * 8 + k * 2)) & 3 : 0;
                if (sel == 0 && !(vn == 0 || k < ncomp)) sel = 3;
                switch (sel) {
                case 0: { uint32_t x = d[k];
                    if (vif.mode == 1) x += vif.row[k]; else if (vif.mode == 2) { vif.row[k] += x; x = vif.row[k]; }
                    m[k] = x; break; }
                case 1: m[k] = vif.row[k]; break;
                case 2: m[k] = vif.col[crow]; break;
                default: break;
                }
            }
        }
        addr++;
        if (++cycle == wl) { cycle = 0; addr += cl - wl; }
    }
    return 1;
}
static void do_unpack(void) {
    static int check = -1; if (check < 0) check = getenv("HG_UNPACKCHECK") && atoi(getenv("HG_UNPACKCHECK"));
    n_unpack++;
    if (!check) { if (!unpack_fast()) unpack_ref(); return; }
    /* HG_UNPACKCHECK=1: the fast path must leave VU memory exactly as the reference does */
    static uint8_t before[16384], ref[16384]; static uint64_t checked, bad;
    memcpy(before, vu1_mem, sizeof before);
    uint32_t row[4]; memcpy(row, vif.row, 16);
    unpack_ref(); memcpy(ref, vu1_mem, sizeof ref);
    uint32_t row_ref[4]; memcpy(row_ref, vif.row, 16);
    memcpy(vu1_mem, before, sizeof before); memcpy(vif.row, row, 16);
    if (unpack_fast()) {
        checked++;
        if ((memcmp(ref, vu1_mem, sizeof ref) || memcmp(row_ref, vif.row, 16)) && bad++ < 20) L("UNPACKCHECK: mismatch for UNPACK 0x%08x\n", vif.cmd);
        if (checked % 100000 == 0) L("UNPACKCHECK: %llu fast unpacks checked, %llu mismatches\n", (unsigned long long)checked, (unsigned long long)bad);
    }
    memcpy(vu1_mem, ref, sizeof ref); memcpy(vif.row, row_ref, 16);   /* go on with the reference result */
}
static void payload_done(void) {
    switch (vif.kind) {
    case 2: do_unpack(); break;
    case 4: vif.mask = vif.buf[0]; break;
    case 5: memcpy(vif.row, vif.buf, 16); break;
    case 6: memcpy(vif.col, vif.buf, 16); break;
    }
    vif.kind = 0;
}
static void vif_word(uint32_t w) {
    vif_words++;
    if (vif.left) {
        vif.left--;
        switch (vif.kind) {
        case 1:
            vif.q[vif.nq++] = w;
            if (vif.nq == 4) { uint64_t q[2] = { vif.q[0] | (uint64_t)vif.q[1] << 32, vif.q[2] | (uint64_t)vif.q[3] << 32 }; gif_qword(2, q); vif.nq = 0; n_direct_qw++; }
            break;
        case 3: memcpy(vu1_code + (vif.addr & 0x3FFC), &w, 4); vif.addr += 4; vu1_code_gen++; break;
        default: if (vif.nbuf < 1100) vif.buf[vif.nbuf++] = w; break;
        }
        if (!vif.left) payload_done();
        return;
    }
    uint32_t cmd = (w >> 24) & 0x7F, imm = w & 0xFFFF, num = (w >> 16) & 0xFF;
    vif.nbuf = 0;
    switch (cmd) {
    case 0x00: case 0x06: case 0x07: case 0x10: case 0x11: case 0x13: break;              /* NOP, MSKPATH3, MARK, FLUSHE/FLUSH/FLUSHA */
    case 0x01: vif.cl = imm & 0xFF; vif.wl = (imm >> 8) & 0xFF; break;                     /* STCYCL */
    case 0x02: vif.ofst = imm & 0x3FF; vif.dbf = 0; vif.tops = vif.base; break;            /* OFFSET */
    case 0x03: vif.base = imm & 0x3FF; break;                                              /* BASE */
    case 0x04: vif.itops = imm & 0x3FF; break;                                             /* ITOP */
    case 0x05: vif.mode = imm & 3; break;                                                  /* STMOD */
    case 0x14: case 0x15: vu1_start(imm); break;                                           /* MSCAL / MSCALF */
    case 0x17: vu1_start(vu1_tpc); break;                                                  /* MSCNT */
    case 0x20: vif.kind = 4; vif.left = 1; break;                                          /* STMASK */
    case 0x30: vif.kind = 5; vif.left = 4; break;                                          /* STROW */
    case 0x31: vif.kind = 6; vif.left = 4; break;                                          /* STCOL */
    case 0x4A: vif.kind = 3; vif.left = (num ? num : 256) * 2; vif.addr = imm * 8; n_mpg++; break;   /* MPG */
    case 0x50: case 0x51: vif.kind = 1; vif.left = (imm ? imm : 65536) * 4; vif.nq = 0; break;     /* DIRECT/DIRECTHL */
    default:
        if (cmd >= 0x60) {                                                                 /* UNPACK */
            int vn = (cmd >> 2) & 3, vl = cmd & 3, n = num ? num : 256;
            int cl = vif.cl ? vif.cl : 1, wl = vif.wl ? vif.wl : 1;
            int vecs = wl <= cl ? n : cl * (n / wl) + (n % wl < cl ? n % wl : cl);
            int bits = vl == 3 ? 16 : (32 >> vl) * (vn + 1);
            vif.kind = 2; vif.cmd = w; vif.left = (uint32_t)((vecs * bits + 31) / 32);
            if (!vif.left) do_unpack();
        } else L("VIF1: unknown command 0x%08x\n", w);
    }
}

/* a run of VIF1 words: command payloads (UNPACK data, MPG code, STROW/STCOL/STMASK) are taken a run at a time,
   with the same effect as vif_word() for each word */
static void vif_words_run(const uint32_t *w, uint32_t n) {
    while (n) {
        if (vif.left && vif.kind == 1 && vif.nq == 0 && n >= 4 && vif.left >= 4) {   /* DIRECT: whole qwords to GIF path 2 */
            uint32_t k = (vif.left < n ? vif.left : n) & ~3u;
            for (uint32_t i = 0; i < k; i += 4) {
                const uint64_t q[2] = { w[i] | (uint64_t)w[i + 1] << 32, w[i + 2] | (uint64_t)w[i + 3] << 32 };
                gif_qword(2, q); n_direct_qw++;
            }
            vif.left -= k; vif_words += k; w += k; n -= k;
            if (!vif.left) payload_done();
        } else if (vif.left && vif.kind != 1) {
            uint32_t k = vif.left < n ? vif.left : n;
            if (vif.kind == 3) { for (uint32_t i = 0; i < k; i++) { memcpy(vu1_code + (vif.addr & 0x3FFC), &w[i], 4); vif.addr += 4; } vu1_code_gen += k; }
            else { uint32_t room = vif.nbuf < 1100 ? 1100 - vif.nbuf : 0, c = k < room ? k : room;
                   memcpy(vif.buf + vif.nbuf, w, c * 4); vif.nbuf += c; }
            vif.left -= k; vif_words += k; w += k; n -= k;
            if (!vif.left) payload_done();
        } else { vif_word(*w++); n--; }
    }
}
/* ------------------------------------------------------------------ DMA */
static void feed(int ch, uint32_t madr, uint32_t qwc) {
    n_dma_qw[ch] += qwc;
    if (ch == 2) {
        for (uint32_t i = 0; i < qwc; i++) { uint64_t q[2]; memcpy(q, dma_src(madr + 16 * i), 16); gif_qword(3, q); }
        return;
    }
    for (uint32_t i = 0; i < qwc; ) {   /* contiguous stretches: the scratchpad wraps at 16 KB */
        const uint32_t a = madr + 16 * i;
        uint32_t run = qwc - i;
        if (a & 0x80000000u) { const uint32_t left = (0x4000 - (a & 0x3FF0)) >> 4; if (run > left) run = left; }
        uint32_t w[4 * 256]; if (run > 256) run = 256;
        memcpy(w, dma_src(a), run * 16);
        vif_words_run(w, run * 4); i += run;
    }
}
static uint32_t R32h(uint8_t *hw, uint32_t off) { uint32_t v; memcpy(&v, hw + off, 4); return v; }
static void W32h(uint8_t *hw, uint32_t off, uint32_t v) { memcpy(hw + off, &v, 4); }
static void dma_run_inner(int ch, uint8_t *hw);
void dma_run(int ch, uint8_t *hw) { double t0 = host_now(); double v0 = tprof[0] + tprof[1]; dma_run_inner(ch, hw); tprof[4] += host_now() - t0 - (tprof[0] + tprof[1] - v0); }
static void dma_run_inner(int ch, uint8_t *hw) {
    uint32_t base = ch == 1 ? 0x9000 : 0xA000;
    uint32_t chcr = R32h(hw, base), madr = R32h(hw, base + 0x10), qwc = R32h(hw, base + 0x20) & 0xFFFF, tadr = R32h(hw, base + 0x30);
    int mode = (chcr >> 2) & 3, tte = (chcr >> 6) & 1;
    if (chcr & 1) { if (mode == 0) feed(ch, madr, qwc); }       /* from memory */
    if (mode == 0) { madr += qwc * 16; qwc = 0; }
    else if (mode == 1) {
        uint32_t asr[2]; int sp = 0, guard = 0;
        if (qwc) { feed(ch, madr, qwc); madr += qwc * 16; qwc = 0; }
        for (;;) {
            if (++guard > 1000000) { L("DMA ch%d: chain did not end\n", ch); break; }
            uint64_t tag[2]; memcpy(tag, dma_src(tadr), 16); n_tags++;
            uint32_t tq = tag[0] & 0xFFFF, id = (tag[0] >> 28) & 7, addr = (uint32_t)(tag[0] >> 32) & 0x7FFFFFF0u;
            if ((tag[0] >> 63) & 1) addr |= 0x80000000u;
            if (tte && ch == 1) { vif_word((uint32_t)tag[1]); vif_word((uint32_t)(tag[1] >> 32)); }
            int end = 0;
            switch (id) {
            case 0: feed(ch, addr, tq); tadr += 16; end = 1; break;                               /* refe */
            case 1: feed(ch, tadr + 16, tq); tadr += 16 + tq * 16; break;                         /* cnt */
            case 2: feed(ch, tadr + 16, tq); tadr = addr; break;                                  /* next */
            case 3: case 4: feed(ch, addr, tq); tadr += 16; break;                                /* ref, refs */
            case 5: feed(ch, tadr + 16, tq); if (sp < 2) asr[sp++] = tadr + 16 + tq * 16; tadr = addr; break;   /* call */
            case 6: feed(ch, tadr + 16, tq); if (sp > 0) tadr = asr[--sp]; else end = 1; break;   /* ret */
            case 7: feed(ch, tadr + 16, tq); end = 1; break;                                      /* end */
            }
            if (end || ((tag[0] >> 31) & 1 && (chcr >> 7) & 1)) break;                            /* IRQ with TIE stops the chain */
        }
    }
    W32h(hw, base, chcr & ~0x100u); W32h(hw, base + 0x10, madr); W32h(hw, base + 0x20, 0); W32h(hw, base + 0x30, tadr);
}

/* GIF FIFO (PATH3 by CPU writes, 0x10006000): two 64-bit stores make one qword */
void gif_fifo_write(uint32_t off, uint64_t v) {
    static uint64_t q[2];
    q[(off >> 3) & 1] = v;
    if (off & 8) gif_qword(3, q);
}

void dma_dump_stats(void) {
    L("DMA: GIF %llu qw, VIF1 %llu qw, %llu tags; VIF1: %llu words, %llu DIRECT qw, %llu UNPACK, %llu MPG, %llu MSCAL\n",
      (unsigned long long)n_dma_qw[2], (unsigned long long)n_dma_qw[1], (unsigned long long)n_tags, (unsigned long long)vif_words,
      (unsigned long long)n_direct_qw, (unsigned long long)n_unpack, (unsigned long long)n_mpg, (unsigned long long)n_mscal);
}
