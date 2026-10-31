/* Keyboard keys in the game's text (docs/options-menu.md section 12).

   The game names buttons with PlayStation glyphs: the escape 0x03 c g 0x04 in a message draws glyph g (0xc0 Circle ..
   0xc9 Start, cells of the font sheet) in colour c; tutorials reach the glyph of a button through 0x13 n, a call
   into a short string the game builds for the controller type. A player on the keyboard should see the key
   instead. While the keyboard was the device used last (settings_device(), recorded like the controller type, so
   a replay draws the same text), three hooks on the text renderer do that without touching its layout code:

   - 0x382d30 (draws a line) and 0x382730 (a line's width, for centring and the window): the string the text object
     points at is flattened into a frame on the guest stack, following the same calls and returns the game's
     parser (0x3821f0) makes, with every glyph replaced by 0x05 <key name> 0x06 (two codes the parsers step over
     without drawing); the object points at the copy for the call and is put back after it. The game's own code
     lays the key's letters out with their widths, so advances, centring, window sizes and the typewriter effect
     (the count of elements drawn so far, obj+0x2c, mapped onto the copy) stay the game's.
   - 0x381bb0 (one character): at the first letter after 0x05 the keycap is drawn under the letters up to 0x06: a
     frame with chamfered corners and a fill, light on dark for message text (drawn additively), dark on light for
     the menus (their text subtracts from the parchment), as untextured sprites in the same display list, the
     text's PRIM switched off and back inside the packet, so the game's own glyph packets are as before.

   Without the keyboard (headless runs, the gates, a controller) the hooks return at once and nothing changes. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ee_runtime.h"
#include "hle.h"
#include "settings.h"
#include "game/guest.h"

void set_hook(uint32_t a, RecHook h);

/* ------------------------------------------------------------------ the game's side (SLUS_210.75) */
enum { FN_LINE = 0x382d30, FN_WIDTH = 0x382730, FN_GLYPH = 0x381bb0 };
enum { T_ALPHA = 0x16, T_PRIO = 0x17, T_STR = 0x28, T_COUNT = 0x2c };   /* the text object */
enum { WIDTH_TABLE = 0x44ad00,   /* a byte per glyph: width = 16 - (b >> 4), y shift = b & 15 (sixteenths of the size) */
       BANKS = 0x1991ec0,        /* message banks per language; the string of id n is at bank + u16[n + 1] */
       VARS = 0x1991ed0,         /* 0x08 n: 32-byte strings */
       BUTTONS = 0x1991f50,      /* 0x13 n: 8-byte strings, the glyph of the button that does action n */
       LANG = 0x47b350,          /* the language (gp - 0x75a0) */
       DLIST = 0x44e4f0 };       /* the display-list manager: vtable +0x10 (mgr, qwords, priority) -> a packet, or 0 */
enum { KC_OPEN = 0x05, KC_CLOSE = 0x06 };   /* the markers round a key's name (no-ops to the game) */
enum { PRIM_TEXT = 0x156, PRIM_FLAT = 0x46 };   /* sprite + texture + alpha + UV (0x382000); sprite + alpha */
enum { FN_TEXT = 0x384650 };   /* (text, x, y, colour, str, alpha, prio, size, [sp] height): a text to draw */

static int enabled = 1;   /* options.c turns the keycaps off round the glyphs that name a button being mapped */
void keycaps_enable(int on) { enabled = on; }
static int kb_mode(void) { return enabled && settings_device() == DEV_KEYBOARD; }

/* the key for a glyph, or 0 when none is bound (the glyph stays) */
static const char *key_for(uint8_t g) {
    static const int pb[10] = { PB_CIRCLE, PB_TRIANGLE, PB_SQUARE, PB_CROSS, PB_L1, PB_L2, PB_R1, PB_R2, PB_SELECT, PB_START };
    if (g < 0xc0 || g > 0xc9) return 0;
    const int sc = settings()->kb[pb[g - 0xc0]];
    return sc > 0 ? settings_key_label(sc) : 0;
}

/* ------------------------------------------------------------------ the flat copy of a string */
typedef struct {
    uint8_t s[2048]; int n;        /* the copy, with its terminator */
    uint16_t map[1024]; int elems;   /* map[i]: the copy's element index where the original's element i starts */
} Flat;

