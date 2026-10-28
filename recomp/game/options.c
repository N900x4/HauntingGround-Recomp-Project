/* The game's Options menu with extra rows for the host settings (roadmap phase 3; docs/options-menu.md).

   The Options page (Controller type, Vibration, Sound, Volume, Screen position) is the game's own code: the state
   "choose a row" (0x393bd0) and the page's drawing (0x391450). Both are hooked here:

   - While the row byte (obj+0xa8c56) is 0-4 the game's code runs untouched, except that Down on Screen Position, or
     Up on Controller Type, moves into the new rows instead of wrapping round.
   - Rows 5 and up are ours: a second page of the same panel with Renderer, Resolution, Frame rate, Display, V-Sync,
     Keyboard and Controller, and the two mapping pages behind the last two. They are drawn with the game's own
     panel pictures, text, icons, arrows, cursor, sounds and Reset Defaults dialog, called as guest functions
     (game/guest.h), and driven by the game's own menu input (the pressed-buttons word it builds for its menus).
   - The values are host settings (settings.c): they never touch the game's memory or its save. A change is applied
     when the row is confirmed with Cross, as the game does for its own rows.

   Everything here depends only on the menu input and the settings, which a replay reproduces (settings.c records
   what does not come from the pad), so recordings that use these rows replay identically. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ee_runtime.h"
#include "hle.h"
#include "settings.h"
#include "hdtex.h"
#include "game/guest.h"

void set_hook(uint32_t a, RecHook h);

/* ------------------------------------------------------------------ the game's side (SLUS_210.75) */
enum {
    FN_CHOOSE_ROW = 0x393bd0,   /* state "choose a row": input, then draws the page and the footer */
    FN_DRAW_PAGE = 0x391450,    /* (obj, editing): the whole page */
    FN_PIECE = 0x3858e0,        /* (obj, piece, 128, 48): a piece of the panel (table 0x44c160) */
    FN_SPRITE = 0x3854c0,       /* (obj, x, y, sprite, alpha, 0): table 0x44c280 */
    FN_TEXT = 0x384650,         /* (text, x, y, colour, str, 128, 48, size, [sp] 21) */
    FN_TEXT_WIDTH = 0x382730,   /* (text, {str, 0}) after text+34 = size */
    FN_MESSAGE = 0x384b00,      /* (text, id): message text */
    FN_DIALOG_OPEN = 0x384a90,  /* (dialog, message): YES/NO */
    FN_DIALOG_RUN = 0x384bc0,   /* (dialog): one frame of it */
};
enum {
    O_ROW = 0xa8c56,            /* the row being chosen */
    O_NOINPUT = 0xa8c61,        /* nonzero: the page takes no input (opening or closing) */
    O_PANEL = 0xa8c66,          /* the panel set, an index into the table at 0x44c120 */
    O_FRAMES = 0xa8de0,         /* frames since the page opened (the arrows blink with it) */
    O_TEXT = 0x97868,           /* the page's text renderer */
    O_DIALOG = 0x97764,         /* the page's YES/NO dialog */
    O_DIALOG_STATE = 0x97764 + 0x10,   /* 0: closed */
    O_DIALOG_ANSWER = 0x97764 + 0x48,  /* 0: YES */
};
enum { PIECE_TABLE = 0x44c160, PANEL_SETS = 0x44c120 };
enum { PAD_HELD = 0x47e364, PAD_PRESSED = 0x47e36c, PAD_RAW = 0x47e374, SOUND_OBJ = 0x44e560, GAME_OBJ = 0x44e4d8 };
/* the menu word's bits (built by the game from the controller type's mapping) */
enum { M_UP = 1 << 0, M_RIGHT = 1 << 1, M_DOWN = 1 << 2, M_LEFT = 1 << 3, M_OK = 1 << 4, M_BACK = 1 << 5,
       M_TAB_A = 1 << 6, M_TAB_B = 1 << 7, M_RESET = 1 << 9 };
enum { SND_MOVE = 42, SND_OK = 43, SND_CANCEL = 44 };
enum { SPR_ICON_PAD = 16, SPR_ICON_WAVES = 17, SPR_ICON_SPEAKER = 18, SPR_ICON_VOLUME = 19, SPR_ICON_SCREEN = 20,
       SPR_ARROW_L = 26, SPR_ARROW_R = 27, SPR_CURSOR = 28 };
