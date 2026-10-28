/* Function overrides: host C code that replaces a recompiled game function at its entry.
   A hook returns 1 to skip the guest function (its result goes in v0) or 0 to let it run.
   Addresses are SLUS_210.75 NTSC-U; found with REA/Ghidra (see docs/runtime-notes.md). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ee_runtime.h"
#include "hle.h"
#include "settings.h"

void set_hook(uint32_t a, RecHook h);

/* ------------------------------------------------------------------ movies (CRI MWSFD / Sofdec)
   Every SFD movie goes through an MWPLY handle whose function table is at 0x3e8758:
     +0x18 StartFname 0x23b618, +0x1c Stop 0x23b8e8, +0x20 GetStat 0x23c9a8 (0 stop, 1 prep, 2 playing, 3 play end, 4 error).
   The decoder drives the IPU directly, which the runtime does not emulate yet, so with HG_MOVIES=skip (the default)
   StartFname starts nothing and GetStat reports "play end"; the game's movie object then frees itself as if the movie had finished. */
#define MAXH 8
static uint32_t skipped[MAXH], polls[MAXH];
static int movie_skip = 1; static long skip_until = -1;   /* HG_MOVIES_UNTIL=vb: skip only before that VBlank */
extern uint64_t hle_vblank_count(void);
static int skipping_now(void) { return skip_until < 0 || (long)hle_vblank_count() < skip_until; }
static int is_skipped(uint32_t h) { for (int i = 0; i < MAXH; i++) if (skipped[i] == h) return 1; return 0; }
static void set_skipped(uint32_t h, int on) {
    for (int i = 0; i < MAXH; i++) if (skipped[i] == h) { if (!on) skipped[i] = 0; return; }
    if (on) for (int i = 0; i < MAXH; i++) if (!skipped[i]) { skipped[i] = h; return; }
}
static int ov_mwply_start_fname(Ctx *c) {
    if (!skipping_now()) return 0;
    uint32_t h = c->r[4].u32[0], name = c->r[5].u32[0];
    for (int i = 0; i < MAXH; i++) if (skipped[i] == h) polls[i] = 0;
    L("movie: skipping \"%s\" (handle 0x%x)\n", name ? (char *)gptr(name) : "?", h);
    set_skipped(h, 1); c->r[2].u64[0] = 0; return 1;
}
static int ov_mwply_stop(Ctx *c) { uint32_t h = c->r[4].u32[0]; if (!is_skipped(h)) return 0; set_skipped(h, 0); c->r[2].u64[0] = 0; return 1; }
/* report "playing" for the first polls, then "play end": in-game cutscenes restart a movie that ends at once */
static int ov_mwply_get_stat(Ctx *c) {
    uint32_t h = c->r[4].u32[0]; if (!is_skipped(h)) return 0;
    int i = 0; while (i < MAXH && skipped[i] != h) i++;
    c->r[2].u64[0] = i < MAXH && polls[i]++ < 30 ? 2 : 3; return 1;
}

/* movies that play (the default): only noted, for the smoothness and A/V logs (host_sdl.c); the game's own code runs */
char movie_now[64]; volatile int movie_on;
static int ov_mwply_note_start(Ctx *c) {
    const uint32_t name = c->r[5].u32[0];
    snprintf(movie_now, sizeof movie_now, "%s", name ? (char *)gptr(name) : "?"); movie_on = 1;
    L("movie: \"%s\" starts at VBlank %llu\n", movie_now, (unsigned long long)hle_vblank_count());
    return 0;
}
static int ov_mwply_note_stop(Ctx *c) {
    (void)c; if (movie_on) L("movie: \"%s\" stopped at VBlank %llu\n", movie_now, (unsigned long long)hle_vblank_count());
    movie_on = 0; return 0;
}
/* HG_FRAMEPROF (hle.c): the game frame's start (frame_begin 0x1b86a0) and end (frame_end 0x1b8750), docs/frame-timing.md */
static int ov_frame_mark(Ctx *c, const char *what) {
    (void)c; extern FILE *hle_prof(void); extern uint64_t ee_cycles, hle_idle_cycles; extern int hle_cur_thread(void);
    FILE *f = hle_prof(); if (f) fprintf(f, "%s %llu %llu %llu %d\n", what, (unsigned long long)hle_vblank_count(), (unsigned long long)ee_cycles, (unsigned long long)hle_idle_cycles, hle_cur_thread());
    return 0;
}
/* sceSifCallRpc (0x2700e8): the client (its server's id at client->server+0x?), the function number and the caller */
static int ov_rpc_call(Ctx *c) {
    extern FILE *hle_prof(void); extern uint64_t ee_cycles; extern int hle_cur_thread(void);
    FILE *f = hle_prof(); if (!f) return 0;
    const uint32_t cl = c->r[4].u32[0], srv = cl ? *(uint32_t *)gptr(cl + 0x24) : 0;   /* client->server (sceSifClientData.server) */
    const uint32_t sid = srv ? *(uint32_t *)gptr(srv + 0x0) : 0;
    fprintf(f, "RPC %llu %llu %d client %x server %x sid %x fno %u mode %u ra %x\n", (unsigned long long)hle_vblank_count(), (unsigned long long)ee_cycles,
            hle_cur_thread(), cl, srv, sid, c->r[5].u32[0], c->r[6].u32[0], c->r[31].u32[0]);
    return 0;
}
static int ov_frame_begin(Ctx *c) { return ov_frame_mark(c, "FB"); }
static int ov_frame_end(Ctx *c) { return ov_frame_mark(c, "FE"); }
/* ------------------------------------------------------------------ widescreen
   The camera's projection (0x122810, called through the camera object's table) makes its screen matrix (+144) and,
   from a second one at half scale, its clip and cull matrices (+464, +528) with sceVu0ViewScreenMatrix (0x10e2b8;
   f12 scrz, f13 ax, f14 ay, f15 cx, f16 cy, f17 zmin, f18 zmax, f19 nearz, farz on the stack). ax is the horizontal
   scale: times the wide factor (3/4 and less) the same camera, from the same place and with the same vertical framing,
   sees as much wider as the display is, and its culling widens with it; the middle of the picture stays as in 4:3.
   Only those two calls are changed (their return addresses). */
static int ov_view_screen(Ctx *c) {
    const uint32_t ra = c->r[31].u32[0];
    if (ra == 0x122888 || ra == 0x1228c8) { const float k = settings_wide_factor(); if (k != 1.0f) c->f[13] *= k; }
    return 0;
}
void overrides_init(void) {
    set_hook(0x10e2b8, ov_view_screen);
    { extern FILE *hle_prof(void); if (hle_prof()) { set_hook(0x1b86a0, ov_frame_begin); set_hook(0x1b8750, ov_frame_end); set_hook(0x2700e8, ov_rpc_call); } }
    extern void options_init(void); options_init();
    const char *m = getenv("HG_MOVIES");
    movie_skip = m && !strcmp(m, "skip");   /* movies play (IPU emulation); HG_MOVIES=skip skips them */
    if (getenv("HG_MOVIES_UNTIL")) skip_until = atol(getenv("HG_MOVIES_UNTIL"));
    if (movie_skip) {
        set_hook(0x23b618, ov_mwply_start_fname);
        set_hook(0x23b8e8, ov_mwply_stop);
        set_hook(0x23c9a8, ov_mwply_get_stat);
    } else { set_hook(0x23b618, ov_mwply_note_start); set_hook(0x23b8e8, ov_mwply_note_stop); }
}
