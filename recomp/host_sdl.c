/* Host window and input (SDL2). A UI thread owns the window: it polls keyboard/gamepad and shows the GS pictures,
   one every 1/59.94 s (1/119.88 s with HG_GSVK_INTERP=2), from the queue the GS thread fills (gs.c). With the GPU
   renderer (HG_GSVK=1) the pictures stay in video memory and the UI thread blits them into a Vulkan swapchain on the
   window (gsvk_present, vsync; HG_VSYNC=0: mailbox); otherwise, or with HG_PRESENT=sdl, they are copied into an SDL
   texture. Once per
   VBlank hle.c calls host_vblank() on the game thread, which keeps emulated time at real-time pace and hands the
   latest input to the emulated DualShock 2 (iop_pad_mc.c). Without SDL (or with HG_WINDOW=0, or no display)
   everything here is a no-op and the runtime stays headless.

   Keyboard and controller mappings come from the settings (settings.c: hg.ini, changed in the game's Options menu).
   The defaults: arrows = D-pad, X = Cross, C = Circle, Z = Square, S = Triangle, Enter = Start, Right Shift = Select,
             Q/E = L1/R1, 1/3 = L2/R2, I/J/K/L = left stick; F11 = fullscreen, F3 = overlay, F9 = mark the moment in the
             log (for reporting what was seen), Esc = quit.
   Gamepad (SDL GameController): A/B/X/Y = Cross/Circle/Square/Triangle, shoulders = L1/R1, triggers = L2/R2,
             sticks = analog sticks (not mapped), stick clicks = L3/R3, Back = Select, Start = Start; on a
             PlayStation controller the same buttons by their own names (Cross = Cross ...), Create/Share = Select,
             Options = Start, and a click of the touchpad is Select too (touchpad_select).
   Controllers (docs/controller.md): every controller SDL sees is opened (USB or Bluetooth; DualSense and DualShock 4
   through SDL's HIDAPI drivers, with their extended reports switched on so rumble and the light bar work over
   Bluetooth too); the first one to press a button becomes player 1 and is the only one read, until it goes. The
   game's DualShock 2 vibration (iop_pad_mc.c: host_rumble each VBlank) is played on player 1's motors, scaled by
   the vibration setting; PlayStation controllers get the light bar colour of the settings. A gamecontrollerdb.txt
   next to the executable adds SDL mappings for unusual controllers.
   Settings the UI thread applies itself when they change: full screen, vsync (the swapchain's present mode), the
   presenting rate (60 or 120 a second, following the GS's frame interpolation). It also tells the settings which
   kind of controller is connected (the names in the Options menu) and the display's height ("auto" resolution), and
   turns the next key or button into a mapping while the Options menu waits for one. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "ee_runtime.h"
#include "hle.h"
#include "settings.h"

int gs_display(uint32_t *out, int maxw, int maxh, int *pw, int *ph);
int gs_display_gpu(int *slot, uint64_t *id);
int gs_display_gpu_q(int *slot, uint64_t *id, int keep, int *ready, int *queued);
uint64_t gs_display_tag(void);
double host_idle;   /* seconds slept to pace the window to real time (HG_SPEEDLOG leaves them out) */

#ifdef HAVE_SDL
#define SDL_MAIN_HANDLED          /* our own main(): no SDL2main, and the console stays */
#include <SDL.h>
#ifdef _WIN32
#include <SDL_syswm.h>     /* (the window's HWND for Vulkan; elsewhere SDL_vulkan.h: no X11 headers needed) */
#endif
#if defined HAVE_VULKAN && !defined _WIN32
#include <SDL_vulkan.h>
#endif
#ifdef HAVE_VULKAN
#include "gsvk.h"
#endif

enum { B_SELECT = 0, B_L3, B_R3, B_START, B_UP, B_RIGHT, B_DOWN, B_LEFT, B_L2, B_R2, B_L1, B_R1, B_TRIANGLE, B_CIRCLE, B_CROSS, B_SQUARE };
static SDL_Window *win; static SDL_Renderer *ren; static SDL_Texture *tex;
#define MAXPADS 8
static SDL_GameController *pads[MAXPADS];   /* every controller open */
static SDL_GameController *pad;             /* player 1: the first to press a button (0 until one does) */
static SDL_atomic_t rumble_req, rumble_tick;   /* game thread -> UI thread: small << 8 | big, SDL ticks of that VBlank */
static int tex_w, tex_h, active = -1;
static SDL_atomic_t ui_ready, ui_quit, in_held, in_an;   /* UI thread -> game thread */
static SDL_atomic_t ui_stop, ui_stopped;                 /* host_shutdown() */
static uint32_t *frame;   /* the SDL path: up to 4096 x 4096 (the upscaled picture, HG_GSVK_SCALE <= 6) */
static int vkmode;   /* 0: SDL renderer; 1: waiting for the GPU renderer to present from video memory; 2: presenting from it */

/* the mappings in use (copied from the settings whenever they change) */
static int kbmap[PB_N], padmap[PB_BUTTONS];
static uint32_t ui_gen;        /* the settings generation last applied by the UI thread */
static int fs_flip;            /* F11 while a recording is replayed: the window only (the settings come from it) */
static int ui_vsync = -1;      /* the present mode last asked of the swapchain */
static int ui_vib = VIB_HIGH, ui_deadzone = 18, ui_touchsel = 1;   /* controller settings (copied like the maps) */
static uint32_t ui_lightbar = 0x401a00;

int host_window_wanted(void) { const char *w = getenv("HG_WINDOW"); return !(w && !strcmp(w, "0"))
#ifndef _WIN32
        && (getenv("DISPLAY") || getenv("WAYLAND_DISPLAY"))
#endif
        ; }
static volatile int host_ff;   /* HG_FF: before its VBlank (see the pacing in host_vblank) */
int host_window_active(void) { return active == 1; }
int host_fast_forward(void) { return host_ff; }   /* HG_FF: before its VBlank */
static int pad_kind(SDL_GameController *c) {   /* for the button names: PADKIND_* */
    if (!c) return PADKIND_NONE;
    const SDL_GameControllerType t = SDL_GameControllerGetType(c);
    if (t == SDL_CONTROLLER_TYPE_PS5) return PADKIND_PS5;
    if (t == SDL_CONTROLLER_TYPE_PS3 || t == SDL_CONTROLLER_TYPE_PS4) return PADKIND_PS;
    return PADKIND_XBOX;
}
static const char *type_name(SDL_GameController *c) {
    switch (SDL_GameControllerGetType(c)) {
    case SDL_CONTROLLER_TYPE_PS5: return "PS5 (DualSense)";
    case SDL_CONTROLLER_TYPE_PS4: return "PS4 (DualShock 4)";
    case SDL_CONTROLLER_TYPE_PS3: return "PS3";
    case SDL_CONTROLLER_TYPE_XBOX360: return "Xbox 360";
    case SDL_CONTROLLER_TYPE_XBOXONE: return "Xbox One";
    case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_PRO: return "Switch Pro";
    case SDL_CONTROLLER_TYPE_VIRTUAL: return "virtual";
    default: return "other";
    }
}
/* the controller that names the buttons: player 1, or before anyone pressed a button the first one open */
static void note_kind(void) {
    SDL_GameController *c = pad;
    for (int i = 0; !c && i < MAXPADS; i++) c = pads[i];
    settings_set_pad_type_live(pad_kind(c));
}
static void set_lightbar(SDL_GameController *c) {
    if (c && SDL_GameControllerHasLED(c)) SDL_GameControllerSetLED(c, ui_lightbar >> 16 & 0xFF, ui_lightbar >> 8 & 0xFF, ui_lightbar & 0xFF);
}
static void pad_pressed(SDL_JoystickID id);
static void open_pad(int idx) {
    if (!SDL_IsGameController(idx)) return;
    const SDL_JoystickID id = SDL_JoystickGetDeviceInstanceID(idx);
    for (int i = 0; i < MAXPADS; i++) if (pads[i] && SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(pads[i])) == id) return;   /* open already */
    int slot = 0; while (slot < MAXPADS && pads[slot]) slot++;
    if (slot == MAXPADS) return;
    SDL_GameController *c = SDL_GameControllerOpen(idx);
    if (!c) { L("host: gamepad %d: %s\n", idx, SDL_GetError()); return; }
    pads[slot] = c;
    SDL_GameControllerSetPlayerIndex(c, -1);   /* (no player LEDs until it is player 1) */
    const char *name = SDL_GameControllerName(c);
    L("host: gamepad \"%s\" connected: type %s, rumble %s, light bar %s, touchpad %s%s\n", name ? name : "?", type_name(c),
      SDL_GameControllerHasRumble(c) ? "yes" : "no", SDL_GameControllerHasLED(c) ? "yes" : "no", SDL_GameControllerGetNumTouchpads(c) > 0 ? "yes" : "no",
      name && SDL_strstr(name, "Steam") ? " (Steam Input's virtual controller: Steam owns the real one, docs/controller.md)" : "");
    set_lightbar(c);
    note_kind();
    if (getenv("HG_PADAUTO") && atoi(getenv("HG_PADAUTO"))) pad_pressed(SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(c)));   /* tests: player 1 without a press */
}
static void close_pad(SDL_JoystickID id) {
    for (int i = 0; i < MAXPADS; i++) if (pads[i] && SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(pads[i])) == id) {
        if (pads[i] == pad) { pad = 0; L("host: player 1's gamepad disconnected (the next to press a button is player 1)\n"); }
        else L("host: gamepad disconnected\n");
        SDL_GameControllerClose(pads[i]); pads[i] = 0;
        note_kind();
        return;
    }
}
/* a button (or trigger) pressed on controller `id`: the first one becomes player 1 */
static void pad_pressed(SDL_JoystickID id) {
    if (pad) return;
    SDL_GameController *c = SDL_GameControllerFromInstanceID(id);
    if (!c) return;
    pad = c;
    SDL_GameControllerSetPlayerIndex(c, 0);
    L("host: \"%s\" is player 1\n", SDL_GameControllerName(c) ? SDL_GameControllerName(c) : "?");
    note_kind();
}
/* the game's motors on player 1's controller: sent when they change, and again every 100 ms while running (each send
   lasts 250 ms, so the motors stop by themselves if the game stops reporting, e.g. the window closes or it hangs) */
static void ui_rumble(int stop) {
    static SDL_GameController *to; static Uint16 lo_s, hi_s; static Uint32 at;
    static int log = -1; if (log < 0) log = getenv("HG_RUMBLELOG") && atoi(getenv("HG_RUMBLELOG"));
    const Uint32 now = SDL_GetTicks();
    Uint16 lo = 0, hi = 0;
    if (!stop && pad && now - (Uint32)SDL_AtomicGet(&rumble_tick) < 250) {
        const int r = SDL_AtomicGet(&rumble_req), pct = settings_vibration_percent(ui_vib);
        lo = (Uint16)((unsigned)(r & 0xFF) * 257u * (unsigned)pct / 100u);   /* the big motor: the low-frequency (left) one */
        hi = (r >> 8 & 1) ? (Uint16)(65535u * (unsigned)pct / 100u) : 0;     /* the small one, on or off: the right one */
    }
    if (pad != to) { to = pad; lo_s = hi_s = 0; at = 0; }
    if (!pad) return;
    if (lo != lo_s || hi != hi_s || ((lo || hi) && now - at >= 100)) {
        if (log && (lo != lo_s || hi != hi_s)) L("host rumble: low %u high %u\n", lo, hi);
        SDL_GameControllerRumble(pad, lo, hi, lo || hi ? 250 : 0);
        lo_s = lo; hi_s = hi; at = now;
    }
}
void host_rumble(int small, int big) {
    if (active != 1) return;
    SDL_AtomicSet(&rumble_req, (small ? 0x100 : 0) | (big & 0xFF));
    SDL_AtomicSet(&rumble_tick, (int)SDL_GetTicks());
}
/* "auto" resolution: the height of the display the window is on */
static void note_display(void) {
    SDL_DisplayMode m; const int d = SDL_GetWindowDisplayIndex(win);
    if (d >= 0 && !SDL_GetDesktopDisplayMode(d, &m)) settings_ui_auto_scale(m.h);
}
static void sdl_renderer(void) {
    ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED);
    if (!ren) ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
}
static int host_init(void) {   /* on the UI thread */
    SDL_SetHint("SDL_WINDOWS_DPI_AWARENESS", "permonitorv2");   /* a sharp window on scaled displays, not one Windows stretches */
    /* DualSense / DualShock 4 over Bluetooth: rumble and the light bar need the extended reports (an environment
       variable of the same name still wins; once on, other programs see the controller differently until it is
       switched off and on again) */
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS5_RUMBLE, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS4_RUMBLE, "1");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0) { L("host: SDL_Init failed: %s (running headless)\n", SDL_GetError()); return 0; }
    {   /* community controller mappings next to the executable (optional) */
        char *base = SDL_GetBasePath(), p[1100];
        snprintf(p, sizeof p, "%sgamecontrollerdb.txt", base ? base : "");
        SDL_free(base);
        FILE *f = fopen(p, "rb");
        if (f) { fclose(f); const int n = SDL_GameControllerAddMappingsFromFile(p); L("host: %s: %d controller mappings%s%s\n", p, n, n < 0 ? ": " : "", n < 0 ? SDL_GetError() : ""); }
    }
    const int scale = settings()->window_scale;   /* HG_SCALE, window_scale */
    Uint32 wflags = SDL_WINDOW_RESIZABLE;