enum { COL_NORMAL = 128, COL_EDIT = 130 };
enum { MSG_FOOTER = 130, MSG_RESET_DEFAULTS = 132 };
/* a button glyph of the game's font in one of its colours */
#define GLYPH(colour, g) "\x03" colour g "\x04"
#define G_CIRCLE GLYPH("\x04", "\xc0")
#define G_TRIANGLE GLYPH("\x05", "\xc1")
#define G_SQUARE GLYPH("\x04", "\xc2")
#define G_CROSS GLYPH("\x05", "\xc3")

/* ------------------------------------------------------------------ drawing through the game */
typedef struct { Ctx *c; GuestFrame f; uint32_t obj; } Draw;
static void d_open(Draw *d, Ctx *c, uint32_t obj) { d->c = c; d->obj = obj; guest_frame_open(c, &d->f, 3072); }
static void d_close(Draw *d) { guest_frame_close(d->c, &d->f); }
static void d_piece(Draw *d, int piece) {
    const uint32_t a[4] = { d->obj, (uint32_t)piece, 128, 48 };
    guest_call(d->c, FN_PIECE, 4, a, 0, 0);
}
/* a rectangle of panel picture `tex` at (x, y): piece 0's table entry is set for the call and put back at once (the
   packet is built during the call), so game memory is as before */
static void d_rect(Draw *d, int tex, int u, int v, int w, int h, int x, int y) {
    uint8_t keep[14];
    memcpy(keep, gptr(PIECE_TABLE), 14);
    const uint16_t e[7] = { (uint16_t)tex, (uint16_t)u, (uint16_t)v, (uint16_t)w, (uint16_t)h, (uint16_t)x, (uint16_t)y };
    memcpy(gptr(PIECE_TABLE), e, 14);
    d_piece(d, 0);
    memcpy(gptr(PIECE_TABLE), keep, 14);
}
static void d_sprite(Draw *d, int x, int y, int id, int alpha) {
    const uint32_t a[6] = { d->obj, (uint32_t)x, (uint32_t)y, (uint32_t)id, (uint32_t)alpha, 0 };
    guest_call(d->c, FN_SPRITE, 6, a, 0, 0);
}
static void d_text_at(Draw *d, int x, int y, int colour, uint32_t str, int size) {
    if (!str) return;
    const uint32_t a[8] = { d->obj + O_TEXT, (uint32_t)x, (uint32_t)y, (uint32_t)colour, str, 128, 48, (uint32_t)size };
    const uint64_t st[1] = { 21 };
    guest_call(d->c, FN_TEXT, 8, a, 1, st);
}
static void d_text(Draw *d, int x, int y, int colour, const char *s, int size) {
    d_text_at(d, x, y, colour, guest_str(&d->f, s), size);
}
static int d_width(Draw *d, const char *s, int size) {
    const uint32_t p = guest_str(&d->f, s);
    if (!p) return 0;
    uint8_t blk[32] = { 0 };
    memcpy(blk, &p, 4);
    const uint32_t b = guest_put(&d->f, blk, sizeof blk);
    if (!b) return 0;
    wr16(d->obj + O_TEXT + 34, (uint16_t)size);
    const uint32_t a[2] = { d->obj + O_TEXT, b };
    return (int)(guest_call(d->c, FN_TEXT_WIDTH, 2, a, 0, 0) & 0xFFFF);
}
static void d_text_centred(Draw *d, int cx, int y, int colour, const char *s, int size) {
    d_text(d, cx - d_width(d, s, size) / 2, y, colour, s, size);
}
static uint32_t message(Draw *d, int id) {
    const uint32_t a[2] = { d->obj + O_TEXT, (uint32_t)id };
    return guest_call(d->c, FN_MESSAGE, 2, a, 0, 0);
}
static void sound(Ctx *c, int id) {
    const uint32_t s = rd32(SOUND_OBJ);
    if (!s) return;
    GuestFrame f;
    guest_frame_open(c, &f, 0);
    const uint32_t fn = rd32(rd32(s) + 20), a[3] = { s, (uint32_t)id, 5 };
    guest_call(c, fn, 3, a, 0, 0);
    guest_frame_close(c, &f);
}
static void dialog_run(Ctx *c, uint32_t obj) {
    GuestFrame f;
    guest_frame_open(c, &f, 0);
    const uint32_t a[1] = { obj + O_DIALOG };
    guest_call(c, FN_DIALOG_RUN, 1, a, 0, 0);
    guest_frame_close(c, &f);
}
static void dialog_open(Ctx *c, uint32_t obj) {
    GuestFrame f;
    guest_frame_open(c, &f, 0);
    const uint32_t a[2] = { obj + O_DIALOG, MSG_RESET_DEFAULTS };   /* "Reset Defaults?" YES / NO */
    guest_call(c, FN_DIALOG_OPEN, 2, a, 0, 0);
    guest_frame_close(c, &f);
}
/* the page as the game's own code draws it (row byte 0-4), with its footer: the end of 0x393bd0 */
static void game_page(Ctx *c, uint32_t obj) {
    GuestFrame f;
    guest_frame_open(c, &f, 0);
    const uint32_t a[2] = { obj, 0 };
    guest_call(c, FN_DRAW_PAGE, 2, a, 0, 0);
    guest_frame_close(c, &f);
    Draw d;
    d_open(&d, c, obj);
    d_text_at(&d, 70, 390, COL_NORMAL, message(&d, MSG_FOOTER), 16);
    d_close(&d);
}

