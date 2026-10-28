/* Host settings: hg.ini, environment overrides, recordings. See settings.h for the rules. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <stdatomic.h>
#include <pthread.h>
#include "settings.h"
#include "hle.h"
#ifdef HAVE_SDL
#include <SDL.h>
#endif
#ifdef _WIN32
#include <windows.h>
#endif

static Settings eff;     /* in effect now */
static Settings file;    /* what hg.ini holds (and gets on save) */
static int loaded;
static char ini_path[1024];
static int ini_off;
static int replaying;    /* a recording is replayed: nothing is saved, host changes come from the recording */
static _Atomic uint32_t gen = 1;
static _Atomic int auto_scale = 0;     /* "auto" resolved by the UI thread from the display (0: not known) */
static _Atomic int pad_type_live = 0;  /* the UI thread's view */
static int pad_type_rec;               /* the game-visible one (recorded) */
static uint32_t capture_count;         /* key or button captures (and cancels) applied so far */

/* ------------------------------------------------------------------ names */
static const char *const pb_key[PB_N] = {
    "select", "l3", "r3", "start", "up", "right", "down", "left",
    "l2", "r2", "l1", "r1", "triangle", "circle", "cross", "square",
    "ls_up", "ls_down", "ls_left", "ls_right",
};
static const char *const pb_label[PB_N] = {
    "Select", "L3", "R3", "Start", "Up", "Right", "Down", "Left",
    "L2", "R2", "L1", "R1", "Triangle", "Circle", "Cross", "Square",
    "L-stick up", "L-stick down", "L-stick left", "L-stick right",
};
const char *settings_button_key(int pb) { return pb >= 0 && pb < PB_N ? pb_key[pb] : "?"; }
const char *settings_button_label(int pb) { return pb >= 0 && pb < PB_N ? pb_label[pb] : "?"; }

/* SDL game-controller buttons (SDL2 numbering), their hg.ini names and display names */
static const struct { int id; const char *ini, *ps5, *ps4, *xbox; } padnames[] = {
    { 0, "a", "Cross", "Cross", "A" },
    { 1, "b", "Circle", "Circle", "B" },
    { 2, "x", "Square", "Square", "X" },
    { 3, "y", "Triangle", "Triangle", "Y" },
    { 4, "back", "Create", "Share", "View" },
    { 5, "guide", "PS", "PS", "Xbox" },
    { 6, "start", "Options", "Options", "Menu" },
    { 7, "leftstick", "L3", "L3", "LS" },
    { 8, "rightstick", "R3", "R3", "RS" },
    { 9, "leftshoulder", "L1", "L1", "LB" },
    { 10, "rightshoulder", "R1", "R1", "RB" },
    { 11, "dpup", "Up", "Up", "Up" },
    { 12, "dpdown", "Down", "Down", "Down" },
    { 13, "dpleft", "Left", "Left", "Left" },
    { 14, "dpright", "Right", "Right", "Right" },
    { 15, "misc1", "Mute", "Misc", "Share" },
    { 16, "paddle1", "Paddle 1", "Paddle 1", "P1" },
    { 17, "paddle2", "Paddle 2", "Paddle 2", "P2" },
    { 18, "paddle3", "Paddle 3", "Paddle 3", "P3" },
    { 19, "paddle4", "Paddle 4", "Paddle 4", "P4" },
    { 20, "touchpad", "Touchpad", "Touchpad", "Touchpad" },
    { PAD_LT, "lefttrigger", "L2", "L2", "LT" },
    { PAD_RT, "righttrigger", "R2", "R2", "RT" },
};
#define NPADNAMES (int)(sizeof padnames / sizeof padnames[0])
const char *settings_pad_name(int input, int kind) {
    for (int i = 0; i < NPADNAMES; i++) if (padnames[i].id == input)
        return kind == PADKIND_PS5 ? padnames[i].ps5 : kind == PADKIND_PS ? padnames[i].ps4 : padnames[i].xbox;
    return "-";
}
static const char *const vib_names[4] = { "Off", "Low", "Medium", "High" };
const char *settings_vibration_name(int level) { return level >= 0 && level < 4 ? vib_names[level] : "?"; }
int settings_vibration_percent(int level) { static const int p[4] = { 0, 35, 65, 100 }; return level >= 0 && level < 4 ? p[level] : 100; }
/* light bar colours: dim, so the bar does not light up a dark room (the game is played in the dark) */
static const uint32_t lb_rgb[] = { 0x401a00, 0x400400, 0x000c40, 0x282828, 0x000000 };
static const char *const lb_name[] = { "Amber", "Red", "Blue", "White", "Off" };
#define NLB (int)(sizeof lb_rgb / sizeof lb_rgb[0])
int settings_lightbar_presets(const uint32_t **rgb) { *rgb = lb_rgb; return NLB; }
const char *settings_lightbar_name(uint32_t rgb) {
    for (int i = 0; i < NLB; i++) if (lb_rgb[i] == rgb) return lb_name[i];
    static char b[4][8]; static int k; char *o = b[k++ & 3]; snprintf(o, 8, "%06x", rgb & 0xFFFFFF); return o;
}
const char *settings_pad_ini(int input) {
    for (int i = 0; i < NPADNAMES; i++) if (padnames[i].id == input) return padnames[i].ini;
    return "none";
}
static int pad_parse(const char *s, int *out) {
    if (!strcasecmp(s, "none") || !strcmp(s, "-")) { *out = PAD_NONE; return 1; }
    for (int i = 0; i < NPADNAMES; i++) if (!strcasecmp(padnames[i].ini, s)) { *out = padnames[i].id; return 1; }
    return 0;
}
const char *settings_key_name(int sc) {
    if (sc <= 0) return "-";
#ifdef HAVE_SDL
    const char *n = SDL_GetScancodeName((SDL_Scancode)sc);
    if (n && *n) return n;
#endif
    static char b[4][16]; static int k; char *o = b[k++ & 3]; snprintf(o, 16, "sc%d", sc); return o;
}
static int key_parse(const char *s, int *out) {
    if (!strcasecmp(s, "none") || !strcmp(s, "-")) { *out = 0; return 1; }
    if (!strncmp(s, "sc", 2) && isdigit((unsigned char)s[2])) { *out = atoi(s + 2); return 1; }
#ifdef HAVE_SDL
    const SDL_Scancode sc = SDL_GetScancodeFromName(s);
    if (sc != SDL_SCANCODE_UNKNOWN) { *out = (int)sc; return 1; }
#endif
    return 0;
}

