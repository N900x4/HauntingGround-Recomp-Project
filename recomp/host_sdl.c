/* Host window and input (SDL2). Called once per VBlank from hle.c: polls keyboard/gamepad into the
   emulated DualShock 2 (iop_pad_mc.c) and shows the GS display picture. Without SDL (or with HG_WINDOW=0,
   or no display) everything here is a no-op and the runtime stays headless.

   Keyboard: arrows = D-pad, X = Cross, C = Circle, Z = Square, S = Triangle, Enter = Start, Right Shift = Select,
             Q/E = L1/R1, 1/3 = L2/R2, I/J/K/L = left stick, F11 = fullscreen, Esc = quit.
   Gamepad (SDL GameController): A/B/X/Y = Cross/Circle/Square/Triangle, shoulders = L1/R1, triggers = L2/R2,
             sticks = analog sticks, stick clicks = L3/R3, Back = Select, Start = Start. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ee_runtime.h"
#include "hle.h"

int gs_display(uint32_t *out, int maxw, int maxh, int *pw, int *ph);
double host_idle;   /* seconds slept to pace the window to real time (HG_SPEEDLOG leaves them out) */

#ifdef HAVE_SDL
#define SDL_MAIN_HANDLED          /* our own main(): no SDL2main, and the console stays */
#include <SDL.h>

enum { B_SELECT = 0, B_L3, B_R3, B_START, B_UP, B_RIGHT, B_DOWN, B_LEFT, B_L2, B_R2, B_L1, B_R1, B_TRIANGLE, B_CIRCLE, B_CROSS, B_SQUARE };
static SDL_Window *win; static SDL_Renderer *ren; static SDL_Texture *tex;
static SDL_GameController *pad;
static int tex_w, tex_h, active = -1;
static uint32_t frame[1024 * 1024];

static const struct { SDL_Scancode sc; int bit; } keymap[] = {
    { SDL_SCANCODE_UP, B_UP }, { SDL_SCANCODE_DOWN, B_DOWN }, { SDL_SCANCODE_LEFT, B_LEFT }, { SDL_SCANCODE_RIGHT, B_RIGHT },
    { SDL_SCANCODE_X, B_CROSS }, { SDL_SCANCODE_C, B_CIRCLE }, { SDL_SCANCODE_Z, B_SQUARE }, { SDL_SCANCODE_S, B_TRIANGLE },
    { SDL_SCANCODE_RETURN, B_START }, { SDL_SCANCODE_RSHIFT, B_SELECT }, { SDL_SCANCODE_Q, B_L1 }, { SDL_SCANCODE_E, B_R1 },
    { SDL_SCANCODE_1, B_L2 }, { SDL_SCANCODE_3, B_R2 },
};
static const struct { SDL_GameControllerButton b; int bit; } padmap[] = {
    { SDL_CONTROLLER_BUTTON_A, B_CROSS }, { SDL_CONTROLLER_BUTTON_B, B_CIRCLE }, { SDL_CONTROLLER_BUTTON_X, B_SQUARE }, { SDL_CONTROLLER_BUTTON_Y, B_TRIANGLE },
    { SDL_CONTROLLER_BUTTON_LEFTSHOULDER, B_L1 }, { SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, B_R1 }, { SDL_CONTROLLER_BUTTON_LEFTSTICK, B_L3 },
    { SDL_CONTROLLER_BUTTON_RIGHTSTICK, B_R3 }, { SDL_CONTROLLER_BUTTON_BACK, B_SELECT }, { SDL_CONTROLLER_BUTTON_START, B_START },
    { SDL_CONTROLLER_BUTTON_DPAD_UP, B_UP }, { SDL_CONTROLLER_BUTTON_DPAD_DOWN, B_DOWN }, { SDL_CONTROLLER_BUTTON_DPAD_LEFT, B_LEFT },
    { SDL_CONTROLLER_BUTTON_DPAD_RIGHT, B_RIGHT },
};

int host_window_wanted(void) { const char *w = getenv("HG_WINDOW"); return !(w && !strcmp(w, "0"))
#ifndef _WIN32
        && (getenv("DISPLAY") || getenv("WAYLAND_DISPLAY"))
#endif
        ; }
