/* DMA controller (channels 1 = VIF1 and 2 = GIF) and the VIF1 command stream.
   Transfers run synchronously when the game sets CHCR.STR; completion interrupts are still raised by the
   VBlank code in hle.c (dma_pending). VU1 microprograms (MPG/MSCAL) and UNPACK data are stored in VU1
   memory but not executed yet: that is the 3D path (phase 2). DIRECT/DIRECTHL packets go to the GIF. */
#include <stdio.h>
#include <string.h>
#include "ee_runtime.h"
#include "hle.h"

void gif_qword(int path, const uint64_t q[2]);

static uint8_t vu1_code[16384], vu1_data[16384];
static uint64_t n_mscal, n_direct_qw, n_unpack, n_mpg, n_dma_qw[10], n_tags;

static const uint8_t *dma_src(uint32_t a) { return (a & 0x80000000u) ? gptr(0x70000000u | (a & 0x3FF0)) : gptr(a & 0x01FFFFF0u); }

/* ------------------------------------------------------------------ VIF1 */
static struct {
    int state;            /* 0 = expect command word */
    uint32_t cmd, left;   /* words left for the current command's payload */
    int kind;             /* 1 DIRECT, 2 UNPACK, 3 MPG, 4 STMASK/STROW/STCOL */
    uint32_t q[4]; int nq;
    uint32_t addr;        /* VU1 address for MPG/UNPACK */
    int cl, wl;
} vif;
static uint64_t vif_words;
static void vif_word(uint32_t w) {
    vif_words++;
    if (vif.left) {
        vif.left--;
        if (vif.kind == 1) {
            vif.q[vif.nq++] = w;
            if (vif.nq == 4) { uint64_t q[2] = { vif.q[0] | (uint64_t)vif.q[1] << 32, vif.q[2] | (uint64_t)vif.q[3] << 32 }; gif_qword(2, q); vif.nq = 0; n_direct_qw++; }
        } else if (vif.kind == 3) { memcpy(vu1_code + (vif.addr & 0x3FFC), &w, 4); vif.addr += 4; }
        return;
    }
    uint32_t cmd = (w >> 24) & 0x7F, imm = w & 0xFFFF, num = (w >> 16) & 0xFF;
    switch (cmd) {
    case 0x00: case 0x02: case 0x03: case 0x04: case 0x05: case 0x06: case 0x07: case 0x10: case 0x11: case 0x13: break;
    case 0x01: vif.cl = imm & 0xFF; vif.wl = (imm >> 8) & 0xFF; break;                       /* STCYCL */
    case 0x14: case 0x15: case 0x17: n_mscal++; break;                                        /* MSCAL/MSCALF/MSCNT: VU1 not run */
    case 0x20: vif.kind = 4; vif.left = 1; break;                                             /* STMASK */
    case 0x30: case 0x31: vif.kind = 4; vif.left = 4; break;                                  /* STROW/STCOL */
    case 0x4A: vif.kind = 3; vif.left = (num ? num : 256) * 2; vif.addr = imm * 8; n_mpg++; break;   /* MPG */
    case 0x50: case 0x51: vif.kind = 1; vif.left = (imm ? imm : 65536) * 4; vif.nq = 0; break;     /* DIRECT/DIRECTHL */
    default:
        if (cmd >= 0x60) {                                                                    /* UNPACK */
            int vn = (cmd >> 2) & 3, vl = cmd & 3, n = num ? num : 256;
            int cl = vif.cl ? vif.cl : 1, wl = vif.wl ? vif.wl : 1;
            int vecs = wl <= cl ? n : cl * (n / wl) + (n % wl < cl ? n % wl : cl);
            int bits = (32 >> vl) * (vn + 1);
            vif.kind = 2; vif.left = (uint32_t)((vecs * bits + 31) / 32); n_unpack++;
        } else L("VIF1: unknown command 0x%08x\n", w);
    }
}

/* ------------------------------------------------------------------ DMA */
static void feed(int ch, uint32_t madr, uint32_t qwc) {
    n_dma_qw[ch] += qwc;
    for (uint32_t i = 0; i < qwc; i++) {
        const uint8_t *p = dma_src(madr + 16 * i);
        if (ch == 2) { uint64_t q[2]; memcpy(q, p, 16); gif_qword(3, q); }
        else { uint32_t w[4]; memcpy(w, p, 16); for (int k = 0; k < 4; k++) vif_word(w[k]); }
    }
}
static uint32_t R32h(uint8_t *hw, uint32_t off) { uint32_t v; memcpy(&v, hw + off, 4); return v; }
static void W32h(uint8_t *hw, uint32_t off, uint32_t v) { memcpy(hw + off, &v, 4); }
void dma_run(int ch, uint8_t *hw) {
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
    L("DMA: GIF %llu qw, VIF1 %llu qw, %llu tags; VIF1: %llu words, %llu DIRECT qw, %llu UNPACK, %llu MPG, %llu MSCAL (VU1 not executed)\n",
      (unsigned long long)n_dma_qw[2], (unsigned long long)n_dma_qw[1], (unsigned long long)n_tags, (unsigned long long)vif_words,
      (unsigned long long)n_direct_qw, (unsigned long long)n_unpack, (unsigned long long)n_mpg, (unsigned long long)n_mscal);
}
