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
             Q/E = L1/R1, 1/3 = L2/R2, I/J/K/L = left stick; F11 = fullscreen, F3 = overlay, Esc = quit.
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
#include <SDL_syswm.h>
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
int host_window_active(void) { return active == 1; }
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
    win = SDL_CreateWindow("Haunting Ground Recomp", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 640 * scale, 448 * scale, SDL_WINDOW_RESIZABLE);
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
    {'X',{5,5,2,5,5}},
};
static void ov_text(uint32_t *fb, int w, int h, int x, int y, const char *s) {
    for (; *s; s++, x += 8) {
        const uint8_t *g = 0;
        for (size_t i = 0; i < sizeof glyphs / sizeof glyphs[0]; i++) if (glyphs[i].c == *s) g = glyphs[i].r;
        if (!g) continue;
        for (int r = 0; r < 10; r++) for (int c = 0; c < 6; c++)
            if ((g[r / 2] >> (2 - c / 2)) & 1 && x + c < w && y + r < h) fb[(y + r) * w + x + c] = 0x00FFFFFF;
    }
}
static int overlay = -1; static char ov_line[3][40];   /* written on the game thread, painted on the UI thread */
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
        t0 = now; idle0 = host_idle; n = 0; worst = 0;
    }
}
static void ov_paint(uint32_t *fb, int w, int h) {
    if (overlay <= 0) return;
    const int s = w >= 1280 ? w / 640 : 1;   /* about the same size on upscaled pictures */
    for (int y = 4 * s; y < 46 * s && y < h; y++) for (int x = 4 * s; x < 180 * s && x < w; x++) fb[y * w + x] = (fb[y * w + x] >> 2) & 0x003F3F3F;   /* dim box */
    if (s == 1) { for (int i = 0; i < 3; i++) ov_text(fb, w, h, 8, 8 + 12 * i, ov_line[i]); return; }
    static uint32_t small[46 * 180];   /* drawn at 1x, then scaled */
    for (int i = 0; i < 46 * 180; i++) small[i] = 0;
    for (int i = 0; i < 3; i++) ov_text(small, 180, 46, 8, 8 + 12 * i, ov_line[i]);
    for (int y = 0; y < 46 * s && y < h; y++) for (int x = 0; x < 180 * s && x < w; x++) if (small[(y / s) * 180 + x / s]) fb[y * w + x] = 0x00FFFFFF;
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
        if (e.type == SDL_KEYDOWN && e.key.keysym.scancode == SDL_SCANCODE_F3 && !e.key.repeat) overlay = !(overlay > 0);
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
    if (fs != fs_now) { fs_now = fs; SDL_SetWindowFullscreen(win, fs ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0); }
}
/* the F3 overlay for the GPU path (176 x 42), or 0 when it is off */
static const uint32_t *ov_build(void) {
    static uint32_t ov[176 * 42];   /* 4 pixels from the picture's corner */
    if (overlay <= 0) return 0;
    for (int i = 0; i < 176 * 42; i++) ov[i] = 0x00181818;
    for (int i = 0; i < 3; i++) ov_text(ov, 176, 42, 4, 4 + 12 * i, ov_line[i]);
    return ov;
}
static uint64_t pres_tag;   /* the picture the window shows (gs_display_tag of the last one taken) */
static uint64_t ui_present(void) {   /* the timer's way: the next picture (or the last again); the present's id (GPU path) */
#ifdef HAVE_VULKAN
    if (vkmode == 1) {   /* the GPU renderer: set up yet? */
        int r = gsvk_ready();
        if (r > 0) {
            SDL_SysWMinfo wi; SDL_VERSION(&wi.version); r = -1;
#ifdef _WIN32
            if (SDL_GetWindowWMInfo(win, &wi) && gsvk_present_init(wi.info.win.hinstance, wi.info.win.window)) { vkmode = 2; r = 1; }
#endif
        }
        if (r < 0) { vkmode = 0; sdl_renderer(); L("host: presenting with SDL\n"); }
        else if (vkmode == 1) { int s; uint64_t id; while (gs_display_gpu(&s, &id)) gsvk_pic_release(s); return 0; }   /* (drops the early pictures) */
    }
    if (vkmode == 2) {
        int s; uint64_t id = 0;
        if (!gs_display_gpu(&s, &id)) s = -1;   /* none new: the last one again */
        else pres_tag = gs_display_tag();
        return gsvk_present(s, id, ov_build(), 0);
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
        SDL_RenderSetLogicalSize(ren, 640, 448);    /* 4:3 output; the PS2 picture is stretched to the TV like the console did */
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
    plog--;
    char lat[32] = "";
    if (kind && !strcmp(kind, "new") && vb_wall[(tag >> 2) & 1023] > 0) snprintf(lat, sizeof lat, " lat %.1f", (t - vb_wall[(tag >> 2) & 1023]) * 1000);
    L("%s %.3f ms vb %llu.%d%s%s%s%s\n", what, t * 1000.0, (unsigned long long)(tag >> 2), (int)(tag >> 1 & 1), tag & 1 ? " i" : "", kind ? " " : "", kind ? kind : "", lat);
}
#ifdef HAVE_VULKAN
/* the picture of each present (by id), for the log; scanout times (VK_EXT_present_timing) logged as "SCAN" */
static uint64_t ptag[256]; static uint8_t pnew[256];
static void note_present(uint64_t pid, uint64_t tag, int isnew) { if (pid) { ptag[pid & 255] = tag; pnew[pid & 255] = (uint8_t)isnew; } }
static void log_scanouts(void) {
    uint64_t id[64]; double t[64]; const int n = gsvk_present_scanout(id, t, 64);
    for (int i = 0; i < n; i++) if (plog > 0) plog_line("SCAN", t[i], ptag[id[i] & 255], pnew[id[i] & 255] ? "new" : "rep");
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
        if (gs_display_gpu_q(&s, &id, locked ? 4 : 0, &rd, &qd)) { tag = pres_tag = gs_display_tag(); kind = "new"; }
        else {
            s = -1;
            if (locked && (qd > 0 || took_last)) {   /* a picture was due and is not ready: shown again (a stutter); the game runs earlier */
                misses++; miss_win++; ph += 0.25; if (ph > 3) ph = 3; calm_until = now + 20;
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
        if (now - win_t0 >= 10) {   /* two pictures ready at every take for 10 s: the game a quarter VBlank later */
            if (win_t0 > 0 && !miss_win && take_win > 300 && minrd_win >= 2 && now > calm_until && ph > -3) {
                ph -= 0.25; static int nl; if (nl++ < 50) L("host: two pictures ready at every take for 10 s: game phase now %.2f VBlanks\n", ph);
            }
            win_t0 = now; miss_win = take_win = 0; minrd_win = 99;
        }
    }
    const double t_pc = host_now();
    const uint64_t pid = gsvk_present(s, id, ov_build(), timed ? (uint64_t)(hold * rp * 1e9 + 0.5) : 0);
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
        if (plog > 0) {
            const double tp = host_now(); const uint64_t tag = pres_tag;
            plog_line("PRESENT", tp, tag, 0);
#ifdef HAVE_VULKAN
            static uint64_t last; note_present(pid, tag, tag != last); last = tag;
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
        if (!timing_wall) {
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
    }
    if (SDL_AtomicGet(&ui_quit)) stop("window closed");
    const uint32_t an = (uint32_t)SDL_AtomicGet(&in_an); const uint8_t a4[4] = { (uint8_t)an, (uint8_t)(an >> 8), (uint8_t)(an >> 16), (uint8_t)(an >> 24) };
    pad_set_input((uint16_t)SDL_AtomicGet(&in_held), a4);
    if (overlay < 0) overlay = getenv("HG_OVERLAY") && atoi(getenv("HG_OVERLAY"));
    ov_stats();
}
void host_shutdown(void) {
    if (active != 1) return;
    SDL_AtomicSet(&ui_stop, 1);
    for (int i = 0; i < 3000 && !SDL_AtomicGet(&ui_stopped); i++) SDL_Delay(1);
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
static void SDLCALL audio_cb(void *ud, Uint8 *stream, int len) {   /* SDL's audio thread, with the device locked */
    (void)ud;
    int16_t *out = (int16_t *)stream; const int frames = len / 4;
    for (int i = 0; i < frames; i++) {
        if (!ar_playing && ar_n >= AR_START) { ar_playing = 1; ar_gain = 0; }
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
    if (!audio_state) {
        const char *a = getenv("HG_AUDIO");
        if ((a && !strcmp(a, "0")) || !(active == 1 || (a && !strcmp(a, "1")))) { audio_state = -1; return; }
        if (!SDL_WasInit(SDL_INIT_AUDIO) && SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) { L("host: no audio: %s\n", SDL_GetError()); audio_state = -1; return; }
        SDL_AudioSpec want = { 0 }, got; want.freq = 48000; want.format = AUDIO_S16SYS; want.channels = 2; want.samples = 1024; want.callback = audio_cb;
        adev = SDL_OpenAudioDevice(0, 0, &want, &got, 0);
        if (!adev) { L("host: no audio device: %s\n", SDL_GetError()); audio_state = -1; return; }
        SDL_PauseAudioDevice(adev, 0);   /* silence until the cushion is there */
        audio_state = 1; rs_init(); L("host: audio open (48 kHz stereo)\n");
    }
    if (audio_state < 0 || frames <= 0) return;
    static uint64_t drops; static int qmin = 1 << 30, qmax; static double tlog;
    static int mute = -1; if (mute < 0) mute = getenv("HG_AUDIO_MUTE") && atoi(getenv("HG_AUDIO_MUTE"));
    static int16_t rsout[2 * 2 * RS_BUF];
    {   /* the step from the ring's level (see above); resampled before the device is locked */
        const int lv = ar_n;
        rs_lvl = rs_lvl < 0 ? lv : rs_lvl + (lv - rs_lvl) / 60.0;
        double st = 1 + 0.05 * (rs_lvl - AR_START) / 48000.0;
        if (!ar_playing) st = 1;   /* (filling the cushion, or after an underrun) */
        rs_step = st < 0.994 ? 0.994 : st > 1.006 ? 1.006 : st; rs_sum += rs_step; rs_cnt++;
    }
    frames = rs_run(s, frames, rsout, 2 * RS_BUF); s = rsout;
    SDL_LockAudioDevice(adev);
    const int q = ar_n; uint64_t und = ar_underruns;
    if (q < qmin) qmin = q;
    if (q > qmax) qmax = q;
    if (q + frames > AR_MAX) drops++;
    else {
        int w = (ar_r + q) % AR_CAP;
        for (int i = 0; i < frames; i++) {
            ar_buf[w * 2] = mute ? 0 : s[i * 2]; ar_buf[w * 2 + 1] = mute ? 0 : s[i * 2 + 1];   /* (muted: the device's timing, no sound) */
            if (++w == AR_CAP) w = 0;
        }
        ar_n = q + frames;
    }
    SDL_UnlockAudioDevice(adev);
    if (host_now() - tlog > 10) {
        if (tlog) L("host audio: queue %.0f-%.0f ms, %llu underruns, %llu dropped chunks, resampling %+.3f %%\n", qmin == 1 << 30 ? 0 : qmin / 48.0, qmax / 48.0,
                    (unsigned long long)und, (unsigned long long)drops, rs_cnt ? (rs_sum / rs_cnt - 1) * 100 : 0.0);
        tlog = host_now(); qmin = 1 << 30; qmax = 0; rs_sum = 0; rs_cnt = 0;
    }
}
#else
void host_audio_push(const int16_t *s, int frames) { (void)s; (void)frames; }
int host_window_wanted(void) { return 0; }
int host_window_active(void) { return 0; }
void host_vblank(uint64_t vb) { (void)vb; }
void host_rumble(int small, int big) { (void)small; (void)big; }
void host_shutdown(void) { }
#endif