int host_window_active(void) { return active == 1; }
static void open_pad(int idx) {
    if (pad || !SDL_IsGameController(idx)) return;
    pad = SDL_GameControllerOpen(idx);
    if (pad) L("host: gamepad \"%s\" connected\n", SDL_GameControllerName(pad));
}
static int host_init(void) {
    if (!host_window_wanted()) return 0;
    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0) { L("host: SDL_Init failed: %s (running headless)\n", SDL_GetError()); return 0; }
    int scale = getenv("HG_SCALE") ? atoi(getenv("HG_SCALE")) : 2; if (scale < 1) scale = 1;
    win = SDL_CreateWindow("Haunting Ground Recomp", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 640 * scale, 448 * scale, SDL_WINDOW_RESIZABLE);
    if (!win) { L("host: no window: %s (running headless)\n", SDL_GetError()); SDL_Quit(); return 0; }
    ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED);
    if (!ren) ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
    for (int i = 0; i < SDL_NumJoysticks(); i++) open_pad(i);
    L("host: SDL window open (%s renderer, %d joystick(s))\n", ren ? "ok" : "no", SDL_NumJoysticks());
    return 1;
}
static uint8_t axis8(Sint16 v) { int a = (v + 32768) >> 8; return a < 0 ? 0 : a > 255 ? 255 : a; }
void host_vblank(uint64_t vb) {
    if (active < 0) active = host_init();
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
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT || (e.type == SDL_KEYDOWN && e.key.keysym.scancode == SDL_SCANCODE_ESCAPE)) stop("window closed");
        if (e.type == SDL_KEYDOWN && e.key.keysym.scancode == SDL_SCANCODE_F11 && !e.key.repeat) {
            int fs = SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN_DESKTOP; SDL_SetWindowFullscreen(win, fs ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
        }
        if (e.type == SDL_CONTROLLERDEVICEADDED) open_pad(e.cdevice.which);
        if (e.type == SDL_CONTROLLERDEVICEREMOVED && pad && e.cdevice.which == SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(pad))) {
            SDL_GameControllerClose(pad); pad = 0; L("host: gamepad disconnected\n");
        }
    }
    /* input */
    uint16_t held = 0; uint8_t an[4] = { 0x80, 0x80, 0x80, 0x80 };   /* rx ry lx ly */
    const Uint8 *ks = SDL_GetKeyboardState(0);
    for (size_t i = 0; i < sizeof keymap / sizeof keymap[0]; i++) if (ks[keymap[i].sc]) held |= 1u << keymap[i].bit;
    if (ks[SDL_SCANCODE_J]) an[2] = 0x00; if (ks[SDL_SCANCODE_L]) an[2] = 0xFF;
    if (ks[SDL_SCANCODE_I]) an[3] = 0x00; if (ks[SDL_SCANCODE_K]) an[3] = 0xFF;
    if (pad) {
        for (size_t i = 0; i < sizeof padmap / sizeof padmap[0]; i++) if (SDL_GameControllerGetButton(pad, padmap[i].b)) held |= 1u << padmap[i].bit;
        if (SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 16000) held |= 1u << B_L2;
        if (SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 16000) held |= 1u << B_R2;
        Sint16 lx = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTX), ly = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTY);
        Sint16 rx = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_RIGHTX), ry = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_RIGHTY);
        if (abs(lx) > 6000 || abs(ly) > 6000) { an[2] = axis8(lx); an[3] = axis8(ly); }
        if (abs(rx) > 6000 || abs(ry) > 6000) { an[0] = axis8(rx); an[1] = axis8(ry); }
    }
    pad_set_input(held, an);
    /* picture */
    int w, h;
    if (!gs_display(frame, 1024, 1024, &w, &h)) { SDL_SetRenderDrawColor(ren, 0, 0, 0, 255); SDL_RenderClear(ren); SDL_RenderPresent(ren); return; }
    if (!tex || w != tex_w || h != tex_h) {
        if (tex) SDL_DestroyTexture(tex);
        tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_XBGR8888, SDL_TEXTUREACCESS_STREAMING, w, h); tex_w = w; tex_h = h;
        SDL_RenderSetLogicalSize(ren, 640, 448);    /* 4:3 output; the PS2 picture is stretched to the TV like the console did */
    }
    SDL_UpdateTexture(tex, 0, frame, w * 4);
    SDL_RenderClear(ren); SDL_RenderCopy(ren, tex, 0, 0); SDL_RenderPresent(ren);
    (void)vb;
}
static SDL_AudioDeviceID adev; static int audio_state;   /* 0 = not tried, 1 = open, -1 = unavailable */
void host_audio_push(const int16_t *s, int frames) {
    if (!audio_state) {
        const char *a = getenv("HG_AUDIO");
        if ((a && !strcmp(a, "0")) || !(active == 1 || (a && !strcmp(a, "1")))) { audio_state = -1; return; }
        if (!SDL_WasInit(SDL_INIT_AUDIO) && SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) { L("host: no audio: %s\n", SDL_GetError()); audio_state = -1; return; }
        SDL_AudioSpec want = { 0 }, got; want.freq = 48000; want.format = AUDIO_S16SYS; want.channels = 2; want.samples = 1024;
        adev = SDL_OpenAudioDevice(0, 0, &want, &got, 0);
        if (!adev) { L("host: no audio device: %s\n", SDL_GetError()); audio_state = -1; return; }
        SDL_PauseAudioDevice(adev, 0); audio_state = 1; L("host: audio open (48 kHz stereo)\n");
    }
    if (audio_state < 0) return;
    if (SDL_GetQueuedAudioSize(adev) > 48000 * 4 / 6) return;      /* more than ~160 ms queued: drop to keep latency low */
    SDL_QueueAudio(adev, s, (Uint32)frames * 4);
}
#else
void host_audio_push(const int16_t *s, int frames) { (void)s; (void)frames; }
int host_window_wanted(void) { return 0; }
int host_window_active(void) { return 0; }
void host_vblank(uint64_t vb) { (void)vb; }
#endif