#if defined HAVE_VULKAN && !defined _WIN32
    if (settings()->renderer) wflags |= SDL_WINDOW_VULKAN;   /* Linux: SDL makes the GPU renderer's surface (ui_mksurf) */
#endif
    const int ww = settings()->wide ? 448 * scale * 16 / 9 : 640 * scale;   /* widescreen: a 16:9 window */
    win = SDL_CreateWindow("Haunting Ground Recomp", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, ww, 448 * scale, wflags);
    if (!win) { L("host: no window: %s (running headless)\n", SDL_GetError()); SDL_Quit(); return 0; }
#ifdef HAVE_VULKAN   /* the GPU renderer presents itself once it is set up (at the first primitive): no SDL renderer on the window */
    const char *pp = getenv("HG_PRESENT");
    if (settings()->renderer && !getenv("HG_GSVKCHECK") && !(pp && !strcmp(pp, "sdl"))) vkmode = 1;
#endif
    if (!vkmode) sdl_renderer();
    ui_lightbar = settings()->lightbar;
    for (int i = 0; i < SDL_NumJoysticks(); i++) open_pad(i);
    note_display();
    L("host: SDL window open (%s, %d joystick(s))\n", vkmode ? "to be presented by the GPU renderer" : ren ? "SDL renderer" : "no renderer", SDL_NumJoysticks());
    return 1;
}
static SDL_AudioDeviceID adev; static int audio_state;   /* 0 = not tried, 1 = open, -1 = unavailable */
static volatile int ar_n;   /* audio frames queued (host_audio_push), read by the overlay */

/* F3 (or HG_OVERLAY=1): performance readout over the picture, updated every second: emulated speed (100 % = 59.94
   VBlanks a second; with frame interpolation also SKIP: in-between pictures left out in that second because the GPU
   was short of time, gs.c vi_adapt_tick), host work per VBlank (pacing sleep left out) and the worst VBlank, and the
   audio queue. Drawn
   into the frame with a 3x5 pixel font at twice its size. */
static const struct { char c; uint8_t r[5]; } glyphs[] = {
    {'0',{7,5,5,5,7}},{'1',{2,6,2,2,7}},{'2',{7,1,7,4,7}},{'3',{7,1,7,1,7}},{'4',{5,5,7,1,1}},{'5',{7,4,7,1,7}},
    {'6',{7,4,7,5,7}},{'7',{7,1,1,1,1}},{'8',{7,5,7,5,7}},{'9',{7,5,7,1,7}},{'.',{0,0,0,0,2}},{'%',{5,1,2,4,5}},
    {'A',{2,5,7,5,5}},{'D',{6,5,5,5,6}},{'E',{7,4,6,4,7}},{'I',{7,2,2,2,7}},{'K',{5,5,6,5,5}},{'M',{5,7,7,5,5}},
    {'O',{7,5,5,5,7}},{'P',{6,5,6,4,4}},{'R',{6,5,6,5,5}},{'S',{7,4,7,1,7}},{'U',{5,5,5,5,7}},{'W',{5,5,7,7,5}},
    {'X',{5,5,2,5,5}},{'-',{0,0,7,0,0}},{'+',{0,2,7,2,0}},{'F',{7,4,6,4,4}},{'H',{5,5,7,5,5}},{'N',{6,5,5,5,5}},{'V',{5,5,5,5,2}},
    {'B',{6,5,6,5,6}},{'C',{7,4,4,4,7}},{'G',{7,4,5,5,7}},{'J',{1,1,1,5,7}},{'L',{4,4,4,4,7}},{'Q',{7,5,5,7,1}},{'T',{7,2,2,2,2}},
    {'Y',{5,5,2,2,2}},{'Z',{7,1,2,4,7}},{':',{0,2,0,2,0}},{'/',{1,1,2,4,4}},{'=',{0,7,0,7,0}},
};
static void ov_textc(uint32_t *fb, int w, int h, int x, int y, const char *s, uint32_t col) {
    for (; *s; s++, x += 8) {
        const uint8_t *g = 0;
        for (size_t i = 0; i < sizeof glyphs / sizeof glyphs[0]; i++) if (glyphs[i].c == *s) g = glyphs[i].r;
        if (!g) continue;
        for (int r = 0; r < 10; r++) for (int c = 0; c < 6; c++)
            if ((g[r / 2] >> (2 - c / 2)) & 1 && x + c < w && y + r < h) fb[(y + r) * w + x + c] = col;
    }
}
static void ov_text(uint32_t *fb, int w, int h, int x, int y, const char *s) { ov_textc(fb, w, h, x, y, s, 0x00FFFFFF); }
static int mark_n; static double mark_t;   /* F9 marks (UI thread) */
static void mark_now(void);
static int overlay = -1; static char ov_line[4][40]; static char ov_smooth[40], ov_av[16];   /* written on the game thread (the last two: the UI thread, sm_tick), painted on the UI thread */
static void ov_stats(void) {
    static double t0, idle0, worst, last, last_idle; static uint64_t n; char (*line)[40] = ov_line;
    const double now = host_now();
    if (last) { const double work = (now - last) - (host_idle - last_idle); if (work > worst) worst = work; }
    last = now; last_idle = host_idle; n++;
    if (!t0) { t0 = now; idle0 = host_idle; }
    if (now - t0 >= 1.0) {
        const double busy = (now - t0) - (host_idle - idle0);
        snprintf(line[0], sizeof line[0], "SPEED %.0f%%", n / (now - t0) / 59.94 * 100);
#ifdef HAVE_VULKAN
        {   /* adaptive interpolation: in-between pictures left out in the last second */
            extern uint64_t gs_vi_skipped(void); extern int gs_interp_now(void); static uint64_t s0;
            const uint64_t s = gs_vi_skipped();
            if (gs_interp_now()) snprintf(line[0], sizeof line[0], "SPEED %.0f%% SKIP %llu", n / (now - t0) / 59.94 * 100, (unsigned long long)(s - s0));
            s0 = s;
        }
#endif
        snprintf(line[1], sizeof line[1], "WORK %.1f MS MAX %.1f", busy / n * 1000, worst * 1000);
        snprintf(line[2], sizeof line[2], "AUDIO %.0f MS", adev ? ar_n / 48.0 : 0.0);
#ifdef HAVE_VULKAN
        {   /* the full overlay: GPU ms per VBlank and the GS thread's waits for it, in that second */
            static double g0, w0; uint64_t ns, nd; double g, wt; gsvk_stats(&ns, &nd, &g, &wt);
            if (g > 0) snprintf(line[3], sizeof line[3], "GPU %.1f MS GS WAIT %.1f", (g - g0) / n, (wt - w0) / n); else line[3][0] = 0;
            g0 = g; w0 = wt;
        }
#endif
        t0 = now; idle0 = host_idle; n = 0; worst = 0;
    }
}
static void ov_paint(uint32_t *fb, int w, int h) {
    if (overlay <= 0) return;
    const int s = w >= 1280 ? w / 640 : 1;   /* about the same size on upscaled pictures */
    for (int y = 4 * s; y < 58 * s && y < h; y++) for (int x = 4 * s; x < 180 * s && x < w; x++) fb[y * w + x] = (fb[y * w + x] >> 2) & 0x003F3F3F;   /* dim box */
    char l2[64]; snprintf(l2, sizeof l2, "%s%s", ov_line[2], ov_av);
    const char *ln[4] = { ov_line[0], ov_line[1], l2, ov_smooth };
    if (s == 1) { for (int i = 0; i < 4; i++) ov_text(fb, w, h, 8, 8 + 12 * i, ln[i]); return; }
    static uint32_t small[58 * 180];   /* drawn at 1x, then scaled */
    for (int i = 0; i < 58 * 180; i++) small[i] = 0;
    for (int i = 0; i < 4; i++) ov_text(small, 180, 58, 8, 8 + 12 * i, ln[i]);
    for (int y = 0; y < 58 * s && y < h; y++) for (int x = 0; x < 180 * s && x < w; x++) if (small[(y / s) * 180 + x / s]) fb[y * w + x] = 0x00FFFFFF;
}
static uint8_t axis8(Sint16 v) { int a = (v + 32768) >> 8; return a < 0 ? 0 : a > 255 ? 255 : a; }
static void ui_input(void) {
    uint16_t held = 0; uint8_t an[4] = { 0x80, 0x80, 0x80, 0x80 };   /* rx ry lx ly */
    int nk = 0; const Uint8 *ks = SDL_GetKeyboardState(&nk);
    for (int b = 0; b < PB_BUTTONS; b++) if (kbmap[b] > 0 && kbmap[b] < nk && ks[kbmap[b]]) held |= 1u << b;
#define KEY(pb) (kbmap[pb] > 0 && kbmap[pb] < nk && ks[kbmap[pb]])
    if (KEY(PB_LS_LEFT)) an[2] = 0x00;
    if (KEY(PB_LS_RIGHT)) an[2] = 0xFF;
    if (KEY(PB_LS_UP)) an[3] = 0x00;
    if (KEY(PB_LS_DOWN)) an[3] = 0xFF;
#undef KEY
    if (held || an[2] != 0x80 || an[3] != 0x80) settings_set_device_live(DEV_KEYBOARD);   /* (the keys stand in for the glyphs in the text) */
    if (pad) {
        int touch_mapped = 0;
        for (int b = 0; b < PB_BUTTONS; b++) {
            const int in = padmap[b];
            if (in == SDL_CONTROLLER_BUTTON_TOUCHPAD) touch_mapped = 1;
            if (in == PAD_LT || in == PAD_RT) {
                if (SDL_GameControllerGetAxis(pad, in == PAD_LT ? SDL_CONTROLLER_AXIS_TRIGGERLEFT : SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 16000) held |= 1u << b;
            } else if (in >= 0 && SDL_GameControllerGetButton(pad, (SDL_GameControllerButton)in)) held |= 1u << b;
        }
        if (ui_touchsel && !touch_mapped && SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_TOUCHPAD)) held |= 1u << B_SELECT;
        /* the sticks: centred inside the dead zone (a circle), the raw position outside it */
        const long dz = (long)ui_deadzone * 32767 / 100;
        Sint16 lx = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTX), ly = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTY);
        Sint16 rx = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_RIGHTX), ry = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_RIGHTY);
        if ((long)lx * lx + (long)ly * ly > dz * dz) { an[2] = axis8(lx); an[3] = axis8(ly); }
        if ((long)rx * rx + (long)ry * ry > dz * dz) { an[0] = axis8(rx); an[1] = axis8(ry); }
        if (held || an[0] != 0x80 || an[1] != 0x80 || an[2] != 0x80 || an[3] != 0x80) settings_set_device_live(DEV_PAD);   /* (a key and a button together: the controller) */
    }
    SDL_AtomicSet(&in_held, held); SDL_AtomicSet(&in_an, (int)((uint32_t)an[0] | (uint32_t)an[1] << 8 | (uint32_t)an[2] << 16 | (uint32_t)an[3] << 24));
}
/* a key or button for the mapping page that waits for one (settings_capture_target): 1 if taken */
static int capture_event(const SDL_Event *e) {
    static int target = -1;
    const int t = settings_ui_capture_target();   /* (taken: put back below unless this event answers it) */
    if (t >= 0) target = t;
    if (target < 0) return 0;
    const int kind = target >> 8, pb = target & 0xFF;
    char key[32], val[64]; key[0] = 0;
    if (e->type == SDL_KEYDOWN && !e->key.repeat) {
        const SDL_Scancode sc = e->key.keysym.scancode;
        if (sc == SDL_SCANCODE_ESCAPE) { snprintf(key, sizeof key, "capture"); snprintf(val, sizeof val, "cancel"); }
        else if (kind == 0 && sc != SDL_SCANCODE_F3 && sc != SDL_SCANCODE_F11) {
            snprintf(key, sizeof key, "kb.%s", settings_button_key(pb)); snprintf(val, sizeof val, "%s", settings_key_name((int)sc));
        }
    } else if (kind == 1 && e->type == SDL_CONTROLLERBUTTONDOWN) {
        snprintf(key, sizeof key, "pad.%s", settings_button_key(pb)); snprintf(val, sizeof val, "%s", settings_pad_ini(e->cbutton.button));
    } else if (kind == 1 && e->type == SDL_CONTROLLERAXISMOTION && e->caxis.value > 16000 &&
               (e->caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT || e->caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT)) {
        snprintf(key, sizeof key, "pad.%s", settings_button_key(pb));
        snprintf(val, sizeof val, "%s", settings_pad_ini(e->caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT ? PAD_LT : PAD_RT));
    }
    if (!key[0]) return 0;
    settings_host_request(key, val); target = -1;
    return 1;
}
static void ui_events(void) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_CONTROLLERBUTTONDOWN) pad_pressed(e.cbutton.which);
        if (e.type == SDL_CONTROLLERAXISMOTION && e.caxis.value > 16000 &&
            (e.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT || e.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT)) pad_pressed(e.caxis.which);
        if (capture_event(&e)) continue;   /* (Esc then cancels the capture instead of quitting) */
        if (e.type == SDL_QUIT || (e.type == SDL_KEYDOWN && e.key.keysym.scancode == SDL_SCANCODE_ESCAPE)) SDL_AtomicSet(&ui_quit, 1);
        if (e.type == SDL_KEYDOWN && e.key.keysym.scancode == SDL_SCANCODE_F3 && !e.key.repeat) overlay = overlay > 0 ? (overlay + 1) % 3 : 1;   /* off, compact, full (GPU renderer) */
        if (e.type == SDL_KEYDOWN && e.key.keysym.scancode == SDL_SCANCODE_F9 && !e.key.repeat) {   /* a mark: the game frame and the picture on screen now */
            mark_now();
        }
        if (e.type == SDL_KEYDOWN && e.key.keysym.scancode == SDL_SCANCODE_F11 && !e.key.repeat) {
            /* the setting (recorded, so a replay shows the same value in the menu); a replay flips the window only */
            if (settings_replaying()) fs_flip ^= 1;
            else settings_host_request("fullscreen", settings()->fullscreen ? "0" : "1");
        }
        if (e.type == SDL_CONTROLLERDEVICEADDED) open_pad(e.cdevice.which);
        if (e.type == SDL_CONTROLLERDEVICEREMOVED) close_pad(e.cdevice.which);