/* ------------------------------------------------------------------ defaults */
void settings_defaults(Settings *s) {
    memset(s, 0, sizeof *s);
    s->renderer = 0;        /* the software GS, as without settings (HG_GSVK=1 or renderer = gpu for the GPU) */
    s->scale = 0;           /* auto (from the display, at most x4) and 60 fps: with the GPU renderer the 0.1 defaults (2026-10-16:
                               x4 + 60 fps leaves a third of the frame's time to spare in the heaviest rooms tested on an
                               RTX 5090, x5 + 60 does not); the software renderer draws at native 30 fps whatever these say */
    s->fps = 60;
    s->fullscreen = 0;
    s->vsync = VSYNC_ON;    /* display-locked (host_sdl.c, pacing): every picture held the same number of refreshes */
    s->window_scale = 2;
    /* keyboard: SDL scancodes (USB HID usage ids) */
    s->kb[PB_UP] = 82;          /* Up */
    s->kb[PB_DOWN] = 81;        /* Down */
    s->kb[PB_LEFT] = 80;        /* Left */
    s->kb[PB_RIGHT] = 79;       /* Right */
    s->kb[PB_CROSS] = 27;       /* X */
    s->kb[PB_CIRCLE] = 6;       /* C */
    s->kb[PB_SQUARE] = 29;      /* Z */
    s->kb[PB_TRIANGLE] = 22;    /* S */
    s->kb[PB_START] = 40;       /* Return */
    s->kb[PB_SELECT] = 229;     /* Right Shift */
    s->kb[PB_L1] = 20;          /* Q */
    s->kb[PB_R1] = 8;           /* E */
    s->kb[PB_L2] = 30;          /* 1 */
    s->kb[PB_R2] = 32;          /* 3 */
    s->kb[PB_L3] = 0;
    s->kb[PB_R3] = 0;
    s->kb[PB_LS_UP] = 12;       /* I */
    s->kb[PB_LS_DOWN] = 14;     /* K */
    s->kb[PB_LS_LEFT] = 13;     /* J */
    s->kb[PB_LS_RIGHT] = 15;    /* L */
    /* controller */
    s->pad[PB_CROSS] = 0;
    s->pad[PB_CIRCLE] = 1;
    s->pad[PB_SQUARE] = 2;
    s->pad[PB_TRIANGLE] = 3;
    s->pad[PB_SELECT] = 4;
    s->pad[PB_START] = 6;
    s->pad[PB_L3] = 7;
    s->pad[PB_R3] = 8;
    s->pad[PB_L1] = 9;
    s->pad[PB_R1] = 10;
    s->pad[PB_UP] = 11;
    s->pad[PB_DOWN] = 12;
    s->pad[PB_LEFT] = 13;
    s->pad[PB_RIGHT] = 14;
    s->pad[PB_L2] = PAD_LT;
    s->pad[PB_R2] = PAD_RT;
    s->vibration = VIB_HIGH;     /* the game's vibration as the game made it */
    s->lightbar = 0x401a00;      /* dim amber */
    s->deadzone = 18;            /* about 6000 of 32768, as before settings existed */
    s->touchpad_select = 1;
    s->audio_offset = 0;
    s->hd_textures[0] = 0;
    s->hd_on = 1;
}

