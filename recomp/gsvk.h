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
} GsVkSprite;
int gsvk_init(const uint32_t *xoff32, const uint32_t *xoff32z);   /* 0: no Vulkan */
int gsvk_sprite(const GsVkSprite *s, const uint32_t *clut, const uint32_t *before, uint32_t *out);
#endif