#if SDL_VERSION_ATLEAST(2, 0, 18)
        if (e.type == SDL_WINDOWEVENT && e.window.event == SDL_WINDOWEVENT_DISPLAY_CHANGED) note_display();
#endif
    }
}
/* settings that changed since the last look: mappings, full screen, vsync */
static void ui_settings(void) {
    const uint32_t g = settings_generation();
    static int fs_now = -1;
    if (g != ui_gen) {
        ui_gen = g;
        const Settings *st = settings();
        for (int i = 0; i < PB_N; i++) kbmap[i] = st->kb[i];
        for (int i = 0; i < PB_BUTTONS; i++) padmap[i] = st->pad[i];
        ui_vib = st->vibration; ui_deadzone = st->deadzone; ui_touchsel = st->touchpad_select;
        if (st->lightbar != ui_lightbar) { ui_lightbar = st->lightbar; for (int i = 0; i < MAXPADS; i++) set_lightbar(pads[i]); }
#ifdef HAVE_VULKAN
        if (st->vsync != ui_vsync) { ui_vsync = st->vsync; gsvk_present_mode(ui_vsync); }
#endif
    }
    const int fs = settings()->fullscreen ^ fs_flip;
    if (fs != fs_now) {
        fs_now = fs; SDL_SetWindowFullscreen(win, fs ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
#ifdef HAVE_VULKAN
        gsvk_present_fullscreen(fs);   /* (HG_FSE=1: full-screen exclusive mode while full screen) */
#endif
    }
    static int wide_now = -1;   /* widescreen switched in a window: the window takes the new shape at its height */
    if (settings()->wide != wide_now) {
        if (wide_now >= 0 && !fs) { int w, h; SDL_GetWindowSize(win, &w, &h); SDL_SetWindowSize(win, settings()->wide ? h * 16 / 9 : h * 640 / 448, h); }
        wide_now = settings()->wide;
    }
}
static const uint32_t *ov_full(int *w, int *h);
/* the F3 overlay for the GPU path (176 x 54, or the full one), or 0 when it is off */
static const uint32_t *ov_build(int *w, int *h) {
    static uint32_t ov[176 * 54];   /* 4 pixels from the picture's corner */
    const int marked = mark_n && host_now() - mark_t < 2;
    char mk[24]; if (marked) snprintf(mk, sizeof mk, "MARK %d", mark_n);
    if (overlay <= 0 && !marked) return 0;
    if (overlay == 2) { const uint32_t *o = ov_full(w, h); if (marked) ov_textc((uint32_t *)o, *w, *h, 200, 4, mk, 0x0000E0FF); return o; }
    *w = 176; *h = 54;
    if (overlay <= 0) { for (int i = 0; i < 176 * 54; i++) ov[i] = 0x00181818; ov_textc(ov, 176, 54, 4, 4, mk, 0x0000E0FF); return ov; }
    for (int i = 0; i < 176 * 54; i++) ov[i] = 0x00181818;
    char l2[64]; snprintf(l2, sizeof l2, "%s%s", ov_line[2], ov_av);
    for (int i = 0; i < 4; i++) ov_text(ov, 176, 54, 4, 4 + 12 * i, i == 2 ? l2 : i == 3 ? ov_smooth : ov_line[i]);
    return ov;
}
static uint64_t pres_tag;   /* the picture the window shows (gs_display_tag of the last one taken) */
#if defined HAVE_VULKAN && !defined _WIN32
static int ui_mksurf(void *inst, void *surf) {   /* gsvk_present_init_with: the window's surface on Wayland or X11 */
    if (SDL_Vulkan_CreateSurface(win, (VkInstance)inst, (VkSurfaceKHR *)surf)) return 1;
    L("host: SDL_Vulkan_CreateSurface: %s\n", SDL_GetError()); return 0;
}
static void ui_size(int *w, int *h) { SDL_Vulkan_GetDrawableSize(win, w, h); }   /* the swapchain's size where the surface leaves it to us (Wayland) */
#endif
static uint64_t ui_present(void) {   /* the timer's way: the next picture (or the last again); the present's id (GPU path) */
#ifdef HAVE_VULKAN
    if (vkmode == 1) {   /* the GPU renderer: set up yet? */
        int r = gsvk_ready();
        if (r > 0) {
            r = -1;
#ifdef _WIN32
            SDL_SysWMinfo wi; SDL_VERSION(&wi.version);
            if (SDL_GetWindowWMInfo(win, &wi) && gsvk_present_init(wi.info.win.hinstance, wi.info.win.window)) { vkmode = 2; r = 1; }
#else
            if (gsvk_present_init_with(ui_mksurf, ui_size)) { vkmode = 2; r = 1; }
#endif
        }
        if (r < 0) { vkmode = 0; sdl_renderer(); L("host: presenting with SDL\n"); }
        else if (vkmode == 1) { int s; uint64_t id; while (gs_display_gpu(&s, &id)) gsvk_pic_release(s); return 0; }   /* (drops the early pictures) */
    }
    if (vkmode == 2) {
        int s; uint64_t id = 0;
        if (!gs_display_gpu(&s, &id)) s = -1;   /* none new: the last one again */
        else pres_tag = gs_display_tag();
        int ow = 0, oh = 0; const uint32_t *ov = ov_build(&ow, &oh);
        return gsvk_present(s, id, ov, ow, oh, 0);
    }
#endif
    int w, h;
    if (!frame && !(frame = malloc((size_t)4096 * 4096 * 4))) return 0;
    if (!ren) return 0;
    if (!gs_display(frame, 4096, 4096, &w, &h)) { SDL_SetRenderDrawColor(ren, 0, 0, 0, 255); SDL_RenderClear(ren); SDL_RenderPresent(ren); return 0; }
    pres_tag = gs_display_tag();
    if (!tex || w != tex_w || h != tex_h) {
        if (tex) SDL_DestroyTexture(tex);
        tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_XBGR8888, SDL_TEXTUREACCESS_STREAMING, w, h); tex_w = w; tex_h = h;
    }
    {   /* 4:3 output, the PS2 picture stretched to the TV like the console did; widescreen: the display's shape */
        int ow, oh; if (!SDL_GetRendererOutputSize(ren, &ow, &oh)) settings_ui_window_size(ow, oh);
        const int lw = (int)(448 * settings_display_aspect() + 0.5); static int last_lw;
        if (lw != last_lw) { SDL_RenderSetLogicalSize(ren, lw, 448); last_lw = lw; }
    }
    ov_paint(frame, w, h);
    SDL_UpdateTexture(tex, 0, frame, w * 4);
    SDL_RenderClear(ren); SDL_RenderCopy(ren, tex, 0, 0); SDL_RenderPresent(ren);
    return 0;
}

/* ================================================================== pacing (docs/frame-timing.md, "Pacing the window")
   Two ways to time the window, by the vsync setting:
   - Display-locked (vsync "on", the default with the GPU renderer): the swapchain is FIFO and the UI thread presents
     at every refresh of the display, each time waiting (VK_KHR_present_wait) until the present has reached the
     screen, so the display is the clock. Its refresh period is measured from those times. When the refresh rate is
     within 0.5 % of a whole multiple N of 59.94 Hz (60, 120, 180, 240 Hz...), a new picture is taken every N
     refreshes (N/2 at 120 fps) and repeated in between, so every picture is held exactly N refreshes; and the game
     thread's emulated VBlanks follow those ticks (host_vblank), so emulation runs at refresh / N (at 120.00 Hz: 60.00
     VBlanks a second instead of 59.94, 0.1 % faster; the sound's resampler absorbs it, host_audio_push). A slow
     controller keeps the picture queue short: a picture not ready when its refresh comes (it is then repeated) makes
     the game run a quarter VBlank earlier from then on; two ready at every take for 10 s, a quarter VBlank later.
     Other rates (144, 165 Hz) have no whole multiple: the game keeps its own 59.94 Hz clock and each refresh shows
     the picture that clock says is due, so the holds are as even as the rate allows (144 Hz: 2,3,2,3,2...).
     If the present times turn irregular (more than 10 % of the intervals off by a quarter), the lock is let go the
     same way until they settle.
   - Timer (vsync "mailbox", the exact 59.94 Hz, or "off"): the UI thread presents on its own 59.94 Hz clock (mailbox
     or immediate) and the game thread keeps emulated time at 59.94 Hz of the host clock. On a 120 Hz screen a
     picture then lands on 1 or 3 refreshes whenever the two clocks' jitter crosses a refresh.
   HG_PRESENTLOG=S[:N]: N presents (default 600) from S seconds on are logged ("PRESENT" after the call, and with
   VK_KHR_present_wait "SHOWN": the time it reached the screen, new or repeated, and for a new picture the time since
   the game thread began its VBlank, "lat"); tools/verify/pacing.py makes the histograms. */
static SDL_SpinLock lk_lock;
static int lk_on; static uint64_t lk_n; static double lk_t, lk_per = 1 / 59.94, lk_phase;   /* under lk_lock: ticks, the last one's time, seconds per tick, phase (ticks) */
static volatile uint64_t vb_last;   /* the game thread's VBlank */
static double vb_wall[1024];   /* host_now() when the game thread began each VBlank (the picture delay in the log) */
static int plog = -1; static double plog_from, t_ui0;
static void lock_set(int on, uint64_t n, double t, double per, double ph) {
    SDL_AtomicLock(&lk_lock); lk_on = on; if (on) { lk_n = n; lk_t = t; lk_per = per; lk_phase = ph; } SDL_AtomicUnlock(&lk_lock);
}
static void plog_line(const char *what, double t, uint64_t tag, const char *kind) {   /* HG_PRESENTLOG */
    if (plog <= 0 || t - t_ui0 < plog_from) return;
    static int only = -1; if (only < 0) only = getenv("HG_PRESENTLOG_NEW") && atoi(getenv("HG_PRESENTLOG_NEW"));
    if (only && (strcmp(what, "SCAN") || !kind || strcmp(kind, "new"))) return;   /* (long runs: the first scanout of each picture only) */
    plog--;
    char lat[32] = "";
    if (kind && !strcmp(kind, "new") && vb_wall[(tag >> 2) & 1023] > 0) snprintf(lat, sizeof lat, " lat %.1f", (t - vb_wall[(tag >> 2) & 1023]) * 1000);
    L("%s %.3f ms vb %llu.%d%s%s%s%s\n", what, t * 1000.0, (unsigned long long)(tag >> 2), (int)(tag >> 1 & 1), tag & 1 ? " i" : "", kind ? " " : "", kind ? kind : "", lat);
}
/* ================================================================== smoothness and A/V sync (what the player sees)
   Every picture's hold is measured where it ends: the time its first scanout (VK_EXT_present_timing, first pixel out;
   without it the present-wait time) to the next picture's, in refreshes. A smooth 60 fps on a 120 Hz display holds
   every picture exactly N = 2 refreshes. Counted per second, logged every 10 s ("smooth:"), shown on the F3 overlay
   (the last 10 s), and kept per minute and as a list of hitches (a hold other than N, a picture not ready at its tick,
   an in-between picture left out, a sound underrun or drop) with the VBlank, the time, the stage and whether a movie
   was playing; the table and the list are written at exit and read by tools/verify/sessionreport.py.
   A/V: the sound of emulated moment VBlank v is 48 kHz sample vb_samp[v] (the SPU2's count when the game thread
   began v); it reaches the speakers at the ring's play position for it (host_audio_push keeps the latest
   sample index -> time) plus what SDL took from the ring and has not played yet plus the device's own latency
   (HG_AUDIO_LAT_MS, by default one SDL buffer; not measurable through SDL, an estimate). The picture of VBlank v
   reaches the screen at its first scanout. offset = picture time - sound time: + the picture is late. */