/* the panel as the game draws it for this page (the start of 0x391450) */
static void d_panel(Draw *d) {
    const uint32_t g = rd32(GAME_OBJ);
    uint8_t set;
    if (rd8(d->obj + 4)) set = 138;
    else if (!g || rd8(g + 0x1fbec1) != 1) set = 3;
    else set = 10;
    wr8(d->obj + O_PANEL, set);
    for (int i = 0; i < 4; i++) {
        const int8_t p = (int8_t)rd8(PANEL_SETS + (set & 0x7f) * 4 + i);
        if (p < 0) break;
        d_piece(d, p);
    }
}
/* panel pictures: 6 holds the upper part (the CONTROLLER TYPE bar at y 84-116, the two-column area below it with its
   divider at x 256), 7 the lower part from y 256 (four rows of 32 pixels from y 244, each a separator and a bar) */
enum { TEX_UPPER = 6, TEX_LOWER = 7 };
static void d_row_strip(Draw *d, int y) { d_rect(d, TEX_LOWER, 0, 276 - 256, 512, 32, 0, y); }   /* Sound's row */
/* plain parchment over [y, y + h), keeping the panel's left and right edges: the two-column area without the divider */
static void d_plain(Draw *d, int y, int h) {
    while (h > 0) {
        const int n = h > 100 ? 100 : h;
        d_rect(d, TEX_UPPER, 0, 128, 250, n, 0, y);
        d_rect(d, TEX_UPPER, 262, 128, 250, n, 262, y);
        d_rect(d, TEX_UPPER, 270, 128, 12, n, 250, y);   /* where the divider was */
        y += n;
        h -= n;
    }
}
/* the two-column area (with its divider) over [y, y + h) */
static void d_columns(Draw *d, int y, int h) {
    while (h > 0) {
        const int n = h > 100 ? 100 : h;
        d_rect(d, TEX_UPPER, 0, 128, 512, n, 0, y);
        y += n;
        h -= n;
    }
}

/* ------------------------------------------------------------------ our pages */
enum { PG_NONE, PG_DISPLAY, PG_KEYBOARD, PG_PAD };
enum { R_RENDERER, R_RESOLUTION, R_FPS, R_DISPLAY, R_ASPECT, R_VSYNC, R_HD, R_KEYBOARD, R_PAD, NROWS };
static const char *const row_label[NROWS] = { "Renderer", "Resolution", "Frame rate", "Display", "Aspect ratio", "V-Sync", "HD textures", "Keyboard", "Controller" };
static const int row_icon[NROWS] = {
    SPR_ICON_SCREEN, SPR_ICON_SCREEN, SPR_ICON_WAVES, SPR_ICON_SCREEN, SPR_ICON_SCREEN, SPR_ICON_WAVES, SPR_ICON_SCREEN, SPR_ICON_PAD, SPR_ICON_PAD,
};
#define ROW_Y(r) (82 + 30 * (r))   /* the icon's top; the label 6 below (the game's rows were 32 apart from 84: nine rows and the
                                     note need 30, on parchment of our own with a bar per row) */
