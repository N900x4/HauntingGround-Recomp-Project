/* Host settings: the options the player sets in the game's Options menu (the extra rows, game/options.c) and in
   hg.ini next to the executable. They never go into the game's memory or its memory-card save.

   Order of precedence, highest first:
     1. environment variables (HG_GSVK, HG_GSVK_SCALE, HG_GSVK_INTERP, HG_VSYNC, HG_SCALE, HG_FULLSCREEN), so every
        test script keeps working unchanged;
     2. while replaying a recording (HG_PADPLAY): the settings written in its header, then the changes it recorded;
     3. hg.ini (HG_INI=path to use another file, HG_INI=off for none);
     4. the defaults, which are the behaviour without any of the above.
   One log line at start lists the effective settings ("settings: ...").

   Threads: the game thread owns the settings (the Options rows run there, and so do the recorded changes); the UI
   thread reads the few it needs through settings_ui_*() and asks for changes it originates (F11, a captured key,
   the type of controller) through settings_host_request(), which the game thread applies at the next VBlank so a
   recording can write them down at that VBlank. */
#ifndef SETTINGS_H
#define SETTINGS_H
#include <stdio.h>
#include <stdint.h>

/* the emulated controller's buttons in the order of its button word (iop_pad_mc.c), then the left stick's four
   directions (keyboard only: a controller's sticks are analog) */
enum {
    PB_SELECT, PB_L3, PB_R3, PB_START, PB_UP, PB_RIGHT, PB_DOWN, PB_LEFT,
    PB_L2, PB_R2, PB_L1, PB_R1, PB_TRIANGLE, PB_CIRCLE, PB_CROSS, PB_SQUARE,
    PB_LS_UP, PB_LS_DOWN, PB_LS_LEFT, PB_LS_RIGHT,
    PB_N
};
#define PB_BUTTONS 16

/* a controller input: an SDL game-controller button (0-20), or one of the triggers */
enum { PAD_LT = 100, PAD_RT = 101, PAD_NONE = -1 };

enum { VSYNC_MAILBOX = 0, VSYNC_ON = 1, VSYNC_OFF = 2 };   /* 0: no tearing, the game's clock; 1: FIFO; 2: immediate */

typedef struct {
    int renderer;       /* 0 software GS, 1 GPU (Vulkan): the one running */
    int renderer_next;  /* the one chosen in the menu for the next start */
    int scale;          /* 0 auto (from the display), 1 native, 2-8: upscaling factor */
    int fps;            /* 30, 60 or 120 */
    int fullscreen;     /* 0 window, 1 full screen (desktop) */
    int vsync;          /* VSYNC_* */
    int window_scale;   /* the window's size at start, times 640 x 448 */
    int kb[PB_N];       /* SDL scancode per button (0: none) */
    int pad[PB_BUTTONS];   /* controller input per button (PAD_*), or PAD_NONE */
} Settings;

void settings_init(void);                 /* load, apply the environment, log; once, at start */
const Settings *settings(void);           /* the effective settings */
void settings_defaults(Settings *s);
const char *settings_ini_path(void);      /* "" when there is no file */

/* what the renderer runs with now */
int settings_gs_scale(void);              /* 1-8 (auto resolved; 1 with the software GS or at 120 fps) */
int settings_gs_interp(void);             /* 0, 1 (60 fps), 2 (120 fps); 0 with the software GS */

/* changing a setting from the game thread (the Options rows): applied at once, logged, saved to hg.ini unless a
   recording is replayed. `why` goes into the log line. Keys: renderer, resolution, fps, fullscreen, vsync,
   window_scale, kb.<button>, pad.<button> (button names as in hg.ini). Returns 0 for an unknown key or value. */
int settings_set(const char *key, const char *value, const char *why);
void settings_reset(const char *group, const char *why);   /* "display", "keyboard" or "controller" to the defaults */
void settings_save(void);
int settings_env_overrides(const char *key);   /* an environment variable sets this one at start */

/* recordings (iop_pad_mc.c) */
void settings_write_header(FILE *f);              /* "# set key=value" lines */
void settings_replay_header(const char *key, const char *value);   /* a "# set" line of a replayed recording */
void settings_replay_event(uint64_t vb, const char *key, const char *value);   /* "set VB key=value" */
void settings_vblank(uint64_t vb, int rr_mode, FILE *rec);   /* game thread, each VBlank (pad_vblank) */

/* host-originated changes (UI thread): queued, applied by the game thread at the next VBlank and recorded there */
void settings_host_request(const char *key, const char *value);
uint32_t settings_generation(void);      /* bumped by every change (the UI thread polls it) */

/* the UI thread's view */
int settings_ui_display_rate_x2(void);   /* 1 at 120 fps (two presents per VBlank) */
void settings_ui_auto_scale(int display_height);   /* the display the window is on: resolves "auto" */
int settings_auto_scale(void);
int settings_pad_type(void);             /* 0 none, 1 Xbox-like, 2 PlayStation (as recorded for the names) */
void settings_set_pad_type_live(int t);  /* UI thread: a controller came or went (recorded as an event) */

/* "press a key" / "press a button" (the mapping pages): the game thread names the target (kind 0 keyboard, 1
   controller; pb < 0: none); the UI thread turns the next key or button into a host request ("kb.<b>=<key>",
   "pad.<b>=<input>", or "capture=cancel" for Esc), recorded like any host change. The capture is over when
   settings_capture_count() changes. */
void settings_capture_target(int kind, int pb);
int settings_ui_capture_target(void);     /* kind << 8 | pb, or -1; taken (cleared) by the UI thread */
uint32_t settings_capture_count(void);
int settings_replaying(void);

/* names for the Options pages and hg.ini */
const char *settings_button_key(int pb);          /* "cross", "l1", "ls_up" ... (hg.ini keys) */
const char *settings_button_label(int pb);        /* "Cross", "L1", "L-stick up" ... */
const char *settings_key_name(int scancode);      /* "X", "Return", "-" */
const char *settings_pad_name(int input, int ps); /* "Cross"/"A", "L2"/"LT" ... ps: PlayStation names */
const char *settings_pad_ini(int input);          /* "a", "lefttrigger" ... (hg.ini) */
#endif