extern uint64_t spu2_sample_count(void); extern uint64_t ipu_bdec_count(void); extern char movie_now[64];
extern uint64_t gs_vi_skipped(void);
static uint64_t vb_samp[1024], vb_samp_vb[1024];   /* game thread: the SPU2's sample count at each VBlank */
static volatile uint32_t sm_stage; static volatile int sm_movie;   /* game thread: stage number; a movie playing */
static SDL_SpinLock av_lock; static uint64_t av_in_end; static double av_t_end, av_step = 1; static int av_valid;   /* under av_lock */
static double av_dev = -1;   /* the device's latency beyond the frames SDL holds (s) */
static volatile double av_hw = 0, av_user = 0;   /* the sound card's own queue (Linux, measured: av_probe_hw), and audio_offset (s) */
static volatile double cb_t; static volatile int cb_frames;   /* audio thread: the last callback's time and size */
static uint64_t aud_in;      /* game thread: frames given to host_audio_push so far (= the SPU2's samples pushed) */
static volatile uint64_t ar_drops; static uint64_t ar_underruns_total(void);
/* A/V alignment (HG_AV, default 1; 0: the fixed 120 ms cushion and the 1024-frame device buffer of before). The sound's
   delay is the ring's level, which the resampler holds at a target: the larger of a floor against underruns (35 ms,
   HG_AUDIO_FLOOR_MS; 20 ms more after each underrun the host caused, none for one while the game itself ran slow,
   e.g. a stall at a loading point; 10 ms less after each 10 s without one; at most 150 ms) and what the A/V control
   asks. The picture's delay is the game's phase against the display's ticks (display-locked: ui_refresh's ph, the
   game earlier -> its pictures wait longer). Every 2 s the mean offset, corrected for where the ring is heading
   (level - target), decides: the sound late by more than 12 ms -> the ring's target down if above the floor, else
   the game a quarter VBlank earlier (pictures later); the picture late by more than 12 ms -> the game a quarter
   VBlank later if no picture was missing for 20 s, else the ring's target up. */
static int av_on = -1;
static volatile int ar_floor = -1, ar_avwant = 0;   /* frames: the underrun floor (game thread), the A/V control's wish (UI thread) */
static volatile double ar_lvl_ms, ar_tgt_ms;   /* game thread: the ring's smoothed level and target, ms */
static double av_c_sum; static int av_c_n;     /* UI thread: offsets (corrected) since the last control step */
static int ar_floor0(void) { static int f = -1; if (f < 0) { const char *e = getenv("HG_AUDIO_FLOOR_MS"); const int ms = e ? atoi(e) : 35; f = 48 * (ms < 10 ? 10 : ms > 150 ? 150 : ms); } return f; }   /* frames */
static int ar_target(void) { if (!av_on) return 48000 * 12 / 100; if (ar_floor < 0) ar_floor = ar_floor0(); const int f = ar_floor, w = ar_avwant; return w > f ? w : f; }
static void av_init(void) { if (av_on < 0) { const char *e = getenv("HG_AV"); av_on = !(e && !atoi(e)); } if (ar_floor < 0) ar_floor = ar_floor0(); }
/* the A/V control's audio side: lower (dir < 0) or raise the ring's target by about ms; 0 if it is at its limit */
static int av_audio_move(double ms, int dir) {
    const int f = ar_floor, cur = ar_target(), step = (int)((ms > 20 ? 20 : ms) * 48);
    if (dir < 0) { if (cur <= f + 48) return 0; int w = cur - step; if (w < f) w = f; ar_avwant = w; return 1; }
    if (cur >= 48000 * 150 / 1000) return 0;
    int w = cur + step; if (w > 48000 * 150 / 1000) w = 48000 * 150 / 1000; ar_avwant = w; return 1;
}
/* every 2 s on the UI thread: the mean corrected offset (ms) of the real pictures shown, or 0 when too few */
static int av_ctl_take(double *o) {
    if (av_c_n < 30) return 0;
    *o = av_c_sum / av_c_n; av_c_sum = 0; av_c_n = 0; return 1;
}
typedef struct { uint64_t pics, h_ok, h_short, h_long1, h_long2, notready, skipped, underruns, drops, av_n; double av_sum, av_min, av_max; } SmStat;
static void sm_add(SmStat *a, const SmStat *b) {
    if (b->av_n) { if (!a->av_n || b->av_min < a->av_min) a->av_min = b->av_min; if (!a->av_n || b->av_max > a->av_max) a->av_max = b->av_max; }
    a->pics += b->pics; a->h_ok += b->h_ok; a->h_short += b->h_short; a->h_long1 += b->h_long1; a->h_long2 += b->h_long2; a->notready += b->notready;
    a->skipped += b->skipped; a->underruns += b->underruns; a->drops += b->drops; a->av_n += b->av_n; a->av_sum += b->av_sum;
}
enum { SM_MIN = 720, SM_HITCH = 4096 };
static SmStat sm_sec[10], sm_cur, sm_tot, *sm_minute; static int sm_sec_i, sm_nmin; static double sm_t0, sm_sec_t;   /* UI thread */
static struct { double t; uint64_t vb; uint32_t stage; int movie; char kind; int val; } sm_hit[SM_HITCH]; static int sm_nhit; static uint64_t sm_hit_lost;
static int sm_expect = 2; static double sm_rp = 1.0 / 120;   /* UI thread: refreshes each picture should be held; the refresh period */
static double sm_last_t; static uint64_t sm_last_vb; static int sm_scan_seen = -1;   /* the last new picture's first scanout; SCAN times come in for this swapchain */
static uint64_t sm_und_seen, sm_drop_seen, sm_skip_seen; static double av_last_log;
static void sm_hitch(char kind, int val, uint64_t vb) {
    if (sm_nhit >= SM_HITCH) { sm_hit_lost++; return; }
    sm_hit[sm_nhit].t = host_now() - sm_t0; sm_hit[sm_nhit].vb = vb; sm_hit[sm_nhit].stage = sm_stage; sm_hit[sm_nhit].movie = sm_movie;
    sm_hit[sm_nhit].kind = kind; sm_hit[sm_nhit].val = val; sm_nhit++;
}
static double av_offset(uint64_t vb, double t_pic, double *vlat, double *alat) {   /* ms, or NAN */
    uint64_t in_end; double t_end, step; int ok;
    SDL_AtomicLock(&av_lock); in_end = av_in_end; t_end = av_t_end; step = av_step; ok = av_valid; SDL_AtomicUnlock(&av_lock);
    if (!ok || vb_samp_vb[vb & 1023] != vb) return NAN;
    const double t_snd = t_end - ((double)in_end - (double)vb_samp[vb & 1023]) * step / 48000.0;
    if (vlat) *vlat = (t_pic - vb_wall[vb & 1023]) * 1e3;
    if (alat) *alat = (t_snd - vb_wall[vb & 1023]) * 1e3;
    return (t_pic - t_snd) * 1e3;
}
static void sm_tick(int final);
static void mark_now(void) {
    mark_n++; mark_t = host_now();
    L("mark %d: F9 at game VBlank %llu, picture on screen from VBlank %llu%s, %.1f s into the session\n", mark_n, (unsigned long long)vb_last,
      (unsigned long long)(pres_tag >> 2), pres_tag & 1 ? " (in-between)" : "", sm_t0 > 0 ? mark_t - sm_t0 : 0.0);
}
/* The full overlay (F3 twice, or HG_OVERLAY=2; GPU renderer): the last OVG pictures' times on the screen (from the
   one before it to it, at first scanout where the driver reports it) as a graph, and the numbers of the last second
   (or 10 s). UI thread. */
enum { OVG = 128, OFW = 296, OFH = 172 };
static struct { float ms; uint8_t h, real; } ovg[OVG]; static double ovg_t[OVG]; static int ovg_i, ovg_n;
static double ovl_sum; static int ovl_n; static char ov_fl[3][48];   /* latency (VBlank begun -> on the screen) of the second; the lines sm_tick writes */
static void ovg_add(double ms, int h, int real, double t) { ovg[ovg_i].ms = (float)ms; ovg[ovg_i].h = (uint8_t)(h > 255 ? 255 : h); ovg[ovg_i].real = (uint8_t)real; ovg_t[ovg_i] = t; ovg_i = (ovg_i + 1) % OVG; if (ovg_n < OVG) ovg_n++; }
/* a new picture reached the screen at t (src 1: first scanout, 0: present wait) */
static void sm_shown(uint64_t tag, double t, int src) {
    { const double now = host_now(); if (t < now - 2 || t > now + 1) return; }   /* (a time from another clock: not used) */
    if (src) sm_scan_seen = 1; else if (sm_scan_seen > 0) return;
    const uint64_t vb = tag >> 2;
    if (sm_last_t > 0 && t > sm_last_t) {
        const int h = (int)((t - sm_last_t) / sm_rp + 0.5);
        ovg_add((t - sm_last_t) * 1e3, h, !(tag & 1), t);
        sm_cur.pics++;
        if (h == sm_expect) sm_cur.h_ok++;
        else {
            if (h < sm_expect) sm_cur.h_short++; else if (h == sm_expect + 1) sm_cur.h_long1++; else sm_cur.h_long2++;
            sm_hitch('H', h, sm_last_vb);
        }
    }
    sm_last_t = t; sm_last_vb = vb;
    if (!(tag & 1)) {   /* a real picture (not an in-between one): A/V */
        double vl = 0, al = 0; const double o = av_offset(vb, t, &vl, &al);
        if (!isnan(o)) {
            ovl_sum += vl; ovl_n++;
            if (!sm_cur.av_n || o < sm_cur.av_min) sm_cur.av_min = o;
            if (!sm_cur.av_n || o > sm_cur.av_max) sm_cur.av_max = o;
            sm_cur.av_n++; sm_cur.av_sum += o;
            av_c_sum += o + (ar_lvl_ms - ar_tgt_ms); av_c_n++;   /* (where the offset goes once the ring is at its target) */
            if (sm_movie && t - av_last_log >= 1) { av_last_log = t;
                L("av: VB %llu movie \"%s\": picture %.0f ms after its VBlank began, sound %.0f ms: offset %+.0f ms (+: picture later)\n", (unsigned long long)vb,
                  movie_now[0] ? movie_now : "?", vl, al, o); }
        }
    }
}
/* Linux: the sound card's own queue, which the device latency above leaves out (PipeWire's ALSA sinks keep 1-2 periods
   in the hardware buffer: 11-21 ms on the user's HDMI output). Read 4 times a second from the one running playback
   stream in /proc/asound and averaged; not used when none or several run, nor with HG_AUDIO_LAT_MS. UI thread. */