/* ------------------------------------------------------------------ values as text */
static const char *vsync_name(int v) { return v == VSYNC_ON ? "on" : v == VSYNC_OFF ? "off" : "mailbox"; }
static void value_of(const Settings *s, const char *key, char *out, size_t n) {
    out[0] = 0;
    if (!strcmp(key, "renderer")) snprintf(out, n, "%s", s->renderer ? "gpu" : "software");
    else if (!strcmp(key, "renderer_next")) snprintf(out, n, "%s", s->renderer_next ? "gpu" : "software");
    else if (!strcmp(key, "resolution")) { if (s->scale == 0) snprintf(out, n, "auto"); else snprintf(out, n, "%d", s->scale); }
    else if (!strcmp(key, "fps")) snprintf(out, n, "%d", s->fps);
    else if (!strcmp(key, "fullscreen")) snprintf(out, n, "%d", s->fullscreen);
    else if (!strcmp(key, "vsync")) snprintf(out, n, "%s", vsync_name(s->vsync));
    else if (!strcmp(key, "window_scale")) snprintf(out, n, "%d", s->window_scale);
    else if (!strcmp(key, "vibration")) { snprintf(out, n, "%s", settings_vibration_name(s->vibration)); for (char *c = out; *c; c++) *c = (char)tolower((unsigned char)*c); }
    else if (!strcmp(key, "lightbar")) { snprintf(out, n, "%s", settings_lightbar_name(s->lightbar)); for (char *c = out; *c; c++) *c = (char)tolower((unsigned char)*c); }
    else if (!strcmp(key, "deadzone")) snprintf(out, n, "%d", s->deadzone);
    else if (!strcmp(key, "touchpad_select")) snprintf(out, n, "%d", s->touchpad_select);
    else if (!strcmp(key, "audio_offset")) snprintf(out, n, "%d", s->audio_offset);
    else if (!strcmp(key, "hd_textures")) snprintf(out, n, "%s", s->hd_textures);
    else if (!strcmp(key, "hd")) snprintf(out, n, "%s", s->hd_on ? "on" : "off");
    else if (!strcmp(key, "aspect")) snprintf(out, n, "%s", s->wide ? "wide" : "4:3");
    else if (!strncmp(key, "kb.", 3)) {
        for (int i = 0; i < PB_N; i++) if (!strcmp(key + 3, pb_key[i])) snprintf(out, n, "%s", settings_key_name(s->kb[i]));
    } else if (!strncmp(key, "pad.", 4)) {
        for (int i = 0; i < PB_BUTTONS; i++) if (!strcmp(key + 4, pb_key[i])) snprintf(out, n, "%s", settings_pad_ini(s->pad[i]));
    }
}
/* one key into s; swap: a key or button already bound to another of the controller's buttons moves to it (so a
   change made in the menu never leaves two buttons on one key) */
