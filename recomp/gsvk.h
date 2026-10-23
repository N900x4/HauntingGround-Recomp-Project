/* GS on the GPU (Vulkan compute): see gsvk.c. GsVkSprite matches struct Prim in vk/gs_sprite.comp (std430, all
   members 4-byte scalars in order). */
#ifndef GSVK_H
#define GSVK_H
#include <stdint.h>
typedef struct {
    int32_t px0, px1, py0, py1, x0, y0; float u0, v0, du, dv;
    uint32_t rgba, fog, fbp, fbw, fpsm, fbmsk;
    int32_t scx0, scx1, scy0, scy1;
    int32_t tme, tbp, tbw, tpsm, tw, th, tcc, tfx, wms, wmt, minu, maxu, minv, maxv, bilinear, aem, ta0, ta1, csa, cpsm;
    int32_t ate, atst, aref, afail, abe, a, b, cc, d, fix, pabe, fba, colclamp, fge, need_dst, zte, ztst;
    uint32_t fogcol;
    uint32_t z, zbp; int32_t zpsm, zmsk, zread, date, datm, fb16;
    int32_t prim, iip, fst;   /* 0 sprite, 1 triangle; the rest is the triangle's set-up from draw_triangle() */
    float tx0, ty0, tx1, ty1, tx2, ty2, tinv, tu0, tu1, tu2, tv0, tv1, tv2, tq0, tq1, tq2;
    uint32_t tc0, tc1, tc2, tz0, tz1, tz2; int32_t tf0, tf1, tf2;
    int32_t lx0, ly0, lx1, ly1, ln;   /* 2 line, 3 point (texel in x0, y0) */
    int32_t serial;                   /* 1: one invocation draws every pixel in the CPU's order (buffers overlap) */
    uint32_t clutoff;                 /* set by gsvk_prim() */
    uint32_t mbase;                   /* copy of local memory drawn into: copy k at k << 20 (0: the exact one) */
    int32_t gcopy, rt, cn;            /* upscaling copies made on the GPU (batches), texturing from a render target, N */
    int32_t sx0, sy0, sx1, sy1;       /* sprite or point: the vertices in 1/16 pixels */
    float su0, sv0, su1, sv1;         /* sprite: their texture coordinates (1/16 texel) */
} GsVkSprite;
/* tables: the 8-, 4- and 16-bit block and column tables, laid out as the shader's BT8 ... CT16 offsets */
#define GSVK_TABLES 1056
/* ncopies: 1, or 1 + N*N with the sub-pixel copies for upscaling (gs.c, HG_GSVK_SCALE) */
int gsvk_init(const uint32_t *xoff32, const uint32_t *xoff32z, const int32_t *tables, int ncopies);   /* 0: no Vulkan */
/* recording, in stream order. Primitives that are not `alone` collect into a batch that one dispatch draws, each
   invocation owning a pixel and going through the list in order: only primitives without order effects between
   their pixels may share a batch, and gsvk_break() must end it where a later one depends on an earlier one's
   pixels elsewhere (gs.c decides). */
void gsvk_prim(const GsVkSprite *s, const uint32_t *clut, int alone);
void gsvk_break(void);
void gsvk_upload(uint32_t word, uint32_t nwords, const uint32_t *src);   /* CPU data into local memory (every copy) */
void gsvk_upload_masked(const uint32_t *trip, uint32_t n);              /* n (word, value, mask): those bits of those words, every copy */
void gsvk_download(const uint64_t pages[8], uint32_t *dst);              /* those 8 KB pages to dst (waits) */
void gsvk_download_copy(int k, const uint64_t pages[8], uint32_t *dst);  /* the same from copy k */
/* a display picture into dst, W x H, interleaving N x N copies from copy ly1: s->prim 4, fbp/fbw/fpsm and lx0/ly0 =
   the display buffer and its origin, px1/py1 = W/H, ln = N, ly1 = the first copy */