static void av_probe_hw(double now) {
#ifdef __linux__
    static double t_next, avg = -1; static int off = -1;
    if (off < 0) off = getenv("HG_AUDIO_LAT_MS") != 0 || (getenv("HG_AUDIO_HWPROBE") && !atoi(getenv("HG_AUDIO_HWPROBE")));
    if (off || now < t_next || audio_state != 1) return;
    t_next = now + 0.25;
    int found = 0; long delay = 0, rate = 0;
    for (int c = 0; c < 8; c++) for (int d = 0; d < 16; d++) {
        char p[96]; snprintf(p, sizeof p, "/proc/asound/card%d/pcm%dp/sub0/status", c, d);
        FILE *f = fopen(p, "r"); if (!f) continue;
        char line[128]; int run = 0; long dl = -1;
        while (fgets(line, sizeof line, f)) { if (strstr(line, "RUNNING")) run = 1; if (!strncmp(line, "delay", 5)) dl = atol(strchr(line, ':') ? strchr(line, ':') + 1 : line); }
        fclose(f);
        if (!run || dl < 0) continue;
        found++; delay = dl;
        snprintf(p, sizeof p, "/proc/asound/card%d/pcm%dp/sub0/hw_params", c, d);
        if ((f = fopen(p, "r"))) { while (fgets(line, sizeof line, f)) if (!strncmp(line, "rate:", 5)) rate = atol(line + 5); fclose(f); }
    }
    if (found != 1 || rate <= 0) return;
    const double s = (double)delay / rate;
    avg = avg < 0 ? s : avg + (s - avg) * 0.05;
    av_hw = avg;
    static int nl; static double t_log; if (now - t_log > 60 && nl++ < 30) { t_log = now; L("host audio: the sound card holds %.1f ms (measured, added to the A/V latency)\n", avg * 1e3); }
#else
    (void)now;
#endif
}
static void sm_tick(int final) {   /* UI thread, every refresh: events polled, the seconds closed */
    const double now = host_now();
    av_probe_hw(now);
    if (!sm_t0) { sm_t0 = sm_sec_t = now; sm_und_seen = ar_underruns_total(); sm_drop_seen = ar_drops; sm_skip_seen = gs_vi_skipped(); }
    { const uint64_t u = ar_underruns_total(), d = ar_drops, s = gs_vi_skipped();
      if (u > sm_und_seen) { sm_cur.underruns += u - sm_und_seen; sm_hitch('U', (int)(u - sm_und_seen), vb_last); sm_und_seen = u; }
      if (d > sm_drop_seen) { sm_cur.drops += d - sm_drop_seen; sm_hitch('D', (int)(d - sm_drop_seen), vb_last); sm_drop_seen = d; }
      if (s > sm_skip_seen) { sm_cur.skipped += s - sm_skip_seen; sm_hitch('S', (int)(s - sm_skip_seen), vb_last); sm_skip_seen = s; } }
    if (now - sm_sec_t < 1 && !final) return;
    sm_sec_t = now;
    sm_sec[sm_sec_i] = sm_cur; sm_sec_i = (sm_sec_i + 1) % 10;
    sm_add(&sm_tot, &sm_cur);
    {   const int m = (int)((now - sm_t0) / 60);
        if (!sm_minute) sm_minute = calloc(SM_MIN, sizeof *sm_minute);
        if (sm_minute && m < SM_MIN) { sm_add(&sm_minute[m], &sm_cur); if (m + 1 > sm_nmin) sm_nmin = m + 1; } }
    memset(&sm_cur, 0, sizeof sm_cur);
    SmStat w; memset(&w, 0, sizeof w); for (int i = 0; i < 10; i++) if (!final || !sm_sec_i || i < sm_sec_i) sm_add(&w, &sm_sec[i]);   /* the last 10 s (at exit: since the last line) */
    snprintf(ov_smooth, sizeof ov_smooth, "%d-REFRESH %.1f%% NR %llu", sm_expect, w.pics ? 100.0 * w.h_ok / w.pics : 0.0, (unsigned long long)w.notready);
    if (w.av_n) snprintf(ov_av, sizeof ov_av, " AV %+.0f", w.av_sum / w.av_n); else ov_av[0] = 0;
    {   /* the full overlay: the last second's pictures */
        int np = 0, nr = 0; double mn = 1e9, mx = 0, sum = 0; const double tl = ovg_n ? ovg_t[(ovg_i + OVG - 1) % OVG] : 0;
        for (int i = 0; i < ovg_n; i++) { const int j = (ovg_i + OVG - 1 - i) % OVG; if (ovg_t[j] <= tl - 1) break;
            np++; nr += ovg[j].real; sum += ovg[j].ms; if (ovg[j].ms < mn) mn = ovg[j].ms; if (ovg[j].ms > mx) mx = ovg[j].ms; }
        snprintf(ov_fl[0], sizeof ov_fl[0], "FPS %.1f GAME %.1f DISPLAY %.0f HZ", sum > 0 ? np * 1e3 / sum : 0.0, sum > 0 ? nr * 1e3 / sum : 0.0, 1 / sm_rp);   /* (per second of the pictures' own times) */
        if (np) snprintf(ov_fl[1], sizeof ov_fl[1], "FRAME %.1f MS MIN %.1f MAX %.1f", sum / np, mn, mx); else snprintf(ov_fl[1], sizeof ov_fl[1], "FRAME -");
        snprintf(ov_fl[2], sizeof ov_fl[2], "VBLANK TO SCREEN %.0f MS", ovl_n ? ovl_sum / ovl_n : 0.0); ovl_sum = 0; ovl_n = 0;
        static char ov_10[2][48];
        snprintf(ov_10[0], sizeof ov_10[0], "10 S: %.1f%% EXACT, NOT READY %llu", w.pics ? 100.0 * w.h_ok / w.pics : 0.0, (unsigned long long)w.notready);
        snprintf(ov_10[1], sizeof ov_10[1], "SHORT %llu LONG %llu LONGER %llu SKIP %llu", (unsigned long long)w.h_short, (unsigned long long)w.h_long1, (unsigned long long)w.h_long2, (unsigned long long)w.skipped);
        static char ov_snd[80]; snprintf(ov_snd, sizeof ov_snd, "%s%s UNDER %llu", ov_line[2], ov_av, (unsigned long long)w.underruns);
        extern const char *ov_full_lines[8]; ov_full_lines[0] = ov_fl[0]; ov_full_lines[1] = ov_fl[1]; ov_full_lines[2] = ov_10[0]; ov_full_lines[3] = ov_10[1];
        ov_full_lines[5] = ov_snd; ov_full_lines[7] = ov_fl[2];
    }
    if (sm_sec_i == 0 || final) {   /* every 10 s (and the rest at exit) */
        L("smooth: %.0f s: %llu pictures, held %d refreshes %.1f %% (%llu), %d: %llu, %d: %llu, more: %llu; in-between left out %llu; not ready %llu; sound underruns %llu, drops %llu; "
          "A/V %s%+.0f ms (%+.0f..%+.0f, + picture later); stage %x%s\n", now - sm_t0, (unsigned long long)w.pics, sm_expect, w.pics ? 100.0 * w.h_ok / w.pics : 0.0, (unsigned long long)w.h_ok,
          sm_expect - 1, (unsigned long long)w.h_short, sm_expect + 1, (unsigned long long)w.h_long1, (unsigned long long)w.h_long2, (unsigned long long)w.skipped,
          (unsigned long long)w.notready, (unsigned long long)w.underruns, (unsigned long long)w.drops, w.av_n ? "" : "n/a ", w.av_n ? w.av_sum / w.av_n : 0.0, w.av_min, w.av_max,
          sm_stage, sm_movie ? ", movie" : "");
    }
}
static void sm_report(void) {   /* at exit: the session's table and hitches (tools/verify/sessionreport.py) */
    if (!sm_t0) return;
    sm_tick(1);
    L("smooth session: %.0f s, %llu pictures, held %d refreshes %.2f %%, %d: %llu, %d: %llu, more: %llu; in-between left out %llu; not ready %llu; sound underruns %llu, drops %llu; A/V %+.0f ms (%+.0f..%+.0f)\n",
      host_now() - sm_t0, (unsigned long long)sm_tot.pics, sm_expect, sm_tot.pics ? 100.0 * sm_tot.h_ok / sm_tot.pics : 0.0, sm_expect - 1, (unsigned long long)sm_tot.h_short,
      sm_expect + 1, (unsigned long long)sm_tot.h_long1, (unsigned long long)sm_tot.h_long2, (unsigned long long)sm_tot.skipped, (unsigned long long)sm_tot.notready,
      (unsigned long long)sm_tot.underruns, (unsigned long long)sm_tot.drops, sm_tot.av_n ? sm_tot.av_sum / sm_tot.av_n : 0.0, sm_tot.av_min, sm_tot.av_max);
    for (int m = 0; m < sm_nmin && sm_minute; m++) { const SmStat *s = &sm_minute[m];
        L("smooth minute %d: %llu pictures, ok %llu, short %llu, long %llu, longer %llu, left out %llu, not ready %llu, underruns %llu, drops %llu, A/V %+.0f (%+.0f..%+.0f)\n", m,
          (unsigned long long)s->pics, (unsigned long long)s->h_ok, (unsigned long long)s->h_short, (unsigned long long)s->h_long1, (unsigned long long)s->h_long2,
          (unsigned long long)s->skipped, (unsigned long long)s->notready, (unsigned long long)s->underruns, (unsigned long long)s->drops, s->av_n ? s->av_sum / s->av_n : 0.0, s->av_min, s->av_max); }
    for (int i = 0; i < sm_nhit; i++)
        L("smooth hitch: %.3f s VB %llu stage %x%s: %s %d\n", sm_hit[i].t, (unsigned long long)sm_hit[i].vb, sm_hit[i].stage, sm_hit[i].movie ? " movie" : "",
          sm_hit[i].kind == 'H' ? "held" : sm_hit[i].kind == 'N' ? "not ready" : sm_hit[i].kind == 'U' ? "underrun" : sm_hit[i].kind == 'D' ? "drop" : "left out", sm_hit[i].val);
    if (sm_hit_lost) L("smooth hitch: %llu more not kept\n", (unsigned long long)sm_hit_lost);
}
const char *ov_full_lines[8];   /* (sm_tick's lines; 4, 6: the game thread's) */
static const uint32_t *ov_full(int *w, int *h) {
    static uint32_t ov[OFW * OFH]; *w = OFW; *h = OFH;
    for (int i = 0; i < OFW * OFH; i++) ov[i] = 0x00181818;
    static char cpu[96], sp[40]; snprintf(sp, sizeof sp, "%s", ov_line[0]); { char *k = strstr(sp, " SKIP"); if (k) *k = 0; }
    snprintf(cpu, sizeof cpu, "CPU %.30s %.30s", ov_line[1][0] ? ov_line[1] + 5 : "-", sp);   /* the game thread's work per VBlank, the speed */
    const char *ln[8]; for (int i = 0; i < 8; i++) ln[i] = ov_full_lines[i] ? ov_full_lines[i] : "";
    ln[4] = ov_line[3]; ln[6] = cpu;
    for (int i = 0; i < 8; i++) ov_text(ov, OFW, OFH, 4, 4 + 12 * i, ln[i]);
    /* the graph: each bar a picture's time on the screen (40 ms full height); the line where it should be; under
       each bar white for a real picture, grey for an in-between one */
    const int gx = 4, gy = 104, gh = 48; const double per = gh / 40.0;
    for (int y = gy; y < gy + gh; y++) for (int x = gx; x < gx + 2 * OVG; x++) ov[y * OFW + x] = 0x00242424;
    const int ty = gy + gh - 1 - (int)(sm_expect * sm_rp * 1e3 * per + 0.5);
    for (int x = gx; x < gx + 2 * OVG; x += 2) if (ty >= gy) ov[ty * OFW + x] = 0x00A0A0A0;
    for (int i = 0; i < ovg_n; i++) {
        const int j = (ovg_i + OVG - ovg_n + i) % OVG, x = gx + 2 * (OVG - ovg_n + i);
        const int h = ovg[j].h;   /* RGBA in memory: 0xAABBGGRR */
        const uint32_t col = h == sm_expect ? 0x0040E040 : h < sm_expect ? 0x00FFC040 : h == sm_expect + 1 ? 0x0000E0FF : 0x004040FF;
        int bh = (int)(ovg[j].ms * per + 0.5); if (bh > gh) bh = gh; if (bh < 1) bh = 1;
        for (int y = gy + gh - bh; y < gy + gh; y++) ov[y * OFW + x] = col;
        for (int y = gy + gh + 1; y < gy + gh + 3; y++) ov[y * OFW + x] = ovg[j].real ? 0x00FFFFFF : 0x00686868;
    }
    ov_textc(ov, OFW, OFH, gx + 2 * OVG + 4, gy, "40", 0x00A0A0A0);
    { char t[16]; snprintf(t, sizeof t, "%.1f", sm_expect * sm_rp * 1e3); ov_textc(ov, OFW, OFH, gx + 2 * OVG + 4, ty - 4 < gy ? gy : ty - 4, t, 0x00A0A0A0); }
    ov_textc(ov, OFW, OFH, 4, gy + gh + 6, "GREEN EXACT YELLOW +1 RED +2 BLUE -1", 0x00A0A0A0);
    return ov;
}
#ifdef HAVE_VULKAN
/* the picture of each present (by id), for the log; scanout times (VK_EXT_present_timing) logged as "SCAN" */
static uint64_t ptag[256]; static uint8_t pnew[256];
static void note_present(uint64_t pid, uint64_t tag, int isnew) { if (pid) { ptag[pid & 255] = tag; pnew[pid & 255] = (uint8_t)isnew; } }
static void log_scanouts(void) {
    uint64_t id[64]; double t[64]; const int n = gsvk_present_scanout(id, t, 64);
    for (int i = 0; i < n; i++) {
        if (plog > 0) plog_line("SCAN", t[i], ptag[id[i] & 255], pnew[id[i] & 255] ? "new" : "rep");
        if (pnew[id[i] & 255]) sm_shown(ptag[id[i] & 255], t[i], 1);
    }
}
/* The refresh period from the times presents reached the screen: each interval is a whole number of refreshes (one,
   unless a refresh was missed), so the period is the time over the refreshes counted. The single intervals jitter
   (in a window, where the compositor shows them, +-1.5 ms), the sum over 10 s does not. */