/* the mapping pages: the keyboard has the 16 buttons and the left stick's 4 directions, the controller 16 buttons */
static const int map_order[PB_N] = {
    PB_CROSS, PB_CIRCLE, PB_SQUARE, PB_TRIANGLE, PB_L1, PB_R1, PB_L2, PB_R2, PB_L3, PB_R3,
    PB_UP, PB_DOWN, PB_LEFT, PB_RIGHT, PB_START, PB_SELECT, PB_LS_UP, PB_LS_DOWN, PB_LS_LEFT, PB_LS_RIGHT,
};
#define MAP_ROWS 10
#define MAP_Y(i) (122 + 25 * (i))
/* the controller page's two value entries after its 16 buttons (right column): vibration strength, light bar */
enum { E_VIB = PB_BUTTONS, E_LIGHT, PAD_ENTRIES };

typedef struct {
    uint32_t obj;
    int page;            /* PG_* */
    int row;             /* the row (PG_DISPLAY) or the entry (mapping pages) */
    int editing;         /* a PG_DISPLAY row being changed... */
    int value;           /* ...and the value shown while it is */
    int capturing;       /* a mapping entry waiting for a key or button... */
    uint32_t cap_seen;   /* ...since settings_capture_count() was this */
    int wait_release;    /* no input until every button is up (after a capture) */
    int dialog;          /* the Reset Defaults dialog is open */
} Opt;
static Opt opts[4];      /* one per Options object (the title's, the sub-screen's) */
static Opt *opt_for(uint32_t obj) {
    for (int i = 0; i < 4; i++) if (opts[i].obj == obj) return &opts[i];
    for (int i = 0; i < 4; i++) if (!opts[i].obj) { memset(&opts[i], 0, sizeof opts[i]); opts[i].obj = obj; return &opts[i]; }
    memset(&opts[0], 0, sizeof opts[0]);
    opts[0].obj = obj;
    return &opts[0];
}
static void set_row_byte(uint32_t obj, int v) { wr8(obj + O_ROW, (uint8_t)v); }

/* the values a row offers (v) with their names, and the count */
static int values(int row, int *v, const char **names) {
    const Settings *s = settings();
    int k = 0;
    switch (row) {
    case R_RENDERER:
        v[k] = 0; names[k++] = "Software";
        v[k] = 1; names[k++] = "GPU (Vulkan)";
        break;
    case R_RESOLUTION: {
        static const char *const n[6] = { "Native  640 x 448", "x2  1280 x 896", "x3  1920 x 1344", "x4  2560 x 1792",
                                          "x5  3200 x 2240", "x6  3840 x 2688" };
        static char other[16];
        for (int i = 1; i <= 6; i++) { v[k] = i; names[k++] = n[i - 1]; }
        if (s->scale >= 7) { snprintf(other, sizeof other, "x%d", s->scale); v[k] = s->scale; names[k++] = other; }   /* from hg.ini */
        v[k] = 0; names[k++] = "Auto (this display)";
        break;
    }
    case R_FPS:
        v[k] = 30; names[k++] = "30 fps";
        v[k] = 60; names[k++] = "60 fps";
        if (s->scale == 1 || s->fps == 120) { v[k] = 120; names[k++] = "120 fps"; }   /* 120 only at native resolution */
        break;
    case R_DISPLAY:
        v[k] = 0; names[k++] = "Window";
        v[k] = 1; names[k++] = "Full screen";
        break;
    case R_ASPECT:
        v[k] = 0; names[k++] = "4:3 (original)";
        v[k] = 1; names[k++] = "Widescreen";
        break;
    case R_VSYNC:
        v[k] = VSYNC_ON; names[k++] = "On (smooth)";
        v[k] = VSYNC_MAILBOX; names[k++] = "Exact 59.94 Hz";
        v[k] = VSYNC_OFF; names[k++] = "Off (may tear)";
        break;
    case R_HD:
        v[k] = 1; names[k++] = "On";
        v[k] = 0; names[k++] = "Off";
        break;
    }
    return k;
}
static int current(int row) {
    const Settings *s = settings();
    switch (row) {
    case R_RENDERER: return s->renderer_next;
    case R_RESOLUTION: return s->scale;
    case R_FPS: return s->fps;
    case R_DISPLAY: return s->fullscreen;
    case R_ASPECT: return s->wide;
    case R_VSYNC: return s->vsync;
    case R_HD: return s->hd_on;
    }
    return 0;
}
static int value_index(int row, int val) {
    int v[16]; const char *n[16];
    const int k = values(row, v, n);
    for (int i = 0; i < k; i++) if (v[i] == val) return i;
    return 0;
}
static const char *value_name(int row, int val) {
    int v[16]; const char *n[16];
    const int k = values(row, v, n);
    return k ? n[value_index(row, val)] : "";
}
/* resolution, frame rate and HD textures need the GPU renderer, running now; HD textures a textures folder too */
static int row_locked(int row) { return ((row == R_RESOLUTION || row == R_FPS || row == R_HD) && !settings()->renderer) || (row == R_HD && hdtex_available() == 0); }
static void commit(int row, int val) {
    char b[16];
    snprintf(b, sizeof b, "%d", val);
    switch (row) {
    case R_RENDERER: settings_set("renderer", val ? "gpu" : "software", "Options menu"); break;
    case R_RESOLUTION:
        settings_set("resolution", val ? b : "auto", "Options menu");
        if (val != 1 && settings()->fps == 120) settings_set("fps", "60", "Options menu: 120 fps needs native resolution");
        break;
    case R_FPS: settings_set("fps", b, "Options menu"); break;
    case R_DISPLAY: settings_set("fullscreen", b, "Options menu"); break;
    case R_ASPECT: settings_set("aspect", val ? "wide" : "4:3", "Options menu"); break;
    case R_VSYNC: settings_set("vsync", val == VSYNC_ON ? "on" : val == VSYNC_OFF ? "off" : "mailbox", "Options menu"); break;
    case R_HD: settings_set("hd", val ? "on" : "off", "Options menu"); break;
    }
}

