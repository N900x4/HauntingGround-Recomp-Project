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
#include "ee_runtime.h"
#include "hle.h"
#include "settings.h"

int gs_display(uint32_t *out, int maxw, int maxh, int *pw, int *ph);
int gs_display_gpu(int *slot, uint64_t *id);
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

/* F3 (or HG_OVERLAY=1): performance readout over the picture, updated every second: emulated speed (100 % = 59.94
   VBlanks a second), host work per VBlank (pacing sleep left out) and the worst VBlank, and the audio queue. Drawn
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
        snprintf(line[1], sizeof line[1], "WORK %.1f MS MAX %.1f", busy / n * 1000, worst * 1000);
        snprintf(line[2], sizeof line[2], "AUDIO %.0f MS", adev ? SDL_GetQueuedAudioSize(adev) / 192.0 : 0.0);
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
static void ui_present(void) {
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
        else if (vkmode == 1) { int s; uint64_t id; while (gs_display_gpu(&s, &id)) gsvk_pic_release(s); return; }   /* (drops the early pictures) */
    }
    if (vkmode == 2) {
        int s; uint64_t id = 0;
        if (!gs_display_gpu(&s, &id)) s = -1;   /* none new: the last one again */
        static uint32_t ov[176 * 42];   /* the F3 overlay, 4 pixels from the picture's corner */
        if (overlay > 0) { for (int i = 0; i < 176 * 42; i++) ov[i] = 0x00181818; for (int i = 0; i < 3; i++) ov_text(ov, 176, 42, 4, 4 + 12 * i, ov_line[i]); }
        gsvk_present(s, id, overlay > 0 ? ov : 0);
        return;
    }
#endif
    int w, h;
    if (!frame && !(frame = malloc((size_t)4096 * 4096 * 4))) return;
    if (!ren) return;
    if (!gs_display(frame, 4096, 4096, &w, &h)) { SDL_SetRenderDrawColor(ren, 0, 0, 0, 255); SDL_RenderClear(ren); SDL_RenderPresent(ren); return; }
    if (!tex || w != tex_w || h != tex_h) {
        if (tex) SDL_DestroyTexture(tex);
        tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_XBGR8888, SDL_TEXTUREACCESS_STREAMING, w, h); tex_w = w; tex_h = h;
        SDL_RenderSetLogicalSize(ren, 640, 448);    /* 4:3 output; the PS2 picture is stretched to the TV like the console did */
    }
    ov_paint(frame, w, h);
    SDL_UpdateTexture(tex, 0, frame, w * 4);
    SDL_RenderClear(ren); SDL_RenderCopy(ren, tex, 0, 0); SDL_RenderPresent(ren);
}
/* HG_PRESENTLOG=S: the time and picture (VBlank, sub-picture, in-between) of 600 presents from S seconds on, to check
   the pacing (host log) */