enum { NIVL = 1200 };
static double period_of(const double *v, int n, double guess, int *irregular) {
    double t = 0, k = 0; int bad = 0;
    for (int i = 0; i < n; i++) { double r = v[i] / guess; const double kk = r < 1.5 ? 1 : (double)(int)(r + 0.5); t += v[i]; k += kk; if (kk > 1 || r < 0.6) bad++; }
    *irregular = bad * 50 > n;   /* more than 2 % missed refreshes or doubled presents */
    return k > 0 ? t / k : guess;
}
/* one refresh of the display-locked way (vsync on): returns 0 if the window cannot be presented this way now */
static int ui_refresh(int x2) {
    static int gen = -1, N, hold, locked, nivl, ivl_i, irregular, took_last, depth = -1;
    static double rp, ivl[NIVL], t_last, sched, win_t0, calm_until, ph, q_rd, q_qd, q_n; static uint64_t rc, ticks, miss_win, take_win, misses;
    static int minrd_win = 99;
    static int timed_ok = -1; static double slack;
    if (depth < 0) { depth = getenv("HG_PRESENT_DEPTH") ? atoi(getenv("HG_PRESENT_DEPTH")) : 1; if (depth < 1) depth = 1; if (depth > 3) depth = 3; }
    if (timed_ok < 0) { timed_ok = getenv("HG_PRESENT_TIMED") && atoi(getenv("HG_PRESENT_TIMED"));   /* (opt-in: not yet measured long enough) */ slack = (getenv("HG_PRESENT_SLACK") ? atof(getenv("HG_PRESENT_SLACK")) : 10.0) / 1000; }
    const int m = x2 ? 2 : 1;
    /* Timed (VK_EXT_present_timing, relative to the previous present): each picture is presented once, to be shown
       exactly `hold` refreshes after the one before it; it is taken `slack` before that (HG_PRESENT_SLACK, 10 ms), so
       the window does not present at every refresh. HG_PRESENT_TIMED=1 turns it on: so far it was measured for a
       minute only (1 picture in 323 held 3 refreshes, like the present-every-refresh way's 1 in 300). */
    const int timed = locked && timed_ok && gsvk_present_timed() && gen == gsvk_present_gen();
    if (timed && t_last > 0) {
        const double due = t_last + hold * rp - slack;
        for (double tn = host_now(); tn < due; tn = host_now()) { if (due - tn > 0.002) SDL_Delay((Uint32)((due - tn - 0.0015) * 1000)); }
    }
    const double now = host_now();
    if (gsvk_present_gen() != gen) {   /* a new swapchain (start, resize, full screen, another display): measure again */
        gen = gsvk_present_gen(); nivl = ivl_i = 0; t_last = 0; locked = 0; rc = 0; sched = now; irregular = 0;
        sm_last_t = 0; sm_scan_seen = -1;
        SDL_DisplayMode dm; const int d = SDL_GetWindowDisplayIndex(win);
        rp = 1.0 / 60; if (d >= 0 && !SDL_GetDesktopDisplayMode(d, &dm) && dm.refresh_rate > 0) rp = 1.0 / dm.refresh_rate;
        lock_set(0, 0, 0, 0, 0);
    }
    /* take the next picture? */
    int adv;
    if (timed) adv = 1;
    else if (locked) adv = rc % (uint64_t)hold == 0;
    else { adv = now + rp >= sched - rp / 2; if (adv) { sched += 1.0 / (59.94 * m); if (now > sched + 0.1) sched = now; } }
    int s = -1, rd = 0, qd = 0; uint64_t id = 0, tag = pres_tag; const char *kind = "rep";
    if (adv) {
        if (gs_display_gpu_q(&s, &id, locked ? (av_on ? 6 : 4) : 0, &rd, &qd)) { tag = pres_tag = gs_display_tag(); kind = "new"; }
        else {
            s = -1;
            if (locked && (qd > 0 || took_last)) {   /* a picture was due and is not ready: shown again (a stutter); the game runs earlier */
                misses++; miss_win++; ph += 0.25; if (ph > 4) ph = 4; calm_until = now + 20;
                sm_cur.notready++; sm_hitch('N', qd, vb_last);
                static int nl; if (nl++ < 50) L("host: VB tick %llu: no picture ready (%d waiting): shown again; game phase now %.2f VBlanks earlier\n", (unsigned long long)ticks, qd, ph);
            }
        }
        took_last = s >= 0;
        if (locked && s >= 0) { take_win++; if (rd < minrd_win) minrd_win = rd; q_rd += rd; q_qd += qd; q_n++; }
    }
    if (locked && rc % (uint64_t)N == 0) {   /* a display VBlank: the game thread's tick */
        ticks++; lock_set(1, ticks, now, rp * N, ph);
        {   /* every 10 s: the lock's state (host log) */
            static double t0; static uint64_t k0, m0;
            if (!t0 || now < t0) { t0 = now; k0 = ticks; m0 = misses; }
            else if (now - t0 >= 10) {
                L("host pacing: locked at %.3f Hz / %d, %.3f ticks a second, %llu not ready, phase %.2f, game at VBlank %llu of tick %llu; at a take %.2f ready of %.2f waiting\n", 1 / rp, N,
                  (double)(ticks - k0) / (now - t0), (unsigned long long)(misses - m0), ph, (unsigned long long)vb_last, (unsigned long long)ticks, q_n ? q_rd / q_n : 0, q_n ? q_qd / q_n : 0);
                t0 = now; k0 = ticks; m0 = misses; q_rd = q_qd = q_n = 0;
            }
        }
        {   /* A/V (see av_on): every 2 s */
            static double t_av; double o;
            if (av_on && now - t_av >= 2) { t_av = now;
                if (av_ctl_take(&o)) {
                    const double ph0 = ph;
                    if (o < -12) { if (!av_audio_move(-o - 6, -1) && ph < 4) { ph += o < -40 ? 0.5 : 0.25; if (ph > 4) ph = 4; } }   /* (big offsets, at start: half VBlanks) */
                    else if (o > 12) { if (ph > -3 && now > calm_until) ph -= 0.25; else av_audio_move(o - 6, 1); }
                    static int nl; if (ph != ph0 && nl++ < 200) L("host: A/V %+.0f ms: game phase now %.2f VBlanks\n", o, ph);
                }
            }
        }
        if (now - win_t0 >= 10) {   /* two pictures ready at every take for 10 s: the game a quarter VBlank later (without A/V control) */
            if (!av_on && win_t0 > 0 && !miss_win && take_win > 300 && minrd_win >= 2 && now > calm_until && ph > -3) {
                ph -= 0.25; static int nl; if (nl++ < 50) L("host: two pictures ready at every take for 10 s: game phase now %.2f VBlanks\n", ph);
            }
            win_t0 = now; miss_win = take_win = 0; minrd_win = 99;
        }
    }
    sm_rp = rp; sm_expect = locked ? hold : (int)(1 / (rp * 59.94 * m) + 0.5); if (sm_expect < 1) sm_expect = 1;
    sm_tick(0);
    const double t_pc = host_now();
    int ow = 0, oh = 0; const uint32_t *ov = ov_build(&ow, &oh);
    const uint64_t pid = gsvk_present(s, id, ov, ow, oh, timed ? (uint64_t)(hold * rp * 1e9 + 0.5) : 0);
    const double t_pr = host_now();
    if (!pid) { if (s >= 0) { } SDL_Delay(8); t_last = 0; lock_set(0, 0, 0, 0, 0); locked = 0; return 1; }   /* (minimized, the swapchain being made again) */
    note_present(pid, tag, kind[0] == 'n');
    if (plog > 0) plog_line("PRESENT", host_now(), tag, 0);
    /* wait until the present `depth - 1` before this one is on the screen: the display paces this loop */
    log_scanouts();
    const uint64_t wp = timed ? pid : pid > (uint64_t)(depth - 1) ? pid - (uint64_t)(depth - 1) : 0;
    const int w = wp ? gsvk_present_wait(wp, 100000000ull) : -1;
    const double t = host_now();
    const int k = timed ? hold : 1;   /* refreshes this present stands for */
    rc += (uint64_t)k;
    if (w == 1) {
        if (plog > 0) plog_line("SHOWN", t, ptag[wp & 255], pnew[wp & 255] ? "new" : "rep");
        if (pnew[wp & 255]) sm_shown(ptag[wp & 255], t, 0);
        static int dbg = -1; if (dbg < 0) dbg = getenv("HG_PACEDBG") ? atoi(getenv("HG_PACEDBG")) : 0;
        {   /* HG_PACEDBG: the GPU timing of a late present, once it is known */
            static uint64_t lp[16]; static double lt[16]; static int nlp;
            if (dbg && t_last > 0 && t - t_last > (k + 0.5) * rp && nlp < 16) { lp[nlp] = wp; lt[nlp] = t - t_last; nlp++; }
            for (int i = 0; i < nlp; i++) { double sb, bg, en; extern int gsvk_present_gpu(uint64_t, double *, double *, double *);
                if (gsvk_present_gpu(lp[i], &sb, &bg, &en)) { static int ng; if (ng++ < 200) L("host pacedbg: present %llu (interval %.2f ms): blit began %.2f ms after its submit, took %.3f ms\n",
                    (unsigned long long)lp[i], lt[i] * 1e3, (bg - sb) * 1e3, (en - bg) * 1e3); lp[i] = lp[--nlp]; lt[i] = lt[nlp]; i--; }
                else if (lp[i] + 16 < wp) { lp[i] = lp[--nlp]; lt[i] = lt[nlp]; i--; } }
            /* a reference: every 500th present's numbers */
            static uint64_t cnt; if (dbg && ++cnt % 500 == 0 && nlp < 16) { lp[nlp] = wp; lt[nlp] = t - t_last; nlp++; }
        }
        if (dbg && t_last > 0 && t - t_last > (k + 0.5) * rp) { static int nd; if (nd++ < 200)
          { extern double gsvk_present_ms[5];
            L("host pacedbg: interval %.2f ms: before the present %.2f (events, take), present call %.2f (fence %.2f acquire %.2f record %.2f submit %.2f present %.2f), wait %.2f; since the last done %.2f at its loop start\n",
              (t - t_last) * 1e3, (t_pc - now) * 1e3, (t_pr - t_pc) * 1e3, gsvk_present_ms[0], gsvk_present_ms[1], gsvk_present_ms[2], gsvk_present_ms[3], gsvk_present_ms[4], (t - t_pr) * 1e3, (now - t_last) * 1e3); } }
        if (t_last > 0) { ivl[ivl_i] = (t - t_last) / k; ivl_i = (ivl_i + 1) % NIVL; if (nivl < NIVL) nivl++; }
        {   /* presents late several times in a row (the compositor taking one in 3 refreshes, playtest 7: 40 Hz for
               seconds): the game follows the display's ticks, so it slowed down with it, sound running dry; the lock
               is dropped at once (the game then follows the host clock) and the check below locks again later */
            static int late_run;
            late_run = t_last > 0 && t - t_last > (k + 0.5) * rp ? late_run + 1 : 0;
            if (locked && late_run >= 6) {
                L("host: display: %d presents in a row late (%.1f ms apart): not locked\n", late_run, (t - t_last) * 1e3);
                locked = 0; irregular = 1; rc = 0; sched = t; lock_set(0, 0, 0, 0, 0); late_run = 0;
            }
        }
        t_last = t;
    } else t_last = 0;   /* (no present wait: FIFO's blocking paces the loop; the period stays the display mode's) */
    if (nivl >= 120 && rc % 120 == 0) {   /* the refresh period, and whether it locks (with a margin once locked) */
        int irr; double r = period_of(ivl, nivl, rp, &irr);
        const double rd = gsvk_present_refresh(); if (rd > 0 && fabs(rd / r - 1) < 0.02) r = rd;   /* the period the swapchain reports, when there is one */
        rp = r;
        const double hz = 1.0 / r; const int n = (int)(hz / 59.94 + 0.5);
        const int ok = !irr && n >= 1 && fabs(hz / n / 59.94 - 1) <= (locked ? 0.006 : 0.005) && n % m == 0 && !(getenv("HG_LOCK") && !atoi(getenv("HG_LOCK")));
        if (ok != locked || (ok && n != N) || irr != irregular) {
            if (ok) L("host: display %.3f Hz: locked, each picture held %d refresh%s, emulation at %.3f VBlanks a second\n", hz, n / m, n / m > 1 ? "es" : "", hz / n);
            else if (irr) L("host: display %.3f Hz: refreshes missed or present times irregular: not locked\n", hz);
            else L("host: display %.3f Hz: not a whole multiple of 59.94 Hz%s: pictures at 59.94 Hz, each on the refresh nearest its time\n", hz, n % m ? " for this frame rate" : "");
            locked = ok; N = n; hold = n / m; irregular = irr; rc = 0; sched = t;
            if (!locked) lock_set(0, 0, 0, 0, 0);
        }
    }
    return 1;
}
#endif
static int ui_main(void *arg) {
    (void)arg;
    const int ok = host_init();
    av_init();
    {   /* the window's thread runs ahead of the game's (ABOVE_NORMAL): it has a deadline at every refresh */
        const char *e = getenv("HG_UIPRIO"); const int p = e ? atoi(e) : 1;
        if (p > 0) SDL_SetThreadPriority(p >= 2 ? SDL_THREAD_PRIORITY_TIME_CRITICAL : SDL_THREAD_PRIORITY_HIGH);
    }
    SDL_AtomicSet(&ui_ready, ok ? 1 : -1);
    if (!ok) return 0;
    extern int gs_interp_now(void);
    int x2 = settings_ui_display_rate_x2();   /* 120 fps: two presents per VBlank (then from what the GS does) */
    double rate = 59.94 * (x2 ? 2 : 1), period = 1.0 / rate;
    double next = host_now();
    { const char *e = getenv("HG_PRESENTLOG"); plog = e ? 600 : 0; plog_from = e ? atof(e) : 0; if (e && strchr(e, ':')) plog = atoi(strchr(e, ':') + 1); t_ui0 = next; }
    L("host: presenting at %.2f Hz\n", rate);
    for (;;) {
        if (SDL_AtomicGet(&ui_stop)) {   /* host_shutdown(): the GPU done with the window before it goes */
#ifdef HAVE_VULKAN
            if (vkmode == 2) gsvk_present_shutdown();
#endif
            ui_rumble(1);
            SDL_DestroyWindow(win); SDL_AtomicSet(&ui_stopped, 1); return 0;
        }
        ui_settings(); ui_events(); ui_input(); ui_rumble(0);
#ifdef HAVE_VULKAN
        if (gsvk_ready() > 0 && (gs_interp_now() >= 2) != x2) {   /* (once the GPU renderer runs) */
            x2 = gs_interp_now() >= 2; rate = 59.94 * (x2 ? 2 : 1); period = 1.0 / rate;
            L("host: presenting at %.2f Hz\n", rate);
        }
        if (vkmode == 2 && ui_vsync == VSYNC_ON && ui_refresh(x2)) { next = host_now(); continue; }
        lock_set(0, 0, 0, 0, 0);
#endif
        const uint64_t pid = ui_present();
#ifdef HAVE_VULKAN
        if (vkmode == 2) {   /* (holds: the scanout times; the expected hold from the display's refresh) */
            static uint64_t last; note_present(pid, pres_tag, pres_tag != last); last = pres_tag;
            static double rfr; if (!rfr || gsvk_present_refresh() > 0) { const double r = gsvk_present_refresh(); SDL_DisplayMode dm; const int d = SDL_GetWindowDisplayIndex(win);
                rfr = r > 0 ? r : d >= 0 && !SDL_GetDesktopDisplayMode(d, &dm) && dm.refresh_rate > 0 ? 1.0 / dm.refresh_rate : 1.0 / 60; }
            sm_rp = rfr; sm_expect = (int)(1 / (rfr * rate) + 0.5); if (sm_expect < 1) sm_expect = 1;
            if (plog <= 0) log_scanouts();
        }
        sm_tick(0);
        if (av_on && vkmode == 2) {   /* A/V without the display lock: the ring's target only */
            static double t_av; double o; const double tn = host_now();
            if (tn - t_av >= 2) { t_av = tn; if (av_ctl_take(&o)) { if (o < -12) av_audio_move(-o - 6, -1); else if (o > 12) av_audio_move(o - 6, 1); } }
        }
#endif
        if (plog > 0) {
            const double tp = host_now(); const uint64_t tag = pres_tag;
            plog_line("PRESENT", tp, tag, 0);
#ifdef HAVE_VULKAN
            if (pid && gsvk_present_wait(pid, 20000000ull) == 1) plog_line("SHOWN", host_now(), tag, ptag[pid & 255] == tag && pnew[pid & 255] ? "new" : "rep");
            log_scanouts();
#endif
        }
        (void)pid;
        next += period;
        double now = host_now();
        if (now > next + 0.1) next = now;   /* far behind (a stall): start again from here */
        if (next - now > 0.002) SDL_Delay((Uint32)((next - now - 0.0015) * 1000));
        while (host_now() < next) { }   /* the last moment, precisely */
    }
    return 0;
}
void host_vblank(uint64_t vb) {
    {   /* the stage and whether a movie plays (the IPU decoded in the last half second), for the smoothness and A/V logs */
        uint32_t obj = 0, stg = 0; memcpy(&obj, gptr(0x44f818), 4); if (obj) memcpy(&stg, gptr(obj + 0x30), 4); sm_stage = stg;   /* (Fiona's object: +0x30 the stage, 0x2a = ST_02A) */
        static uint64_t b0, bvb; const uint64_t b = ipu_bdec_count(); if (b != b0) { b0 = b; bvb = vb; }
        const int mv = bvb && vb - bvb < 30;
        if (mv != sm_movie) L(mv ? "movie: \"%s\" decoding from VBlank %llu\n" : "movie: \"%s\" ended at VBlank %llu (the IPU idle)\n", movie_now[0] ? movie_now : "?", (unsigned long long)(mv ? vb : bvb));
        sm_movie = mv;
    }
    if (active < 0) {
        if (!host_window_wanted()) active = 0;
        else {   /* the UI thread creates the window; wait for it */
            SDL_SetMainReady(); SDL_AtomicSet(&in_an, (int)0x80808080u);   /* sticks centred until the first poll */
            SDL_Thread *t = SDL_CreateThread(ui_main, "hg-ui", 0);
            if (!t) active = 0;
            else { SDL_DetachThread(t); while (!SDL_AtomicGet(&ui_ready)) SDL_Delay(1); active = SDL_AtomicGet(&ui_ready) > 0; }
        }
    }
    if (!active) return;
    {   /* keep emulated time at real-time pace while a window is open: VBlanks follow the display's ticks when the
           window is display-locked (see "pacing" above), else 1/59.94 s of the host clock */
        extern int timing_wall; static Uint64 t0; static uint64_t vb0; static int anch; static uint64_t vb_a; static double n_a;
        /* HG_FF=N (for replays): no pacing and no sound before VBlank N, so a moment late in a recording is reached
           quickly and then watched at real time */
        static long ff = -1; if (ff < 0) ff = getenv("HG_FF") ? atol(getenv("HG_FF")) : 0;
        host_ff = (long)vb < ff;
        if (host_ff) { anch = 0; t0 = 0; }
        else if (ff && (long)vb == ff) L("host: HG_FF: real time from VBlank %ld\n", ff);
        if (!timing_wall && !host_ff) {
            int on; uint64_t n; double tt, per, ph;
            SDL_AtomicLock(&lk_lock); on = lk_on; n = lk_n; tt = lk_t; per = lk_per; ph = lk_phase; SDL_AtomicUnlock(&lk_lock);
            double now = host_now();
            if (on && per > 0 && now - tt < 0.1) {   /* the tick count, extended to now: D; VBlank vb is due at D = n_a + (vb - vb_a) - phase */
                double D = (double)n + (now - tt) / per;
                if (!anch || vb < vb_a) { anch = 1; vb_a = vb; n_a = D + ph; }
                double wait = (n_a + (double)(vb - vb_a) - ph - D) * per;
                if (wait < -0.25) { vb_a = vb; n_a = D + ph; }   /* running slow: do not try to catch up */
                else if (wait > 0.001) {
                    const double s0 = now;
                    if (wait > 0.1) wait = 0.1;
                    if (wait > 0.0015) SDL_Delay((Uint32)((wait - 0.001) * 1000));
                    while (host_now() < s0 + wait) { }
                    host_idle += host_now() - s0;
                }
                t0 = 0;
            } else {
                anch = 0;
                Uint64 nowc = SDL_GetPerformanceCounter(), f = SDL_GetPerformanceFrequency();
                if (!t0 || vb < vb0) { t0 = nowc; vb0 = vb; }
                double due = (double)(vb - vb0) / 59.94, el = (double)(nowc - t0) / f;
                if (due > el + 0.002) { const double s0 = host_now(); SDL_Delay((Uint32)((due - el) * 1000)); host_idle += host_now() - s0; }
                else if (el - due > 0.25) { t0 = nowc; vb0 = vb; }     /* running slow: do not try to catch up */
            }
        }
        vb_wall[vb & 1023] = host_now(); vb_last = vb;
        vb_samp[vb & 1023] = spu2_sample_count(); vb_samp_vb[vb & 1023] = vb;
    }
    if (SDL_AtomicGet(&ui_quit)) stop("window closed");
    const uint32_t an = (uint32_t)SDL_AtomicGet(&in_an); const uint8_t a4[4] = { (uint8_t)an, (uint8_t)(an >> 8), (uint8_t)(an >> 16), (uint8_t)(an >> 24) };
    pad_set_input((uint16_t)SDL_AtomicGet(&in_held), a4);
    if (overlay < 0) { overlay = getenv("HG_OVERLAY") ? atoi(getenv("HG_OVERLAY")) : 0; if (overlay < 0 || overlay > 2) overlay = 1; }
    ov_stats();
}
void host_shutdown(void) {
    if (active != 1) return;
    SDL_AtomicSet(&ui_stop, 1);
    for (int i = 0; i < 3000 && !SDL_AtomicGet(&ui_stopped); i++) SDL_Delay(1);
    if (SDL_AtomicGet(&ui_stopped)) sm_report();
}
/* Sound: the game thread's samples go into a ring that SDL's audio thread plays from (audio_cb). The device takes
   about 21 ms per period and the game delivers about 17 ms per VBlank, so the ring keeps a cushion: playback starts
   (and starts again after an underrun) once 120 ms are queued, which also absorbs a slow second; beyond 300 ms,
   chunks are dropped. When the ring runs dry the last sample fades to silence over 5 ms and playback fades back in
   over 5 ms when it resumes, so an underrun is a short gap, not a click (the old queue stopped and started the
   device on a sample: audible beeps). Statistics are logged every 10 s.
   The game's sound comes at 48 kHz of emulated time; display-locked, emulated time runs at the display's pace
   (0.1 % fast at 120.00 Hz), and even at 59.94 Hz the host clock and the sound card's differ a little. So the samples
   are resampled on their way into the ring (a 32-tap Kaiser-windowed sinc, 256 phases): the step is 1 + 5 % per
   second of sound the ring holds beyond the 120 ms cushion (averaged over about a second), at most +-0.6 %, which
   keeps the ring centred (0.1 % fast settles 20 ms above it) instead of drifting into drops or underruns. */