/* ------------------------------------------------------------------ drawing our pages */
static int blink_alpha(uint32_t obj) {   /* as the game's edit states blink their arrows */
    const int a = 128 - (int)((rd32(obj + O_FRAMES) * 4) & 0xFF);
    return a < 0 ? -a : a;
}
static const char *pad_type_name(void) {
    const int t = settings_pad_type();
    return t == PADKIND_PS5 ? "DualSense" : t == PADKIND_PS ? "PlayStation" : t == PADKIND_XBOX ? "Xbox / other" : "Not connected";
}
/* the controller page's value entries: the values offered (v) and their names; the light bar keeps a colour from
   hg.ini that is not one of the presets as an extra value */
static int pad_values(int e, int *v, const char **names) {
    int k = 0;
    if (e == E_VIB) for (int i = VIB_OFF; i <= VIB_HIGH; i++) { v[k] = i; names[k++] = settings_vibration_name(i); }
    else {
        const uint32_t *rgb; const int n = settings_lightbar_presets(&rgb);
        int found = 0;
        for (int i = 0; i < n; i++) { v[k] = (int)rgb[i]; names[k++] = settings_lightbar_name(rgb[i]); found |= rgb[i] == settings()->lightbar; }
        if (!found) { v[k] = (int)settings()->lightbar; names[k++] = settings_lightbar_name(settings()->lightbar); }
    }
    return k;
}
static int pad_current(int e) { return e == E_VIB ? settings()->vibration : (int)settings()->lightbar; }
static int pad_value_index(int e, int val) {
    int v[16]; const char *n[16];
    const int k = pad_values(e, v, n);
    for (int i = 0; i < k; i++) if (v[i] == val) return i;
    return 0;
}
static void draw_display_page(Draw *d, const Opt *o) {
    const Settings *s = settings();
    d_panel(d);
    d_plain(d, ROW_Y(0), ROW_Y(NROWS) + 32 - ROW_Y(0));       /* the game's rows and two-column area away */
    for (int r = 0; r < NROWS; r++) d_row_strip(d, ROW_Y(r));   /* a bar per row */
    const char *note = 0;                                                     /* the last row: a note, when one applies */
    if (s->renderer_next != s->renderer) note = "The renderer changes at the next start.";
    else if (!s->renderer) note = "Resolution, frame rate and HD need the GPU renderer.";
    else if (s->scale == 1 && s->hd_on && hdtex_available() > 0) note = "HD textures show above native resolution.";
    for (int r = 0; r < NROWS; r++) {
        const int y = ROW_Y(r), edit = o->editing && o->row == r;
        char val[64];
        d_sprite(d, 32, y, row_icon[r], 128);
        d_text(d, 72, y + 6, edit ? COL_EDIT : COL_NORMAL, row_label[r], 16);
        if (r == R_KEYBOARD) snprintf(val, sizeof val, "Change");
        else if (r == R_PAD) snprintf(val, sizeof val, "%s", pad_type_name());
        else if (row_locked(r)) snprintf(val, sizeof val, "%s", r == R_FPS ? "30 fps" : r == R_HD ? (settings()->renderer ? "Not installed" : "Off") : "Native  640 x 448");
        else snprintf(val, sizeof val, "%s", value_name(r, edit ? o->value : current(r)));
        d_text_centred(d, 352, y + 6, COL_NORMAL, val, 16);
        if (edit) {
            const int a = blink_alpha(d->obj);
            d_sprite(d, 224, y, SPR_ARROW_L, a);
            d_sprite(d, 448, y, SPR_ARROW_R, a);
        }
    }
    if (note) d_text_centred(d, 256, ROW_Y(NROWS) + 8, COL_NORMAL, note, 14);
    d_sprite(d, 32, ROW_Y(o->row), SPR_CURSOR, 128);
}
static void draw_map_page(Draw *d, const Opt *o) {
    const Settings *s = settings();
    const int kb = o->page == PG_KEYBOARD, n = kb ? PB_N : PAD_ENTRIES, kind = settings_pad_type();
    d_panel(d);
    d_columns(d, 236, 136);   /* the two columns down to the last row (the row bars and the slider line go) */
    d_sprite(d, 32, 84, SPR_ICON_PAD, 128);
    d_text(d, 72, 90, COL_NORMAL, kb ? "KEYBOARD" : "CONTROLLER", 16);
    if (!kb) d_text_centred(d, 352, 90, COL_NORMAL, pad_type_name(), 16);
    for (int i = 0; i < n; i++) {
        const int col = i / MAP_ROWS, y = MAP_Y(i % MAP_ROWS), x = col ? 272 : 40;
        const int sel = o->row == i, colour = sel ? COL_EDIT : COL_NORMAL;
        char val[48];
        if (!kb && i >= E_VIB) {   /* a value: as the display page's rows, the arrows blinking while it changes */
            int v[16]; const char *nm[16];
            const int k = pad_values(i, v, nm), edit = sel && o->editing;
            d_text(d, x, y, colour, i == E_VIB ? "Vibration" : "Light bar", 14);
            snprintf(val, sizeof val, "%s", k ? nm[pad_value_index(i, edit ? o->value : pad_current(i))] : "");
            d_text(d, x + 118, y, colour, val, 14);
            if (edit) {
                const int a = blink_alpha(d->obj);
                d_sprite(d, x + 118 - 30, y - 9, SPR_ARROW_L, a);
                d_sprite(d, x + 118 + d_width(d, val, 14) - 2, y - 9, SPR_ARROW_R, a);
            }
            continue;
        }
        const int pb = map_order[i];
        const char *glyph = pb == PB_CROSS ? G_CROSS : pb == PB_CIRCLE ? G_CIRCLE : pb == PB_SQUARE ? G_SQUARE : pb == PB_TRIANGLE ? G_TRIANGLE : 0;
        if (glyph) {   /* the game's own glyph, then the name (drawn apart: a glyph sets its own colour) */
            d_text(d, x, y, COL_NORMAL, glyph, 14);
            d_text(d, x + 18, y, colour, settings_button_label(pb), 14);
        } else d_text(d, x, y, colour, settings_button_label(pb), 14);
        if (sel && o->capturing) {
            snprintf(val, sizeof val, "%s", kb ? "Press a key" : "Press a button");
            d_text(d, x + 118, y, blink_alpha(d->obj) > 64 ? COL_EDIT : COL_NORMAL, val, 14);
        } else {
            snprintf(val, sizeof val, "%s", kb ? settings_key_name(s->kb[pb]) : settings_pad_name(s->pad[pb], kind));
            d_text(d, x + 118, y, colour, val, 14);
        }
    }
}
static void draw_page(Draw *d, const Opt *o) {
    if (o->page == PG_DISPLAY) draw_display_page(d, o);
    else draw_map_page(d, o);
}
static void draw_footer(Draw *d, const Opt *o) {
    if (o->page == PG_DISPLAY) d_text_at(d, 70, 390, COL_NORMAL, message(d, MSG_FOOTER), 16);   /* the game's own */
    else d_text(d, 70, 390, COL_NORMAL, G_CROSS ":Change  " G_TRIANGLE ":Back  " G_SQUARE ":Reset Defaults", 16);
}

