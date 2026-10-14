/* GS on the GPU (Vulkan compute): see gsvk.c. GsVkSprite matches the uniform block P in vk/gs_sprite.comp
   (std140, all members 4-byte scalars in order). */
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
} GsVkSprite;
/* tables: the 8-, 4- and 16-bit block and column tables, laid out as the shader's BT8 ... CT16 offsets */
#define GSVK_TABLES 1056
int gsvk_init(const uint32_t *xoff32, const uint32_t *xoff32z, const int32_t *tables);   /* 0: no Vulkan */
/* draw one primitive on the GPU from local memory `now` (1M words); returns the GPU's local memory after it, or 0 */
const uint32_t *gsvk_draw(const GsVkSprite *s, const uint32_t *clut, const uint32_t *now);
#endif