enum { AR_CAP = 48000 / 2, AR_START = 48000 * 12 / 100, AR_MAX = 48000 * 3 / 10, AR_FADE = 240 };   /* frames */
static int16_t ar_buf[AR_CAP * 2]; static int ar_r;   /* ring; read position (audio thread); ar_n: frames queued */
static int ar_playing, ar_fade; static float ar_gain; static float ar_last[2]; static uint64_t ar_underruns;   /* audio thread */
static uint64_t ar_underruns_total(void) { return ar_underruns; }
static void SDLCALL audio_cb(void *ud, Uint8 *stream, int len) {   /* SDL's audio thread, with the device locked */
    (void)ud;
    int16_t *out = (int16_t *)stream; const int frames = len / 4;
    cb_t = host_now(); cb_frames = frames;
    for (int i = 0; i < frames; i++) {
        if (!ar_playing && ar_n >= ar_target()) { ar_playing = 1; ar_gain = 0; }
        if (ar_playing && ar_n > 0) {
            float l = ar_buf[ar_r * 2], r = ar_buf[ar_r * 2 + 1]; ar_r = (ar_r + 1) % AR_CAP; ar_n--;
            if (ar_gain < 1) { ar_gain += 1.0f / AR_FADE; if (ar_gain > 1) ar_gain = 1; l *= ar_gain; r *= ar_gain; }
            out[i * 2] = (int16_t)l; out[i * 2 + 1] = (int16_t)r; ar_last[0] = l; ar_last[1] = r; ar_fade = AR_FADE;
        } else {
            if (ar_playing) { ar_playing = 0; ar_underruns++; }
            const float g = ar_fade > 0 ? (float)ar_fade-- / AR_FADE : 0;   /* the last sample, fading out */
            out[i * 2] = (int16_t)(ar_last[0] * g); out[i * 2 + 1] = (int16_t)(ar_last[1] * g);
        }
    }
}
enum { RS_TAPS = 32, RS_PH = 256, RS_BUF = 4096 };
static float rs_tab[RS_PH + 1][RS_TAPS];   /* tap k of phase p: the sinc at k - 15 - p / RS_PH */
static float rs_in[2][RS_BUF + RS_TAPS]; static int rs_n = RS_TAPS / 2 - 1; static double rs_pos = RS_TAPS / 2 - 1;   /* input kept; the next output's place in it */
static double rs_step = 1, rs_lvl = -1, rs_sum; static uint64_t rs_cnt;   /* input frames per output frame; ring level, smoothed (frames) */
static double bessel_i0(double x) { double s = 1, t = 1; for (int k = 1; k < 30; k++) { t *= (x / (2 * k)) * (x / (2 * k)); s += t; } return s; }
static void rs_init(void) {
    const double fc = 0.90, beta = 7.0;   /* cutoff (of the Nyquist frequency), Kaiser window */
    for (int p = 0; p <= RS_PH; p++) {
        double sum = 0;
        for (int k = 0; k < RS_TAPS; k++) {
            const double x = k - (RS_TAPS / 2 - 1) - (double)p / RS_PH, r = x / (RS_TAPS / 2);
            const double sinc = fabs(x) < 1e-9 ? fc : sin(M_PI * fc * x) / (M_PI * x);
            const double w = fabs(r) >= 1 ? 0 : bessel_i0(beta * sqrt(1 - r * r)) / bessel_i0(beta);
            rs_tab[p][k] = (float)(sinc * w); sum += sinc * w;
        }
        for (int k = 0; k < RS_TAPS; k++) rs_tab[p][k] = (float)(rs_tab[p][k] / sum);
    }
}
/* the input frames resampled by rs_step into out (at most max frames); returns the frames written */
static int rs_run(const int16_t *s, int frames, int16_t *out, int max) {
    int no = 0;
    while (frames > 0) {
        const int take = frames < RS_BUF - rs_n ? frames : RS_BUF - rs_n;
        for (int i = 0; i < take; i++) { rs_in[0][rs_n + i] = s[i * 2]; rs_in[1][rs_n + i] = s[i * 2 + 1]; }
        rs_n += take; s += take * 2; frames -= take;
        while ((int)rs_pos + RS_TAPS / 2 < rs_n && no < max) {
            const int i = (int)rs_pos; const double ph = (rs_pos - i) * RS_PH; const int p = (int)ph; const float a = (float)(ph - p);
            const float *t0 = rs_tab[p], *t1 = rs_tab[p + 1], *x0 = &rs_in[0][i - (RS_TAPS / 2 - 1)], *x1 = &rs_in[1][i - (RS_TAPS / 2 - 1)];
            float l = 0, r = 0;
            for (int k = 0; k < RS_TAPS; k++) { const float c = t0[k] + a * (t1[k] - t0[k]); l += c * x0[k]; r += c * x1[k]; }
            out[no * 2] = (int16_t)(l > 32767 ? 32767 : l < -32768 ? -32768 : l); out[no * 2 + 1] = (int16_t)(r > 32767 ? 32767 : r < -32768 ? -32768 : r); no++;
            rs_pos += rs_step;
        }
        const int drop = (int)rs_pos - (RS_TAPS / 2 - 1);   /* keep what the next outputs still read */
        if (drop > 0) { memmove(rs_in[0], rs_in[0] + drop, (size_t)(rs_n - drop) * 4); memmove(rs_in[1], rs_in[1] + drop, (size_t)(rs_n - drop) * 4); rs_n -= drop; rs_pos -= drop; }
    }
    return no;
}
void host_audio_push(const int16_t *s, int frames) {
    if (frames > 0) aud_in += (uint64_t)frames;   /* (the SPU2's samples pushed: the A/V sample index) */
    if (host_ff) return;   /* (HG_FF: fast-forwarding) */
    if (!audio_state) {
        av_init();
        const char *a = getenv("HG_AUDIO");
        if ((a && !strcmp(a, "0")) || !(active == 1 || (a && !strcmp(a, "1")))) { audio_state = -1; return; }
        if (!SDL_WasInit(SDL_INIT_AUDIO) && SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) { L("host: no audio: %s\n", SDL_GetError()); audio_state = -1; return; }
        SDL_AudioSpec want = { 0 }, got; want.freq = 48000; want.format = AUDIO_S16SYS; want.channels = 2; want.samples = av_on ? 512 : 1024; want.callback = audio_cb;
        adev = SDL_OpenAudioDevice(0, 0, &want, &got, 0);
        if (!adev) { L("host: no audio device: %s\n", SDL_GetError()); audio_state = -1; return; }
        SDL_PauseAudioDevice(adev, 0);   /* silence until the cushion is there */
        audio_state = 1; rs_init();
        {   const char *e = getenv("HG_AUDIO_LAT_MS"); av_dev = e ? atof(e) / 1000 : got.samples / 48000.0; }   /* (the device's own latency: an estimate, see A/V above) */
        {   const char *e = getenv("HG_AUDIO_OFFSET_MS"); av_user = (e ? atof(e) : settings()->audio_offset) / 1000.0; }
        L("host: audio open (48 kHz stereo, %s, buffer %d frames; device latency taken as %.1f ms, audio_offset %+.0f ms)\n", SDL_GetCurrentAudioDriver() ? SDL_GetCurrentAudioDriver() : "?", got.samples, av_dev * 1e3, av_user * 1e3);
    }
    if (audio_state < 0 || frames <= 0) return;
    static int qmin = 1 << 30, qmax; static double tlog;
    static int mute = -1; if (mute < 0) mute = getenv("HG_AUDIO_MUTE") && atoi(getenv("HG_AUDIO_MUTE"));
    static int16_t rsout[2 * 2 * RS_BUF];
    {   /* the step from the ring's level (see above); resampled before the device is locked */
        const int lv = ar_n;
        rs_lvl = rs_lvl < 0 ? lv : rs_lvl + (lv - rs_lvl) / 60.0;
        const int tg = ar_target();
        static double rs_i;   /* (A/V: the integral part, so the level settles at the target itself) */
        double st;
        if (!av_on) st = 1 + 0.05 * (rs_lvl - tg) / 48000.0;
        else {
            const double e = (rs_lvl - tg) / 48000.0;
            if (ar_playing) { rs_i += 0.02 * e * frames / 48000.0; if (rs_i > 0.004) rs_i = 0.004; if (rs_i < -0.004) rs_i = -0.004; }
            st = 1 + 0.1 * e + rs_i;
        }
        ar_lvl_ms = rs_lvl / 48.0; ar_tgt_ms = tg / 48.0;
        if (!ar_playing) st = 1;   /* (filling the cushion, or after an underrun) */
        if (av_on) {   /* the floor against underruns */
            static uint64_t u0; static double t_ok; const double tn = host_now(); const uint64_t u = ar_underruns;
            /* the game's own pace: the samples it gave per second over the last quarter seconds (48,000 at full speed) */
            static double rt[3] = { 48000, 48000, 48000 }, t_r; static uint64_t a_r;
            if (!t_r) { t_r = tn; a_r = aud_in; }
            else if (tn - t_r >= 0.25) { rt[2] = rt[1]; rt[1] = rt[0]; rt[0] = (double)(aud_in - a_r) / (tn - t_r); t_r = tn; a_r = aud_in; }
            const double pace = (rt[0] < rt[1] ? rt[0] < rt[2] ? rt[0] : rt[2] : rt[1] < rt[2] ? rt[1] : rt[2]) / 48000;
            if (!t_ok) { t_ok = tn; u0 = u; }
            if (u > u0 && u0 && pace < 0.9) { t_ok = tn;   /* the game fell behind: a larger ring would not have helped */
                static int nl; if (nl++ < 50) L("host audio: underrun while the game ran at %.0f %% speed: the ring's floor stays %d ms\n", pace * 100, ar_floor / 48); }
            else if (u > u0 && u0) { int f = ar_floor + 48000 * 20 / 1000; if (f > 48000 * 150 / 1000) f = 48000 * 150 / 1000; ar_floor = f; t_ok = tn;
                static int nl; if (nl++ < 50) L("host audio: underrun: the ring's floor now %d ms\n", f / 48); }
            else if (u > u0) t_ok = tn;   /* (the first: the start) */
            else if (tn - t_ok >= 10 && ar_floor > ar_floor0()) { int f = ar_floor - 48000 * 10 / 1000; if (f < ar_floor0()) f = ar_floor0(); ar_floor = f; t_ok = tn; }
            u0 = u;
        }
        rs_step = st < 0.994 ? 0.994 : st > 1.006 ? 1.006 : st; rs_sum += rs_step; rs_cnt++;
    }
    frames = rs_run(s, frames, rsout, 2 * RS_BUF); s = rsout;
    SDL_LockAudioDevice(adev);
    const int q = ar_n; uint64_t und = ar_underruns;
    if (q < qmin) qmin = q;
    if (q > qmax) qmax = q;
    /* a backlog (the game catching up after a stall gives its sound in a burst) would take the 0.6 % resampling about
       40 s per 250 ms to play off, the sound late all that while: well above the target for half a second, the new
       sound is left out until the ring is back near it (one skip instead) */
    static double t_over; static int shed; const int tgt = ar_target();
    if (av_on && ar_playing && q > tgt + 48000 * 80 / 1000) { const double tn = host_now(); if (!t_over) t_over = tn; else if (tn - t_over >= 0.5 && !shed) shed = 1; }
    else t_over = 0;
    if (shed == 1) { static int nl; if (nl++ < 30) L("host audio: %d ms queued (target %d): the backlog left out\n", q / 48, tgt / 48); shed = 2; }
    if (shed && q <= tgt + 48000 * 20 / 1000) { shed = 0; t_over = 0; }
    if (shed) ar_drops++;
    else if (q + frames > AR_MAX) ar_drops++;
    else {
        int w = (ar_r + q) % AR_CAP;
        for (int i = 0; i < frames; i++) {
            ar_buf[w * 2] = mute ? 0 : s[i * 2]; ar_buf[w * 2 + 1] = mute ? 0 : s[i * 2 + 1];   /* (muted: the device's timing, no sound) */
            if (++w == AR_CAP) w = 0;
        }
        ar_n = q + frames;
        {   /* A/V: the last frame pushed reaches the speakers after the ring before it, what SDL holds, the device */
            const double now = host_now(); double held = cb_frames - (now - cb_t) * 48000; if (held < 0) held = 0;
            const double te = now + (q + frames + held) / 48000.0 + av_dev + av_hw + av_user + (RS_TAPS / 2) * rs_step / 48000.0;
            SDL_AtomicLock(&av_lock); av_in_end = aud_in; av_t_end = te; av_step = rs_step; av_valid = ar_playing; SDL_AtomicUnlock(&av_lock);
        }
    }
    SDL_UnlockAudioDevice(adev);
    if (host_now() - tlog > 10) {
        if (tlog) L("host audio: queue %.0f-%.0f ms (target %d), %llu underruns, %llu dropped chunks, resampling %+.3f %%\n", qmin == 1 << 30 ? 0 : qmin / 48.0, qmax / 48.0, ar_target() / 48,
                    (unsigned long long)und, (unsigned long long)ar_drops, rs_cnt ? (rs_sum / rs_cnt - 1) * 100 : 0.0);
        tlog = host_now(); qmin = 1 << 30; qmax = 0; rs_sum = 0; rs_cnt = 0;
    }
}
#else
void host_audio_push(const int16_t *s, int frames) { (void)s; (void)frames; }
int host_window_wanted(void) { return 0; }
int host_window_active(void) { return 0; }
int host_fast_forward(void) { return 0; }
void host_vblank(uint64_t vb) { (void)vb; }
void host_rumble(int small, int big) { (void)small; (void)big; }
void host_shutdown(void) { }
#endif
