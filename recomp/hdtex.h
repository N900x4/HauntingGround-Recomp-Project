/* HD texture replacement (hdtex.c): the game's textures replaced by larger images in the upscaled picture. */
#ifndef HDTEX_H
#define HDTEX_H
#include <stdint.h>
/* 1: replacements are in use (a folder of them was found and the GPU can sample them); call after gsvk_init */
int hdtex_on(void);
/* is a transfer of this format one whose image hdtex_image wants (indexed images)? */
int hdtex_wants(uint32_t psm);
/* GS thread: an indexed image with this id (crc32 of its texels in the disc's byte order) is now in local memory */
void hdtex_image(uint32_t img);
/* GS thread: the replacement for a texture (image id, crc32 of its 1 KB CLUT and of its first 64 bytes, CLUT
   sub-palette for 4-bit textures) as a slot + 1 for the shader, or 0. May wait for the image to be decoded. */
int hdtex_get(uint32_t img, uint32_t clut, uint32_t clut16, int sub);
/* GS thread, each VBlank: decoded images go up to the GPU */
void hdtex_vblank(void);
void hdtex_stats(void);
#endif