/* The original's elements, stepped as 0x3821f0 steps them (calls followed, returns taken), copied with every glyph
   replaced by 0x05 <key> 0x06 (the colour escape round it dropped: the key is text, not a coloured picture).
   Returns 1 when a glyph was replaced, 0 when there is nothing to do or the string is too long or odd. */
static int flatten(uint32_t p, Flat *f) {
    uint32_t ret[4]; int depth = 0, changed = 0, i = 0, j = 0, n = 0;
    for (;;) {
        if (i >= 1023 || n >= 2000) return 0;
        f->map[i] = (uint16_t)j;
        const uint8_t b = rd8(p);
        int k = 1;
        switch (b) {
        case 0:
            if (!depth) { f->s[n++] = 0; f->map[i + 1] = (uint16_t)j; f->elems = i; f->n = n; return changed; }
            p = ret[--depth]; i++; continue;   /* a return: an element with nothing to copy */
        case 8:  if (depth == 4) return 0; ret[depth++] = p + 2; p = VARS + rd8(p + 1) * 0x20; i++; continue;
        case 0x13: if (depth == 4) return 0; ret[depth++] = p + 2; p = BUTTONS + rd8(p + 1) * 8; i++; continue;
        case 0x11: case 0x15: {
            uint32_t id = b == 0x11 ? (uint32_t)rd8(p + 1) << 8 | rd8(p + 2) : (((uint32_t)rd8(p + 1) << 8 | rd8(p + 2)) + 0x8100) & 0xFFFF;
            int lang = rd8(LANG);
            if (id & 0x8000) { id &= 0x7FFF; lang = 0; }
            const uint32_t bank = rd32(BANKS + lang * 4);
            if (!bank) return 0;
            if (b == 0x15) { if (depth == 4) return 0; ret[depth++] = p + 3; }
            p = bank + rd16(bank + id * 2 + 2); i++; continue;
        }
        case 1: case 2: case 4: case 5: case 6: case 0xd: case 0x12: case 0x14: case 0x16: case 0x17: case 0x18: k = 1; break;
        case 3: case 7: case 9: case 0xa: case 0xc: case 0x10: case 0x1a: case 0x1b: case 0x1c: case 0x1d: k = 2; break;
        case 0xb: case 0xe: case 0xf: case 0x19: k = 3; break;
        default: k = 1; break;
        }
        const char *key = b >= 0xc0 && b <= 0xc9 ? key_for(b) : 0;
        if (key && (int)strlen(key) + n + 3 < 2000) {
            if (n >= 2 && f->s[n - 2] == 3 && i >= 1 && f->map[i - 1] == j - 1) { n -= 2; j--; }   /* the colour escape before it */
            f->s[n++] = KC_OPEN; j++;
            for (const char *q = key; *q; q++) { f->s[n++] = (uint8_t)*q; j++; }
            f->s[n++] = KC_CLOSE; j++;
            p += 1; i++; changed = 1;
            if (rd8(p) == 4) { f->map[i] = (uint16_t)j; p += 1; i++; }   /* and the one after it */
            continue;
        }
        for (int m = 0; m < k; m++) f->s[n + m] = rd8(p + m);
        n += k; p += k; i++; j++;
    }
}

/* ------------------------------------------------------------------ the hooks */
static int inside;   /* the hooks call the hooked functions: no second pass on the copy */

