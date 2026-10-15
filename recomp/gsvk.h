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
void gsvk_finish(void);                                                    /* submit and wait for everything */
uint64_t gsvk_submit(void);                                                /* submit, do not wait: an id */
void gsvk_wait(uint64_t id);                                               /* that submission (and earlier) done */
void gsvk_stats(uint64_t *submits, uint64_t *dispatches, double *gpu_ms, double *wait_ms);   /* wait: wall ms in submit+wait */
/* checking: the GPU's memory is set to `now`, then primitives are recorded, then the GPU's memory comes back */
void gsvk_check_begin(const uint32_t *now);
const uint32_t *gsvk_check_end(double *ms);
#endif
