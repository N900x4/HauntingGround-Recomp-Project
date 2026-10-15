/* Calling the game's own (recompiled) functions from host C code, for hooks that extend the game (game/options.c).

   A hook runs at the entry of a guest function, on the guest thread's context (Ctx). To call another guest function
   it sets the argument registers the way the game's compiler does (a0-a3 = r4-r7, then t0-t3 = r8-r11, further
   arguments on the stack from sp+0), points ra at a marker address and calls the function through the dispatcher.
   A GuestFrame saves the whole context first and puts it back afterwards, so the hook leaves the registers as it
   found them except for what it chooses to return. Strings and small structures the callee reads go into a frame
   carved out of the guest stack below the caller's sp, so no guest memory outside the stack is used. */
#ifndef GAME_GUEST_H
#define GAME_GUEST_H
#include <stdint.h>
#include <string.h>
#include "ee_runtime.h"
#include "hle.h"

typedef struct {
    Ctx saved;       /* the context when the frame was opened */
    uint32_t sp;     /* the guest sp while it is open: 64 bytes for stack arguments, then the scratch area */
    uint32_t top;    /* the scratch area's start (sp + 64) */
    uint32_t used;   /* bytes of the scratch area in use */
    uint32_t size;   /* the scratch area's size */
} GuestFrame;

/* open a frame with `bytes` of scratch space on the guest stack, 64 bytes below the caller's sp */
static inline void guest_frame_open(Ctx *c, GuestFrame *f, uint32_t bytes) {
    f->saved = *c;
    f->size = (bytes + 15) & ~15u;
    f->sp = (c->r[29].u32[0] - 64 - f->size - 64) & ~15u;
    f->top = f->sp + 64;
    f->used = 0;
    c->r[29].u64[0] = (uint64_t)(int64_t)(int32_t)f->sp;
}
/* every register back as it was when the frame was opened */
static inline void guest_frame_close(Ctx *c, GuestFrame *f) { *c = f->saved; }
/* bytes into the frame's scratch area: their guest address (0 when it is full) */
static inline uint32_t guest_put(GuestFrame *f, const void *p, uint32_t n) {
    const uint32_t need = (n + 15) & ~15u;
    if (f->used + need > f->size) return 0;
    const uint32_t a = f->top + f->used;
    memcpy(gptr(a), p, n);
    f->used += need;
    return a;
}
static inline uint32_t guest_str(GuestFrame *f, const char *s) { return guest_put(f, s, (uint32_t)strlen(s) + 1); }

/* call guest function `addr` with up to 8 register arguments (a0-a3, t0-t3) and `nstack` 64-bit stack arguments
   (at sp+0, sp+8, ...); returns v0. Within an open frame. */
#define GUEST_RA_MARK 0x00100008u   /* the callee returns to the host; the value is only compared by callers' checks */
static inline uint32_t guest_call(Ctx *c, uint32_t addr, int nargs, const uint32_t *args, int nstack, const uint64_t *stack) {
    const uint32_t sp = c->r[29].u32[0];
    for (int i = 0; i < nargs && i < 8; i++) c->r[4 + i].u64[0] = (uint64_t)(int64_t)(int32_t)args[i];
    for (int i = 0; i < nstack; i++) { const uint64_t v = stack[i]; memcpy(gptr(sp + 8 * i), &v, 8); }
    c->r[25].u64[0] = (uint64_t)(int64_t)(int32_t)addr;   /* t9, as for a call through a pointer */
    c->r[31].u64[0] = GUEST_RA_MARK;
    rec_call(c, addr);
    c->r[29].u64[0] = (uint64_t)(int64_t)(int32_t)sp;   /* (the callee restores it; a safety net) */
    return c->r[2].u32[0];
}
#endif