static int apply_to(Settings *s, const char *key, const char *v, int swap) {
    if (!strcmp(key, "renderer") || !strcmp(key, "renderer_next")) {
        int r;
        if (!strcasecmp(v, "gpu") || !strcmp(v, "1")) r = 1;
        else if (!strcasecmp(v, "software") || !strcmp(v, "0")) r = 0;
        else return 0;
        if (!strcmp(key, "renderer")) s->renderer = r;
        s->renderer_next = r;   /* (renderer: both, as at start) */
        return 1;
    }
    if (!strcmp(key, "resolution")) {
        if (!strcasecmp(v, "auto")) { s->scale = 0; return 1; }
        if (!strcasecmp(v, "native")) { s->scale = 1; return 1; }
        const int n = atoi(v);
        if (n < 1 || n > 8) return 0;
        s->scale = n; return 1;
    }
    if (!strcmp(key, "fps")) {
        const int n = atoi(v);
        if (n != 30 && n != 60 && n != 120) return 0;
        s->fps = n; return 1;
    }
    if (!strcmp(key, "fullscreen")) { s->fullscreen = atoi(v) != 0 || !strcasecmp(v, "on") || !strcasecmp(v, "yes"); return 1; }
    if (!strcmp(key, "vsync")) {
        if (!strcasecmp(v, "on") || !strcasecmp(v, "fifo") || !strcmp(v, "1")) s->vsync = VSYNC_ON;
        else if (!strcasecmp(v, "off") || !strcasecmp(v, "immediate") || !strcmp(v, "0")) s->vsync = VSYNC_OFF;
        else if (!strcasecmp(v, "mailbox") || !strcasecmp(v, "default")) s->vsync = VSYNC_MAILBOX;
        else return 0;
        return 1;
    }
    if (!strcmp(key, "window_scale")) { const int n = atoi(v); if (n < 1 || n > 8) return 0; s->window_scale = n; return 1; }
    if (!strcmp(key, "vibration")) {
        for (int i = 0; i < 4; i++) if (!strcasecmp(v, vib_names[i])) { s->vibration = i; return 1; }
        if (isdigit((unsigned char)v[0]) && !v[1] && v[0] <= '3') { s->vibration = v[0] - '0'; return 1; }
        return 0;
    }
    if (!strcmp(key, "lightbar")) {
        for (int i = 0; i < NLB; i++) if (!strcasecmp(v, lb_name[i])) { s->lightbar = lb_rgb[i]; return 1; }
        char *end; const unsigned long c = strtoul(v[0] == '#' ? v + 1 : v, &end, 16);
        if (*end || strlen(v[0] == '#' ? v + 1 : v) != 6) return 0;
        s->lightbar = (uint32_t)c; return 1;
    }
    if (!strcmp(key, "deadzone")) { const int n = atoi(v); if (n < 0 || n > 50 || !isdigit((unsigned char)v[0])) return 0; s->deadzone = n; return 1; }
    if (!strcmp(key, "touchpad_select")) { s->touchpad_select = atoi(v) != 0 || !strcasecmp(v, "on") || !strcasecmp(v, "yes"); return 1; }
    if (!strcmp(key, "audio_offset")) { char *e; const long n = strtol(v, &e, 10); if (*e || e == v || n < -200 || n > 200) return 0; s->audio_offset = (int)n; return 1; }
    if (!strcmp(key, "hd_textures")) { if (strlen(v) >= sizeof s->hd_textures) return 0; snprintf(s->hd_textures, sizeof s->hd_textures, "%s", v); return 1; }
    if (!strcmp(key, "hd")) {
        if (!strcasecmp(v, "on") || !strcmp(v, "1")) { s->hd_on = 1; return 1; }
        if (!strcasecmp(v, "off") || !strcmp(v, "0")) { s->hd_on = 0; return 1; }
        return 0;
    }
    if (!strcmp(key, "aspect")) {
        if (!strcasecmp(v, "wide") || !strcmp(v, "1")) { s->wide = 1; return 1; }
        if (!strcmp(v, "4:3") || !strcmp(v, "0")) { s->wide = 0; return 1; }
        return 0;
    }
    if (!strncmp(key, "kb.", 3)) {
        for (int i = 0; i < PB_N; i++) if (!strcmp(key + 3, pb_key[i])) {
            int sc; if (!key_parse(v, &sc)) return 0;
            if (swap && sc) for (int j = 0; j < PB_N; j++) if (j != i && s->kb[j] == sc) s->kb[j] = s->kb[i];
            s->kb[i] = sc; return 1;
        }
        return 0;
    }
    if (!strncmp(key, "pad.", 4)) {
        for (int i = 0; i < PB_BUTTONS; i++) if (!strcmp(key + 4, pb_key[i])) {
            int in; if (!pad_parse(v, &in)) return 0;
            if (swap && in != PAD_NONE) for (int j = 0; j < PB_BUTTONS; j++) if (j != i && s->pad[j] == in) s->pad[j] = s->pad[i];
            s->pad[i] = in; return 1;
        }
        return 0;
    }
    if (!strcmp(key, "padtype")) { const int t = atoi(v); if (t < 0 || t > 3) return 0; pad_type_rec = t; return 1; }
    return 0;
}

/* ------------------------------------------------------------------ environment */
typedef struct { const char *env, *key; } EnvMap;
static const EnvMap envmap[] = {
    { "HG_GSVK", "renderer" }, { "HG_GSVK_SCALE", "resolution" }, { "HG_GSVK_INTERP", "fps" },
    { "HG_VSYNC", "vsync" }, { "HG_SCALE", "window_scale" }, { "HG_FULLSCREEN", "fullscreen" }, { "HG_WIDE", "aspect" },
};
#define NENV (int)(sizeof envmap / sizeof envmap[0])
static int env_set[NENV];
/* the environment's value for a key in hg.ini terms, keeping the meaning each variable had before settings existed */
static int env_value(int i, char *out, size_t n) {
    const char *e = getenv(envmap[i].env);
    if (!e) return 0;
    const int v = atoi(e);
    switch (i) {
    case 0: snprintf(out, n, "%s", v > 0 ? "gpu" : "software"); break;                  /* HG_GSVK */
    case 1: snprintf(out, n, "%d", v >= 2 && v <= 8 ? v : 1); break;                    /* HG_GSVK_SCALE: 2-8, else native */
    case 2: snprintf(out, n, "%d", v >= 2 ? 120 : v == 1 ? 60 : 30); break;             /* HG_GSVK_INTERP */
    case 3: snprintf(out, n, "%s", v == 2 || !strcasecmp(e, "mailbox") ? "mailbox" : v ? "on" : "off"); break;   /* HG_VSYNC: 1 on, 0 off, 2 or mailbox */
    case 4: snprintf(out, n, "%d", v < 1 ? 1 : v > 8 ? 8 : v); break;                   /* HG_SCALE */
    default: snprintf(out, n, "%d", v != 0); break;
    }
    return 1;
}
static int env_index(const char *key) { for (int i = 0; i < NENV; i++) if (!strcmp(envmap[i].key, key)) return i; return -1; }
static void apply_env(void) {
    char v[64];
    for (int i = 0; i < NENV; i++) { env_set[i] = env_value(i, v, sizeof v); if (env_set[i]) apply_to(&eff, envmap[i].key, v, 0); }
}