static int ui_main(void *arg) {
    (void)arg;
    const int ok = host_init();
    SDL_AtomicSet(&ui_ready, ok ? 1 : -1);
    if (!ok) return 0;
    extern int gs_interp_now(void);
    int x2 = settings_ui_display_rate_x2();   /* 120 fps: two presents per VBlank (then from what the GS does) */
    double rate = 59.94 * (x2 ? 2 : 1), period = 1.0 / rate;
    const Uint64 f = SDL_GetPerformanceFrequency();
    double next = (double)SDL_GetPerformanceCounter() / f;
    static int plog = -1; if (plog < 0) plog = getenv("HG_PRESENTLOG") ? 600 : 0;
    const double plog_from = getenv("HG_PRESENTLOG") ? atof(getenv("HG_PRESENTLOG")) : 0, t_start = next;
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
        ui_present();
#ifdef HAVE_VULKAN
        if (gsvk_ready() > 0 && (gs_interp_now() >= 2) != x2) {   /* (once the GPU renderer runs) */
            x2 = gs_interp_now() >= 2; rate = 59.94 * (x2 ? 2 : 1); period = 1.0 / rate;
            L("host: presenting at %.2f Hz\n", rate);
        }
#endif
        if (plog > 0 && (double)SDL_GetPerformanceCounter() / f - t_start >= plog_from) { plog--; const uint64_t tg = gs_display_tag();
            L("PRESENT %.3f ms vb %llu.%d%s\n", (double)SDL_GetPerformanceCounter() / f * 1000.0, (unsigned long long)(tg >> 2), (int)(tg >> 1 & 1), tg & 1 ? " i" : ""); }
        next += period;
        double now = (double)SDL_GetPerformanceCounter() / f;
        if (now > next + 0.1) next = now;   /* far behind (a stall): start again from here */
        if (next - now > 0.002) SDL_Delay((Uint32)((next - now - 0.0015) * 1000));
        while ((double)SDL_GetPerformanceCounter() / f < next) { }   /* the last moment, precisely */
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
    {   /* keep emulated time at real-time pace while a window is open (emulated VBlanks are 1/59.94 s) */
        extern int timing_wall; static Uint64 t0; static uint64_t vb0;
        if (!timing_wall) {
            Uint64 now = SDL_GetPerformanceCounter(), f = SDL_GetPerformanceFrequency();
            if (!t0 || vb < vb0) { t0 = now; vb0 = vb; }
            double due = (double)(vb - vb0) / 59.94, el = (double)(now - t0) / f;
            if (due > el + 0.002) { const double s0 = host_now(); SDL_Delay((Uint32)((due - el) * 1000)); host_idle += host_now() - s0; }
            else if (el - due > 0.25) { t0 = now; vb0 = vb; }     /* running slow: do not try to catch up */
        }
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
void host_audio_push(const int16_t *s, int frames) {
    if (!audio_state) {
        const char *a = getenv("HG_AUDIO");
        if ((a && !strcmp(a, "0")) || !(active == 1 || (a && !strcmp(a, "1")))) { audio_state = -1; return; }
        if (!SDL_WasInit(SDL_INIT_AUDIO) && SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) { L("host: no audio: %s\n", SDL_GetError()); audio_state = -1; return; }
        SDL_AudioSpec want = { 0 }, got; want.freq = 48000; want.format = AUDIO_S16SYS; want.channels = 2; want.samples = 1024;
        adev = SDL_OpenAudioDevice(0, 0, &want, &got, 0);
        if (!adev) { L("host: no audio device: %s\n", SDL_GetError()); audio_state = -1; return; }
        audio_state = 1; L("host: audio open (48 kHz stereo)\n");   /* playback starts once the queue is primed */
    }
    if (audio_state < 0) return;
    /* The device takes about 21 ms per period and the game delivers about 17 ms per VBlank, so the queue needs a
       cushion: playback starts (and restarts after an underrun) only once 100 ms are queued, which also absorbs a
       slow second (55 VBlanks: ~80 ms short); beyond 250 ms, chunks are dropped. An underrun plays silence and a
       dropped chunk skips sound, both audible as ticks. Statistics are logged every 10 s. */
    enum { AQ_START = 48000 * 4 / 10, AQ_MAX = 48000 * 4 / 4 };
    static int playing; static uint64_t underruns, drops; static Uint32 qmin = 0xFFFFFFFF, qmax; static double tlog;
    const Uint32 q = SDL_GetQueuedAudioSize(adev);
    if (playing) { if (q < qmin) qmin = q; if (q > qmax) qmax = q; }
    if (playing && q == 0) { underruns++; SDL_PauseAudioDevice(adev, 1); playing = 0; }   /* refill before going on */
    if (host_now() - tlog > 10) {
        if (tlog) L("host audio: queue %.0f-%.0f ms, %llu underruns, %llu dropped chunks\n", qmin == 0xFFFFFFFF ? 0 : qmin / 192.0, qmax / 192.0, (unsigned long long)underruns, (unsigned long long)drops);
        tlog = host_now(); qmin = 0xFFFFFFFF; qmax = 0;
    }
    if (q > AQ_MAX) { drops++; return; }
    static int mute = -1; if (mute < 0) mute = getenv("HG_AUDIO_MUTE") && atoi(getenv("HG_AUDIO_MUTE"));
    if (mute) { static int16_t z[4096 * 2]; if (frames <= 4096) s = z; }   /* testing: the real device's timing, no sound */
    SDL_QueueAudio(adev, s, (Uint32)frames * 4);
    if (!playing && q + (Uint32)frames * 4 >= AQ_START) { SDL_PauseAudioDevice(adev, 0); playing = 1; }
}
#else
void host_audio_push(const int16_t *s, int frames) { (void)s; (void)frames; }
int host_window_wanted(void) { return 0; }
int host_window_active(void) { return 0; }
void host_vblank(uint64_t vb) { (void)vb; }
void host_rumble(int small, int big) { (void)small; (void)big; }
void host_shutdown(void) { }
#endif