int gsvk_compose(const GsVkSprite *s, uint32_t *dst);
int gsvk_compose_rec(const GsVkSprite *s, int slot);              /* the same, recorded into one of 6 slots */
void gsvk_compose_fetch(int slot, uint32_t *dst, size_t words);  /* after gsvk_finish() */
/* where primitives and uploads go: copies [first, first + count) (primitives add first << 20 to their mbase) */
void gsvk_set_target(uint32_t first, uint32_t count);
void gsvk_copy_copies(uint32_t dst, uint32_t src, uint32_t count);   /* whole copies, on the GPU, in stream order */
int gsvk_resize(int ncopies);   /* local memory made again for another number of copies (contents lost); 0: kept the old size */
void gsvk_skip_exact(int on);   /* 1: batches leave out the exact copy (copy 0) of the set drawn into (replays only need the others) */
void gsvk_finish(void);                                                    /* submit and wait for everything */
uint64_t gsvk_submit(void);                                                /* submit, do not wait: an id */
void gsvk_wait(uint64_t id);                                               /* that submission (and earlier) done */
uint64_t gsvk_last(void);                                                  /* the last submission's id */
int gsvk_poll(uint64_t id);                                                /* 1: that submission (and earlier) done; never waits */
void gsvk_tag(int on);                                                     /* command buffers opened from now on: tagged (1) or not */
double gsvk_tag_ms(void);                                                  /* GPU ms of the tagged command buffers so far */
uint64_t gsvk_done(void);                                                  /* any thread: the last submission done (0 without the window) */
void gsvk_stats(uint64_t *submits, uint64_t *dispatches, double *gpu_ms, double *wait_ms);   /* wait: wall ms in submit+wait */
/* HG_GSVK_TIMING=1: GPU ms per phase: exact batches, copies batches, single primitives, compose, transfers; then the
   same five for the replays of frame interpolation */
void gsvk_phase_stats(double ms[10]);
/* presenting the window from video memory (the window's pictures never leave the GPU) */
int gsvk_ready(void);                                    /* any thread: 1 set up, 0 not yet, -1 unavailable */
int gsvk_present_ready(void);                            /* any thread: gsvk_present_init() succeeded */
int gsvk_pic(const GsVkSprite *s, uint64_t *id);         /* GS thread: as gsvk_compose, into a picture image: its index (or -1), the submission */
void gsvk_pic_release(int k);                            /* any thread: picture image k is not needed (dropped unseen) */
int gsvk_present_init(void *hinstance, void *hwnd);      /* UI thread: a swapchain on the window; 0: not possible */
uint64_t gsvk_present(int k, uint64_t id, const uint32_t *ov, uint64_t rel_ns);   /* UI thread: show picture k (k < 0: the last again), overlay 176 x 54 or 0,
                                                         rel_ns (if gsvk_present_timed()): ns after the previous one; the present's id (0: not presented) */
int gsvk_present_timed(void);                            /* UI thread: presents can be timed (VK_EXT_present_timing, relative) */
double gsvk_present_refresh(void);                       /* UI thread: the refresh period the swapchain reports, s (0: unknown) */
int gsvk_present_wait(uint64_t pid, uint64_t timeout_ns);   /* UI thread: present pid on the screen? 1 yes, 0 timeout, -1 cannot tell */
int gsvk_present_can_wait(void);                         /* VK_KHR_present_wait is in use */
int gsvk_present_scanout(uint64_t *pid, double *t, int max);   /* UI thread: when presents went out to the screen (VK_EXT_present_timing); the count */
int gsvk_present_gen(void);                              /* swapchains made so far */
void gsvk_present_fullscreen(int on);                     /* any thread: the window is full screen (HG_FSE=1: exclusive mode) */
int gsvk_present_fifo(void);                             /* UI thread: the swapchain is FIFO (waits for the display) */
void gsvk_present_shutdown(void);                        /* UI thread, before the process ends */
void gsvk_present_mode(int v);   /* any thread: 0 mailbox, 1 FIFO (vsync), 2 immediate; applied at the next present */
/* checking: the GPU's memory is set to `now`, then primitives are recorded, then the GPU's memory comes back */
void gsvk_check_begin(const uint32_t *now);
const uint32_t *gsvk_check_end(double *ms);
#endif