/* ------------------------------------------------------------------ hg.ini */
static void find_ini(void) {
    const char *e = getenv("HG_INI");
    if (e && !strcasecmp(e, "off")) { ini_off = 1; ini_path[0] = 0; return; }
    if (e && *e) { snprintf(ini_path, sizeof ini_path, "%s", e); return; }
#ifdef _WIN32
    char exe[1024]; DWORD n = GetModuleFileNameA(0, exe, sizeof exe);
    if (n > 0 && n < sizeof exe) {
        char *sl = strrchr(exe, '\\'); if (!sl) sl = strrchr(exe, '/');
        if (sl) { sl[1] = 0; snprintf(ini_path, sizeof ini_path, "%shg.ini", exe); return; }
    }
#endif
    snprintf(ini_path, sizeof ini_path, "hg.ini");
}
const char *settings_ini_path(void) { return ini_off ? "" : ini_path; }
static int load_ini(Settings *s) {
    if (ini_off) return 0;
    FILE *f = fopen(ini_path, "r"); if (!f) return 0;
    char line[512]; int ln = 0;
    while (fgets(line, sizeof line, f)) {
        ln++;
        char *p = line; while (isspace((unsigned char)*p)) p++;
        if (!*p || *p == '#' || *p == ';' || *p == '[') continue;
        char *eq = strchr(p, '='); if (!eq) continue;
        *eq = 0; char *k = p, *v = eq + 1;
        char *c = strchr(v, '#'); if (c) *c = 0;
        c = strchr(v, ';'); if (c) *c = 0;
        for (char *t = k + strlen(k); t > k && isspace((unsigned char)t[-1]); ) *--t = 0;
        while (isspace((unsigned char)*v)) v++;
        for (char *t = v + strlen(v); t > v && isspace((unsigned char)t[-1]); ) *--t = 0;
        if (!apply_to(s, k, v, 0)) L("settings: %s line %d: \"%s = %s\" not understood (ignored)\n", ini_path, ln, k, v);
    }
    fclose(f); return 1;
}
void settings_save(void) {
    if (ini_off || replaying) return;
    char tmp[1100]; snprintf(tmp, sizeof tmp, "%s.tmp", ini_path);
    FILE *f = fopen(tmp, "w"); if (!f) { L("settings: cannot write %s\n", tmp); return; }
    char v[64];
    fprintf(f, "# Haunting Ground Recomp settings, written by the game's Options menu (the rows after Screen Position).\n");
    fprintf(f, "# Environment variables (HG_GSVK, HG_GSVK_SCALE, HG_GSVK_INTERP, HG_VSYNC, HG_SCALE, HG_FULLSCREEN, HG_WIDE) override this file.\n\n");
    fprintf(f, "renderer = %s        # software | gpu (takes effect at the next start)\n", file.renderer ? "gpu" : "software");
    value_of(&file, "resolution", v, sizeof v);
    fprintf(f, "resolution = %s      # 1 (native) .. 8, or auto (from the display); the GPU renderer only\n", v);
    fprintf(f, "fps = %d               # 30 | 60 | 120 (60 and 120 add about 17 ms of delay; 120 at native resolution)\n", file.fps);
    fprintf(f, "fullscreen = %d\n", file.fullscreen);
    fprintf(f, "vsync = %s       # on (smooth: locked to the display, default) | mailbox (exact 59.94 Hz, no tearing) | off (may tear)\n", vsync_name(file.vsync));
    fprintf(f, "window_scale = %d      # the window's size at start, times 640 x 448\n", file.window_scale);
    fprintf(f, "aspect = %s           # 4:3 (the original picture) | wide (fills the display, up to 21:9; menus stay 4:3)\n\n", file.wide ? "wide" : "4:3");
    fprintf(f, "# keyboard: SDL key names (\"none\" for no key)\n");
    for (int i = 0; i < PB_N; i++) fprintf(f, "kb.%s = %s\n", pb_key[i], settings_key_name(file.kb[i]));
    fprintf(f, "\n# controller: a b x y back guide start leftstick rightstick leftshoulder rightshoulder dpup dpdown dpleft\n");
    fprintf(f, "# dpright misc1 paddle1-4 touchpad lefttrigger righttrigger none\n");
    for (int i = 0; i < PB_BUTTONS; i++) fprintf(f, "pad.%s = %s\n", pb_key[i], settings_pad_ini(file.pad[i]));
    fprintf(f, "\n# controller extras\n");
    value_of(&file, "vibration", v, sizeof v);
    fprintf(f, "vibration = %s      # off | low | medium | high: the game's vibration (its own Vibration option still applies)\n", v);
    value_of(&file, "lightbar", v, sizeof v);
    fprintf(f, "lightbar = %s       # amber | red | blue | white | off, or a colour RRGGBB (PlayStation controllers)\n", v);
    fprintf(f, "deadzone = %d          # the analog sticks' dead zone, percent (0-50)\n", file.deadzone);
    fprintf(f, "touchpad_select = %d    # 1: clicking the touchpad is Select too (unless pad.<button> = touchpad)\n", file.touchpad_select);
    fprintf(f, "\n# sound\n");
    fprintf(f, "audio_offset = %d       # ms: raise it if the sound still seems late (speakers, TV), lower it if early (-200..200)\n", file.audio_offset);
    fprintf(f, "\n# textures\n");
    fprintf(f, "hd = %s               # on | off: HD textures, when there are some (the Options menu's \"HD textures\")\n", file.hd_on ? "on" : "off");
    fprintf(f, "hd_textures = %s       # a folder of HD replacements (<id>.dds or .png, tools/textures); empty: the textures folder next to the game; used with the GPU renderer when upscaling\n", file.hd_textures);
    fclose(f);
    remove(ini_path);
    if (rename(tmp, ini_path)) L("settings: cannot replace %s\n", ini_path);
}