/* ------------------------------------------------------------------ input */
static void reset_defaults(const Opt *o) {   /* the page's rows (the display page: not the renderer, which the game was started with) */
    settings_reset(o->page == PG_DISPLAY ? "display" : o->page == PG_KEYBOARD ? "keyboard" : "controller", "Options menu: Reset Defaults");
}
enum { IN_DRAW, IN_GAME_PAGE, IN_GAME_CODE };   /* what the frame does after the input */
static int input(Ctx *c, Opt *o) {
    const uint32_t obj = o->obj, p = rd32(PAD_PRESSED);
    if (rd8(obj + O_NOINPUT)) return IN_DRAW;
    if (o->dialog) {
        if (rd8(obj + O_DIALOG_STATE)) return IN_DRAW;   /* still open (it runs after the page is drawn) */
        o->dialog = 0;
        if (rd8(obj + O_DIALOG_ANSWER) == 0) reset_defaults(o);   /* YES */
        return IN_DRAW;
    }
    if (o->capturing) {
        if (settings_capture_count() == o->cap_seen) return IN_DRAW;
        o->capturing = 0;
        o->wait_release = 1;
        settings_capture_target(0, -1);
        sound(c, SND_OK);
        return IN_DRAW;
    }
    if (o->wait_release) {   /* the captured key may now stand for a button: not until it is up */
        if (rd32(PAD_RAW) || rd32(PAD_HELD)) return IN_DRAW;
        o->wait_release = 0;
    }
    if (o->page == PG_DISPLAY && o->editing) {
        int v[16]; const char *n[16];
        const int k = values(o->row, v, n);
        if (p & M_OK) {
            o->editing = 0;
            if (o->value != current(o->row)) commit(o->row, o->value);
            sound(c, SND_OK);
        } else if (p & M_BACK) {
            o->editing = 0;
            sound(c, SND_CANCEL);
        } else if (p & (M_LEFT | M_RIGHT)) {
            const int i = value_index(o->row, o->value);
            o->value = v[(i + ((p & M_RIGHT) ? 1 : k - 1)) % k];
            sound(c, SND_MOVE);
        }
        return IN_DRAW;
    }
    if (o->page == PG_DISPLAY) {
        if (p & M_OK) {
            if (o->row == R_KEYBOARD || o->row == R_PAD) { o->page = o->row == R_KEYBOARD ? PG_KEYBOARD : PG_PAD; o->row = 0; sound(c, SND_OK); }
            else if (row_locked(o->row)) sound(c, SND_CANCEL);
            else { o->editing = 1; o->value = current(o->row); sound(c, SND_OK); }
        } else if (p & M_UP) {
            sound(c, SND_MOVE);
            if (o->row > 0) o->row--;
            else { set_row_byte(obj, 4); o->page = PG_NONE; return IN_GAME_PAGE; }   /* up to Screen Position */
        } else if (p & M_DOWN) {
            sound(c, SND_MOVE);
            if (o->row < NROWS - 1) o->row++;
            else { set_row_byte(obj, 0); o->page = PG_NONE; return IN_GAME_PAGE; }   /* round to Controller Type */
        } else if (p & M_RESET) {
            dialog_open(c, obj);
            o->dialog = 1;
            sound(c, SND_OK);
        } else if (p & (M_TAB_A | M_TAB_B)) {   /* the sub-screen's tabs: the game's code, from its first row */
            set_row_byte(obj, 0);
            o->page = PG_NONE;
            return IN_GAME_CODE;
        } else if (p & M_BACK) return IN_GAME_CODE;   /* leave: the game's code does it (drawing this page through the hook) */
        return IN_DRAW;
    }
    /* a mapping page */
    const int n = o->page == PG_KEYBOARD ? PB_N : PAD_ENTRIES;
    if (o->page == PG_PAD && o->row >= E_VIB && o->editing) {   /* vibration or light bar being changed */
        int v[16]; const char *nm[16];
        const int k = pad_values(o->row, v, nm);
        if (p & M_OK) {
            o->editing = 0;
            if (o->value != pad_current(o->row)) {
                if (o->row == E_VIB) settings_set("vibration", settings_vibration_name(o->value), "Options menu");
                else { char b[16]; snprintf(b, sizeof b, "%06x", (unsigned)o->value & 0xFFFFFF); settings_set("lightbar", b, "Options menu"); }
            }
            sound(c, SND_OK);
        } else if (p & M_BACK) {
            o->editing = 0;
            sound(c, SND_CANCEL);
        } else if (p & (M_LEFT | M_RIGHT)) {
            const int i = pad_value_index(o->row, o->value);
            o->value = v[(i + ((p & M_RIGHT) ? 1 : k - 1)) % k];
            sound(c, SND_MOVE);
        }
        return IN_DRAW;
    }
    if (p & M_OK && o->page == PG_PAD && o->row >= E_VIB) {
        o->editing = 1;
        o->value = pad_current(o->row);
        sound(c, SND_OK);
    } else if (p & M_OK) {
        o->capturing = 1;
        o->cap_seen = settings_capture_count();
        if (!settings_replaying()) settings_capture_target(o->page == PG_KEYBOARD ? 0 : 1, map_order[o->row]);   /* (replays: the recording has it) */
        sound(c, SND_OK);
    } else if (p & M_BACK) {
        o->editing = 0;
        o->row = o->page == PG_KEYBOARD ? R_KEYBOARD : R_PAD;
        o->page = PG_DISPLAY;
        sound(c, SND_CANCEL);
    } else if (p & M_UP) {
        const int top = o->row - o->row % MAP_ROWS, last = top + MAP_ROWS - 1 < n ? top + MAP_ROWS - 1 : n - 1;
        o->row = o->row == top ? last : o->row - 1;
        sound(c, SND_MOVE);
    } else if (p & M_DOWN) {
        const int top = o->row - o->row % MAP_ROWS, last = top + MAP_ROWS - 1 < n ? top + MAP_ROWS - 1 : n - 1;
        o->row = o->row == last ? top : o->row + 1;
        sound(c, SND_MOVE);
    } else if (p & (M_LEFT | M_RIGHT)) {
        const int r = o->row % MAP_ROWS, other = o->row < MAP_ROWS ? MAP_ROWS + r : r;
        o->row = other < n ? other : n - 1;
        sound(c, SND_MOVE);
    } else if (p & M_RESET) {
        dialog_open(c, obj);
        o->dialog = 1;
        sound(c, SND_OK);
    }
    return IN_DRAW;
}