static int hk_line(Ctx *c) {   /* 0x382d30(text): a line, obj+0x2c elements from obj+0x28 */
    if (inside || !kb_mode()) return 0;
    const uint32_t obj = c->r[4].u32[0], str = rd32(obj + T_STR);
    if (!str) return 0;
    static Flat f;
    if (!flatten(str, &f)) return 0;
    const uint16_t count = rd16(obj + T_COUNT);
    GuestFrame fr; guest_frame_open(c, &fr, (uint32_t)f.n + 16);
    const uint32_t copy = guest_put(&fr, f.s, (uint32_t)f.n);
    if (!copy) { guest_frame_close(c, &fr); return 0; }
    wr32(obj + T_STR, copy); wr16(obj + T_COUNT, f.map[count <= f.elems ? count : f.elems]);
    const uint32_t a[1] = { obj };
    inside = 1; guest_call(c, FN_LINE, 1, a, 0, 0); inside = 0;
    wr16(obj + T_COUNT, count); wr32(obj + T_STR, str);
    guest_frame_close(c, &fr);
    return 1;
}
static int hk_width(Ctx *c) {   /* 0x382730(text, {ptr, depth, returns[4]}): the width up to the line's end */
    if (inside || !kb_mode()) return 0;
    const uint32_t blk = c->r[5].u32[0], ptr = rd32(blk);
    if (!ptr || rd8(blk + 4)) return 0;   /* (inside a called string: the game's own walk) */
    static Flat f;
    if (!flatten(ptr, &f)) return 0;
    GuestFrame fr; guest_frame_open(c, &fr, (uint32_t)f.n + 48);
    const uint32_t copy = guest_put(&fr, f.s, (uint32_t)f.n);
    uint8_t nb[24] = { 0 }; memcpy(nb, &copy, 4);
    const uint32_t b = guest_put(&fr, nb, sizeof nb);
    if (!copy || !b) { guest_frame_close(c, &fr); return 0; }
    const uint32_t a[2] = { c->r[4].u32[0], b };
    inside = 1; const uint32_t w = guest_call(c, FN_WIDTH, 2, a, 0, 0); inside = 0;
    guest_frame_close(c, &fr);
    c->r[2].u64[0] = w;
    return 1;
}

/* the keycap: 4 untextured sprites (a frame with the corners cut, then the fill) in one packet of the text's display
   list, as 0x381bb0 builds its glyph packets (DMA tag + VIF1 DIRECT, then GIF tags: A+D ALPHA and PRIM, a REGLIST of
   RGBAQ, XYZ2, XYZ2 per sprite, and A+D PRIM back to the text's) */
static void put64(uint32_t a, uint64_t v) { memcpy(gptr(a), &v, 8); }
static uint64_t xyz(int x, int y) { return (uint64_t)(((x + 0x700) * 16 + 8) & 0xFFFF) | (uint64_t)(((y + 0x720) * 16 + 8) & 0xFFFF) << 16 | 0xFFFFFFFFull << 32; }
static void keycap(Ctx *c, uint32_t obj, int x0, int y0, int x1, int y1, int light_on_dark) {
    enum { NQW = 13 };
    const uint32_t mgr = rd32(DLIST); if (!mgr) return;
    const uint32_t fn = rd32(rd32(mgr) + 0x10); if (!fn) return;
    GuestFrame fr; guest_frame_open(c, &fr, 0);
    const uint32_t a[3] = { mgr, NQW, rd8(obj + T_PRIO) };
    const uint32_t pk = guest_call(c, fn, 3, a, 0, 0);
    guest_frame_close(c, &fr);
    if (!pk) return;
    const uint32_t alpha = rd8(obj + T_ALPHA);   /* the text's fade */
    const uint64_t frame = light_on_dark ? 0xB8B8B8u : 0x404040u, fill = light_on_dark ? 0x202020u : 0xF2EEE4u;
    const uint64_t cf = frame | (uint64_t)alpha << 24, ci = fill | (uint64_t)alpha << 24;
    const struct { uint64_t col; int x0, y0, x1, y1; } r[4] = {
        { cf, x0 + 1, y0, x1 - 1, y1 }, { cf, x0, y0 + 1, x1, y1 - 1 },
        { ci, x0 + 2, y0 + 1, x1 - 2, y1 - 1 }, { ci, x0 + 1, y0 + 2, x1 - 1, y1 - 2 } };
    put64(pk + 0, 0x10000000u | (NQW - 1)); put64(pk + 8, (uint64_t)(0x50000000u | (NQW - 1)) << 32);   /* DMA cnt, VIF1 DIRECT */
    put64(pk + 16, 0x1000000000000002ull); put64(pk + 24, 0xE);                   /* GIFtag: A+D x2 */
    put64(pk + 32, 0x44 | (uint64_t)alpha << 32); put64(pk + 40, 0x42);          /* ALPHA_1: (Cs - Cd) As + Cd */
    put64(pk + 48, PRIM_FLAT); put64(pk + 56, 0x00);                               /* PRIM */
    put64(pk + 64, 0x3ull << 60 | 0x1ull << 58 | 4); put64(pk + 72, 0x551);       /* GIFtag: REGLIST RGBAQ XYZ2 XYZ2, x4 */
    for (int i = 0; i < 4; i++) {
        put64(pk + 80 + 24 * i, r[i].col); put64(pk + 88 + 24 * i, xyz(r[i].x0, r[i].y0)); put64(pk + 96 + 24 * i, xyz(r[i].x1, r[i].y1));
    }
    put64(pk + 176, 0x1000000000008001ull); put64(pk + 184, 0xE);                 /* GIFtag: A+D x1, end of packet */
    put64(pk + 192, PRIM_TEXT); put64(pk + 200, 0x00);                             /* the text's PRIM again */
}