/* ------------------------------------------------------------------ start */
static void log_line(const char *what) {
    char res[16];
    if (eff.scale == 0) snprintf(res, sizeof res, "auto"); else if (eff.scale == 1) snprintf(res, sizeof res, "native"); else snprintf(res, sizeof res, "x%d", eff.scale);
    char envs[256] = ""; for (int i = 0; i < NENV; i++) if (env_set[i]) { strcat(envs, " "); strcat(envs, envmap[i].env); }
    L("settings%s: renderer %s, resolution %s, %d fps, %s, vsync %s, window x%d, vibration %s, light bar %s, dead zone %d%% (file: %s%s; environment:%s)\n", what,
      eff.renderer ? "gpu" : "software", res, eff.fps, eff.fullscreen ? "full screen" : "window", vsync_name(eff.vsync), eff.window_scale,
      settings_vibration_name(eff.vibration), settings_lightbar_name(eff.lightbar), eff.deadzone,
      ini_off ? "off" : ini_path, loaded ? "" : ", none", envs[0] ? envs : " none");
}
void settings_init(void) {
    static int done; if (done) return; done = 1;
    settings_defaults(&file);
    find_ini();
    loaded = load_ini(&file);
    eff = file;
    apply_env();
    replaying = getenv("HG_PADPLAY") != 0;
    log_line("");
}
const Settings *settings(void) { return &eff; }

/* ------------------------------------------------------------------ the renderer's view */
int settings_auto_scale(void) {
    const int a = atomic_load(&auto_scale);
    return a > 0 ? a : 1;
}
int settings_gs_scale(void) {
    if (!eff.renderer || eff.fps == 120) return 1;
    const int s = eff.scale == 0 ? settings_auto_scale() : eff.scale;
    return s < 1 ? 1 : s > 8 ? 8 : s;
}
int settings_gs_interp(void) { return !eff.renderer ? 0 : eff.fps == 120 ? 2 : eff.fps == 60 ? 1 : 0; }
int settings_ui_display_rate_x2(void) { return settings_gs_interp() == 2; }
void settings_ui_auto_scale(int h) {
    int n = 1; while (n < 4 && 448 * n < h) n++;   /* the smallest factor that fills the display's height, at most 4 (a 4K display: x4,
                                                       2560 x 1792 stretched to 2880 x 2160; x5 and up by hand) */
    if (atomic_exchange(&auto_scale, n) != n) atomic_fetch_add(&gen, 1);
}
uint32_t settings_generation(void) { return atomic_load(&gen); }
int settings_pad_type(void) { return pad_type_rec; }
/* widescreen (eff.wide): the shape the picture is shown in, width / height. 4:3 is the console's 640 x 448 on a TV;
   wide follows the window (the UI thread reports its size), from that up to 2.4 (3440 x 1440); with no window, 16:9. */
