/* Function overrides: host C code that replaces a recompiled game function at its entry.
   A hook returns 1 to skip the guest function (its result goes in v0) or 0 to let it run.
   Addresses are SLUS_210.75 NTSC-U; found with REA/Ghidra (see docs/runtime-notes.md). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ee_runtime.h"
#include "hle.h"

void set_hook(uint32_t a, RecHook h);

/* ------------------------------------------------------------------ movies (CRI MWSFD / Sofdec)
   Every SFD movie goes through an MWPLY handle whose function table is at 0x3e8758:
     +0x18 StartFname 0x23b618, +0x1c Stop 0x23b8e8, +0x20 GetStat 0x23c9a8 (0 stop, 1 prep, 2 playing, 3 play end, 4 error).
   The decoder drives the IPU directly, which the runtime does not emulate yet, so with HG_MOVIES=skip (the default)
   StartFname starts nothing and GetStat reports "play end"; the game's movie object then frees itself as if the movie had finished. */
#define MAXH 8
static uint32_t skipped[MAXH];
static int movie_skip = 1;
static int is_skipped(uint32_t h) { for (int i = 0; i < MAXH; i++) if (skipped[i] == h) return 1; return 0; }
static void set_skipped(uint32_t h, int on) {
    for (int i = 0; i < MAXH; i++) if (skipped[i] == h) { if (!on) skipped[i] = 0; return; }
    if (on) for (int i = 0; i < MAXH; i++) if (!skipped[i]) { skipped[i] = h; return; }
}
static int ov_mwply_start_fname(Ctx *c) {
    uint32_t h = c->r[4].u32[0], name = c->r[5].u32[0];
    L("movie: skipping \"%s\" (handle 0x%x)\n", name ? (char *)gptr(name) : "?", h);
    set_skipped(h, 1); c->r[2].u64[0] = 0; return 1;
}
static int ov_mwply_stop(Ctx *c) { uint32_t h = c->r[4].u32[0]; if (!is_skipped(h)) return 0; set_skipped(h, 0); c->r[2].u64[0] = 0; return 1; }
static int ov_mwply_get_stat(Ctx *c) { if (!is_skipped(c->r[4].u32[0])) return 0; c->r[2].u64[0] = 3; return 1; }

void overrides_init(void) {
    const char *m = getenv("HG_MOVIES");
    movie_skip = m && !strcmp(m, "skip");   /* movies play (IPU emulation); HG_MOVIES=skip skips them */
    if (movie_skip) {
        set_hook(0x23b618, ov_mwply_start_fname);
        set_hook(0x23b8e8, ov_mwply_stop);
        set_hook(0x23c9a8, ov_mwply_get_stat);
    }
}