/* ------------------------------------------------------------------ the hooks */
static int hk_choose_row(Ctx *c) {   /* 0x393bd0(obj) */
    const uint32_t obj = c->r[4].u32[0];
    const int row = rd8(obj + O_ROW);
    Opt *o = opt_for(obj);
    if (row < 5) {
        const uint32_t p = rd32(PAD_PRESSED);
        o->page = PG_NONE;
        if (rd8(obj + O_NOINPUT)) return 0;
        /* the game tests Cross first, then Up, then Down: take only what would move its cursor round the page */
        const int down = row == 4 && !(p & (M_OK | M_UP)) && (p & M_DOWN), up = row == 0 && !(p & M_OK) && (p & M_UP);
        if (!down && !up) return 0;
        memset(o, 0, sizeof *o);
        o->obj = obj;
        o->page = PG_DISPLAY;
        o->row = down ? 0 : NROWS - 1;
        sound(c, SND_MOVE);
    } else {
        if (o->page == PG_NONE) { memset(o, 0, sizeof *o); o->obj = obj; o->page = PG_DISPLAY; o->row = row - 5 < NROWS ? row - 5 : 0; }
        const int next = input(c, o);
        if (next == IN_GAME_CODE) return 0;
        if (next == IN_GAME_PAGE) { game_page(c, obj); return 1; }   /* (not the game's code: it would act on this input too) */
    }
    set_row_byte(obj, o->page == PG_DISPLAY ? 5 + o->row : 16 + o->page);
    Draw d;
    d_open(&d, c, obj);
    draw_page(&d, o);
    draw_footer(&d, o);
    d_close(&d);
    if (o->dialog) dialog_run(c, obj);
    return 1;
}
static int hk_draw_page(Ctx *c) {   /* 0x391450(obj, editing): from any state while our rows are shown */
    const uint32_t obj = c->r[4].u32[0];
    const int row = rd8(obj + O_ROW);
    if (row < 5) return 0;
    Opt *o = opt_for(obj);
    if (o->page == PG_NONE) { o->page = PG_DISPLAY; o->row = row - 5 < NROWS ? row - 5 : 0; }
    Draw d;
    d_open(&d, c, obj);
    draw_page(&d, o);
    d_close(&d);
    return 1;
}

void options_init(void) {
    const char *e = getenv("HG_OPTIONS_ROWS");   /* 0: the game's Options page as it was (no extra rows) */
    if (e && !atoi(e)) { L("options: extra rows off (HG_OPTIONS_ROWS=0)\n"); return; }
    set_hook(FN_CHOOSE_ROW, hk_choose_row);
    set_hook(FN_DRAW_PAGE, hk_draw_page);
}