static _Atomic int win_w, win_h;
void settings_ui_window_size(int w, int h) { atomic_store(&win_w, w); atomic_store(&win_h, h); }
double settings_display_aspect(void) {
    const double a43 = 640.0 / 448.0;
    if (!eff.wide) return a43;
    const int w = atomic_load(&win_w), h = atomic_load(&win_h);
    const double a = w > 0 && h > 0 ? (double)w / h : 16.0 / 9.0;
    return a < a43 ? a43 : a > 2.4 ? 2.4 : a;
}
float settings_wide_factor(void) { return (float)(640.0 / 448.0 / settings_display_aspect()); }

/* ------------------------------------------------------------------ changes */
int settings_set(const char *key, const char *value, const char *why) {
    const int ren = !strcmp(key, "renderer");   /* the renderer: for the next start */
    char before[64], after[64];
    value_of(&eff, ren ? "renderer_next" : key, before, sizeof before);
    Settings t = eff;
    if (!apply_to(&t, key, value, 1)) { L("settings: %s=%s not understood\n", key, value); return 0; }
    if (ren) {
        if (t.renderer_next == eff.renderer_next) return 1;
        file.renderer = t.renderer_next;
        eff.renderer_next = t.renderer_next;
    } else {
        if (!memcmp(&t, &eff, sizeof t)) return 1;
        eff = t;
        apply_to(&file, key, value, 1);
    }
    value_of(&eff, ren ? "renderer_next" : key, after, sizeof after);
    const int ei = env_index(key);
    L("settings: %s %s -> %s (%s)%s%s\n", key, before, after, why,
      ei >= 0 && env_set[ei] ? "; note: the environment sets it at start: " : "", ei >= 0 && env_set[ei] ? envmap[ei].env : "");
    atomic_fetch_add(&gen, 1);
    settings_save();
    return 1;
}
void settings_reset(const char *group, const char *why) {
    Settings d, t = eff;
    settings_defaults(&d);
    if (!strcmp(group, "display")) { t.scale = d.scale; t.fps = d.fps; t.fullscreen = d.fullscreen; t.vsync = d.vsync; t.hd_on = d.hd_on; t.wide = d.wide; }
    else if (!strcmp(group, "keyboard")) memcpy(t.kb, d.kb, sizeof t.kb);
    else if (!strcmp(group, "controller")) { memcpy(t.pad, d.pad, sizeof t.pad); t.vibration = d.vibration; t.lightbar = d.lightbar; }
    else return;
    if (!memcmp(&t, &eff, sizeof t)) return;
    eff = t;
    if (!strcmp(group, "display")) { file.scale = d.scale; file.fps = d.fps; file.fullscreen = d.fullscreen; file.vsync = d.vsync; file.hd_on = d.hd_on; file.wide = d.wide; }
    else if (!strcmp(group, "keyboard")) memcpy(file.kb, d.kb, sizeof file.kb);
    else { memcpy(file.pad, d.pad, sizeof file.pad); file.vibration = d.vibration; file.lightbar = d.lightbar; }
    L("settings: %s back to the defaults (%s)\n", group, why);
    atomic_fetch_add(&gen, 1);
    settings_save();
}

/* ------------------------------------------------------------------ recordings */
static const char *const rec_keys[] = { "renderer", "renderer_next", "resolution", "fps", "fullscreen", "vsync", "window_scale",
                                        "vibration", "lightbar", "aspect" };   /* (the last two: shown on the controller page) */