static int hk_glyph(Ctx *c) {   /* 0x381bb0(text, x, y, w, h, colour, p): one character; p inside the flat copy */
    if (!kb_mode()) return 0;
    const uint32_t obj = c->r[4].u32[0], p = c->r[10].u32[0];
    if (!p || rd8(p - 1) != KC_OPEN) return 0;   /* only the key's first letter */
    const int x = (int32_t)c->r[5].u32[0], y = (int32_t)c->r[6].u32[0], size = (int32_t)c->r[7].u32[0], h = (int32_t)c->r[8].u32[0];
    const uint32_t colour = c->r[9].u32[0];
    int w = 0;
    for (uint32_t q = p; rd8(q) && rd8(q) != KC_CLOSE; q++) {
        const uint8_t t = rd8(WIDTH_TABLE + (rd8(q) - 0x20));
        w += ((16 - (t >> 4)) * size >> 4) + 1;   /* the advance, as 0x382d30 */
    }
    const int ys = (rd8(WIDTH_TABLE + (rd8(p) - 0x20)) & 15) * size >> 4;   /* the first letter's own shift (none in a key name) */
    /* the text's blend, as 0x381bb0 picks it: additive, or with the 0x80 flag subtractive unless CSA 4-6 */
    const int sub = (colour & 0x80) && ((colour & 0x7F) < 4 || (colour & 0x7F) > 6);
    const int top = y - ys + h / 16, bottom = y - ys + h * 15 / 16;   /* the letters' rows 1-14 of 16 */
    keycap(c, obj, x - 3, top - 3, x + w + 1, bottom + 3, !sub);
    return 0;   /* the game draws the letter */
}

/* A short text that is a glyph (the Options page's Controller Type block: "(O):", with "Search" as another text
   32 px to the right): the text that follows it on the same row is moved right by what the keycap made the first
   one wider, so the two never overlap. */
static struct { uint32_t obj; int x, y, shift; } last;   /* the last text widened by a keycap */
static int advance(uint8_t ch, int size) { const uint8_t t = rd8(WIDTH_TABLE + (ch - 0x20)); return ((16 - (t >> 4)) * size >> 4) + 1; }
static int hk_text(Ctx *c) {   /* 0x384650(text, x, y, colour, str, ...) */
    if (!kb_mode()) return 0;
    const uint32_t obj = c->r[4].u32[0], str = c->r[8].u32[0];
    const int x = (int32_t)c->r[5].u32[0], y = (int32_t)c->r[6].u32[0], size = (int32_t)c->r[11].u32[0];
    if (last.obj == obj && last.y == y && x - last.x >= 14 && x - last.x <= 40) c->r[5].u64[0] = (uint64_t)(int64_t)(x + last.shift);
    if (getenv("HG_KEYCAPLOG")) { char t[40]; int k = 0; for (uint32_t q = str; k < 36 && rd8(q); q++) k += snprintf(t + k, sizeof t - k, "%02x ", rd8(q)); t[k] = 0;
        L("keycaps: text obj %x x %d y %d size %d str %x [%s] after %x %d %d by %d\n", obj, x, y, size, str, t, last.obj, last.x, last.y, last.shift); }
    last.obj = 0;
    static Flat f;
    if (!str || !flatten(str, &f) || f.n > 16) return 0;
    int before = 0, after = 4;   /* the frame's 3 px left and 1 right of the letters */
    for (uint32_t q = str; rd8(q); q++) { const uint8_t b = rd8(q); if (b == 3) q++; else if (b >= 0x20) before += advance(b, size); }
    for (int i = 0; f.s[i]; i++) if (f.s[i] >= 0x20) after += advance(f.s[i], size);
    if (after <= before) return 0;
    last.obj = obj; last.x = x; last.y = y; last.shift = after - before;
    return 0;
}

void keycaps_init(void) {
    set_hook(FN_TEXT, hk_text);
    set_hook(FN_LINE, hk_line);
    set_hook(FN_WIDTH, hk_width);
    set_hook(FN_GLYPH, hk_glyph);
}