void settings_write_header(FILE *f) {
    char v[64];
    for (size_t i = 0; i < sizeof rec_keys / sizeof rec_keys[0]; i++) { value_of(&eff, rec_keys[i], v, sizeof v); fprintf(f, "# set %s=%s\n", rec_keys[i], v); }
    for (int i = 0; i < PB_N; i++) fprintf(f, "# set kb.%s=%s\n", pb_key[i], settings_key_name(eff.kb[i]));
    for (int i = 0; i < PB_BUTTONS; i++) fprintf(f, "# set pad.%s=%s\n", pb_key[i], settings_pad_ini(eff.pad[i]));
    fprintf(f, "# set padtype=%d\n", pad_type_rec);
}
void settings_replay_header(const char *key, const char *value) {
    replaying = 1;
    const int ei = env_index(key);
    if (ei >= 0 && env_set[ei]) {
        char now[64]; value_of(&eff, key, now, sizeof now);
        if (strcmp(now, value)) L("replay: recorded with %s=%s, now %s (from %s): frames that show the setting may differ\n", key, value, now, envmap[ei].env);
        return;
    }
    if (!apply_to(&eff, key, value, 0)) L("replay: setting %s=%s not understood\n", key, value);
    atomic_fetch_add(&gen, 1);
}
typedef struct { uint64_t vb; char key[32], value[64]; } SetEv;
static SetEv *rev; static size_t rev_n, rev_cap, rev_i;
void settings_replay_event(uint64_t vb, const char *key, const char *value) {
    replaying = 1;
    if (rev_n == rev_cap) { rev_cap = rev_cap ? rev_cap * 2 : 64; rev = realloc(rev, rev_cap * sizeof *rev); }
    SetEv *e = &rev[rev_n++]; e->vb = vb;
    snprintf(e->key, sizeof e->key, "%s", key); snprintf(e->value, sizeof e->value, "%s", value);
}
/* host-originated requests (UI thread -> game thread) */
static pthread_mutex_t hq_mx = PTHREAD_MUTEX_INITIALIZER;
static struct { char key[32], value[64]; } hq[16]; static int hq_n;
void settings_host_request(const char *key, const char *value) {
    pthread_mutex_lock(&hq_mx);
    if (hq_n < 16) { snprintf(hq[hq_n].key, sizeof hq[0].key, "%s", key); snprintf(hq[hq_n].value, sizeof hq[0].value, "%s", value); hq_n++; }
    pthread_mutex_unlock(&hq_mx);
}
void settings_set_pad_type_live(int t) {
    if (atomic_exchange(&pad_type_live, t) == t) return;
    char v[8]; snprintf(v, sizeof v, "%d", t); settings_host_request("padtype", v);
}
static void host_change(uint64_t vb, FILE *rec, const char *key, const char *value) {
    if (!strcmp(key, "capture")) { capture_count++; L("settings: key capture cancelled\n"); }
    else if (!strcmp(key, "padtype")) {
        if (pad_type_rec == atoi(value)) return;
        apply_to(&eff, key, value, 0); atomic_fetch_add(&gen, 1);
        L("settings: controller type for the names: %s\n", pad_type_rec == PADKIND_PS5 ? "DualSense" : pad_type_rec == PADKIND_PS ? "PlayStation" : pad_type_rec == PADKIND_XBOX ? "Xbox" : "none");
    } else {
        settings_set(key, value, "host");
        if (!strncmp(key, "kb.", 3) || !strncmp(key, "pad.", 4)) capture_count++;
    }
    if (rec) { fprintf(rec, "set %llu %s=%s\n", (unsigned long long)vb, key, value); fflush(rec); }
}
void settings_vblank(uint64_t vb, int rr_mode, FILE *rec) {
    if (rr_mode == 2) {   /* replay: the recorded host changes; live ones are dropped */
        while (rev_i < rev_n && rev[rev_i].vb <= vb) {
            const SetEv *e = &rev[rev_i++];
            if (!strcmp(e->key, "capture")) { capture_count++; continue; }
            if (!strcmp(e->key, "padtype")) { apply_to(&eff, e->key, e->value, 0); atomic_fetch_add(&gen, 1); continue; }
            if (apply_to(&eff, e->key, e->value, 1)) {
                L("settings: %s -> %s (recorded at VBlank %llu)\n", e->key, e->value, (unsigned long long)e->vb);
                atomic_fetch_add(&gen, 1);
                if (!strncmp(e->key, "kb.", 3) || !strncmp(e->key, "pad.", 4)) capture_count++;
            }
        }
        pthread_mutex_lock(&hq_mx); hq_n = 0; pthread_mutex_unlock(&hq_mx);
        return;
    }
    {   /* HG_HOSTEV="VB:key=value,..." (tests): host changes at those VBlanks, as if the window had made them */
        static char *ev; static int init;
        if (!init) { init = 1; const char *e = getenv("HG_HOSTEV"); if (e) ev = strdup(e); }
        for (char *p = ev; p && *p; ) {
            char *end = strchr(p, ','); if (end) *end = 0;
            unsigned long long at; char k[32], v[64];
            if (sscanf(p, "%llu:%31[^=]=%63s", &at, k, v) == 3 && at == vb) settings_host_request(k, v);
            if (end) *end = ',';
            p = end ? end + 1 : 0;
        }
    }
    pthread_mutex_lock(&hq_mx);
    int n = hq_n; char k[16][32], v[16][64];
    for (int i = 0; i < n; i++) { memcpy(k[i], hq[i].key, 32); memcpy(v[i], hq[i].value, 64); }
    hq_n = 0;
    pthread_mutex_unlock(&hq_mx);
    for (int i = 0; i < n; i++) host_change(vb, rr_mode == 1 ? rec : 0, k[i], v[i]);
}
uint32_t settings_capture_count(void) { return capture_count; }
static _Atomic int cap_target = -1;
void settings_capture_target(int kind, int pb) { atomic_store(&cap_target, pb < 0 ? -1 : kind << 8 | pb); }
int settings_ui_capture_target(void) { return atomic_exchange(&cap_target, -1); }
int settings_replaying(void) { return replaying; }
int settings_env_overrides(const char *key) { const int i = env_index(key); return i >= 0 && env_set[i]; }
