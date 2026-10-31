/* HD texture replacement. The game's textures come from the disc as indexed images with a CLUT, uploaded whole into
   GS local memory (docs/textures.md). A replacement is a PNG named after the texture's id, <img>_<clut>.png, with
   img the crc32 of the index bytes and clut the crc32 of the 1 KB CLUT exactly as uploaded (tools/textures/texdump.py
   makes the originals); <img>_<clut>_c<N>.png is the same texture drawn with CLUT sub-palette N (4-bit textures, CSA).
   The image may be any size; its alpha is 255 = opaque (the GS's 0x80). The same name with .dds instead: BC7 blocks
   (DX10 header, DXGI_FORMAT_BC7_UNORM) with the mip chain (tools/textures/bc7pack.py makes them from the PNGs: a
   quarter of the video memory, no decoding); a .dds is taken over a .png of the same id.

   gs.c works out a texture setup's id from local memory itself (the texels at TBP read back in the disc's byte order,
   and the CLUT at CBP): the game sends some textures rearranged so that they land right (the characters' 8H ones) and
   some CLUTs inside larger transfers, so the bytes sent do not identify them, what they become does. The replacement
   is then sampled, in the upscaling copies only, instead of local memory (vk/gs_sprite.comp, COPIES). The exact copy
   of local memory never sees it, so the game and every frame hash are unchanged. A replacement is decoded (worker
   threads, with its mipmaps) as soon as its image has been uploaded (hdtex_image), which the game does while
   loading. A draw that needs one still being decoded: with the window, the original is drawn until it is ready (a
   wait there stalls the game, and the sound runs dry); headless (the gates, replays without a window) the draw waits
   for it, so the pictures do not depend on timing. HG_HDTEX_WAIT=1 / 0 always / never waits.
   HG_HDTEX=DIR (or hd_textures in hg.ini) names the folder, by default ../textures (the release's textures folder,
   next to bin/) when there is one; HG_HDTEX=0 turns it off. hd = off in hg.ini (the Options menu's "HD textures")
   leaves the replacements out at once, without unloading them. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <pthread.h>
#include <stdatomic.h>
#include "ee_runtime.h"
#include "hle.h"
#include "hdtex.h"
#include "settings.h"
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO_WARNINGS
#include "third_party/stb_image.h"

#ifdef HAVE_VULKAN
int gsvk_hd_ok(void);   /* gsvk.c */
int gsvk_hd_upload(int slot, int w, int h, int levels, const uint8_t *pix, int bc7);
int gsvk_hd_bc7(void);
double gsvk_hd_mb(void);
#else
static int gsvk_hd_ok(void) { return 0; }
static int gsvk_hd_bc7(void) { return 0; }
static int gsvk_hd_upload(int slot, int w, int h, int levels, const uint8_t *pix, int bc7) { (void)slot; (void)w; (void)h; (void)levels; (void)pix; (void)bc7; return 0; }
static double gsvk_hd_mb(void) { return 0; }
#endif

typedef struct {
    uint32_t img, clut; int csa;
    char *path;
    _Atomic int state;   /* 0 not wanted yet, 1 queued, 2 decoding, 3 decoded, 4 on the GPU, -1 failed */
    int w, h, levels, bc7; uint8_t *pix;   /* bc7: a .dds of BC7 blocks with its mip chain (else a .png: RGBA, mips made here) */
    int slot;
} Ent;
static Ent *ents; static int nents, cap_ents;
static int *tab; static uint32_t tabmask;   /* id -> entry, open addressing */
static int on = -1;
static uint64_t n_hit, n_miss, n_wait, n_late; static double wait_ms; static int nslots;

static uint32_t hkey(uint32_t img, uint32_t clut, int csa) { uint64_t h = ((uint64_t)img << 32 | clut) * 0x9E3779B97F4A7C15ull; return (uint32_t)(h >> 32) ^ (uint32_t)csa * 0x85EBCA6Bu; }
static Ent *find(uint32_t img, uint32_t clut, int csa) {
    if (!tab) return 0;
    for (uint32_t i = hkey(img, clut, csa) & tabmask; tab[i] >= 0; i = (i + 1) & tabmask) {
        Ent *e = &ents[tab[i]]; if (e->img == img && e->clut == clut && e->csa == csa) return e; }
    return 0;
}

/* ------------------------------------------------------------------ decoding (worker threads) */
static pthread_mutex_t mx = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t cv_work = PTHREAD_COND_INITIALIZER, cv_done = PTHREAD_COND_INITIALIZER;
static int *queue_, qn, qhead;   /* each entry is queued at most once: nents long */
static void mips(Ent *e, uint8_t *base) {   /* the whole chain after level 0, each a 2x2 box of the one before */
    int w = e->w, h = e->h; uint8_t *src = base; size_t off = (size_t)w * h * 4;
    e->levels = 1;
    while (w > 1 || h > 1) {
        const int nw = w > 1 ? w / 2 : 1, nh = h > 1 ? h / 2 : 1; uint8_t *dst = base + off;
        for (int y = 0; y < nh; y++) for (int x = 0; x < nw; x++) {
            const int x0 = x * 2, x1 = x * 2 + 1 < w ? x * 2 + 1 : x * 2, y0 = y * 2, y1 = y * 2 + 1 < h ? y * 2 + 1 : y * 2;
            for (int c = 0; c < 4; c++) dst[((size_t)y * nw + x) * 4 + c] = (uint8_t)((src[((size_t)y0 * w + x0) * 4 + c] + src[((size_t)y0 * w + x1) * 4 + c] +
                                                                                      src[((size_t)y1 * w + x0) * 4 + c] + src[((size_t)y1 * w + x1) * 4 + c] + 2) >> 2);
        }
        src = dst; off += (size_t)nw * nh * 4; w = nw; h = nh; e->levels++;
    }
}
static uint32_t rd32le(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static void decode_dds(Ent *e) {   /* DDS with the DX10 header, DXGI_FORMAT_BC7_UNORM (98) or _SRGB (99, read as UNORM) */
    FILE *f = fopen(e->path, "rb"); long n = -1; uint8_t *b = 0;
    if (f && !fseek(f, 0, SEEK_END) && (n = ftell(f)) > 148 && !fseek(f, 0, SEEK_SET) && (b = malloc((size_t)n)) && fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); b = 0; }
    if (f) fclose(f);
    const uint32_t fmt = b ? rd32le(b + 128) : 0;
    if (!b || memcmp(b, "DDS ", 4) || memcmp(b + 84, "DX10", 4) || (fmt != 98 && fmt != 99)) { L("HD textures: %s is not a BC7 .dds\n", e->path); free(b); atomic_store(&e->state, -1); return; }
    const int h = (int)rd32le(b + 12), w = (int)rd32le(b + 16); int lv = (int)rd32le(b + 28);
    if (w < 1 || h < 1 || w > 16384 || h > 16384) { L("HD textures: %s has a bad size\n", e->path); free(b); atomic_store(&e->state, -1); return; }
    {   /* at most the full chain, log2(max(w, h)) + 1 levels: a file claiming more (up to 2^32) looped that long here and
           asked Vulkan for an impossible image (the pre-launch security audit) */
        int full = 1; for (int m = w > h ? w : h; m > 1; m >>= 1) full++;
        if (lv < 1) lv = 1; if (lv > full) lv = full;
    }
    size_t need = 0; int l = 0;
    for (int lw = w, lh = h; l < lv; l++, lw = lw > 1 ? lw / 2 : 1, lh = lh > 1 ? lh / 2 : 1) need += (size_t)((lw + 3) / 4) * ((lh + 3) / 4) * 16;
    if (need > (size_t)n - 148) { L("HD textures: %s is too short\n", e->path); free(b); atomic_store(&e->state, -1); return; }
    memmove(b, b + 148, need);
    e->w = w; e->h = h; e->levels = lv; e->pix = b;
    atomic_store(&e->state, 3);
}
static void decode(Ent *e) {
    if (e->bc7) { decode_dds(e); return; }
    int w, h, n; uint8_t *p = stbi_load(e->path, &w, &h, &n, 4);
    if (!p) { L("HD textures: cannot read %s\n", e->path); atomic_store(&e->state, -1); return; }
    size_t total = 0; for (int lw = w, lh = h; ; lw = lw > 1 ? lw / 2 : 1, lh = lh > 1 ? lh / 2 : 1) { total += (size_t)lw * lh * 4; if (lw == 1 && lh == 1) break; }
    uint8_t *all = malloc(total);
    if (!all) { stbi_image_free(p); atomic_store(&e->state, -1); return; }
    memcpy(all, p, (size_t)w * h * 4); stbi_image_free(p);
    e->w = w; e->h = h; mips(e, all); e->pix = all;
    atomic_store(&e->state, 3);
}
static void *worker(void *arg) {
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&mx);
        while (qhead == qn) pthread_cond_wait(&cv_work, &mx);
        const int i = queue_[qhead++];
        pthread_mutex_unlock(&mx);
        Ent *e = &ents[i]; int st = 1;
        if (atomic_compare_exchange_strong(&e->state, &st, 2)) decode(e);
        pthread_mutex_lock(&mx); pthread_cond_broadcast(&cv_done); pthread_mutex_unlock(&mx);
    }
    return 0;
}
static void want(Ent *e) {   /* queue for decoding (once) */
    int st = 0;
    if (!atomic_compare_exchange_strong(&e->state, &st, 1)) return;
    pthread_mutex_lock(&mx);
    queue_[qn++] = (int)(e - ents);
    pthread_cond_signal(&cv_work);
    pthread_mutex_unlock(&mx);
}

/* ------------------------------------------------------------------ set-up */
static int parse_name(const char *s, uint32_t *img, uint32_t *clut, int *csa, int *bc7) {   /* xxxxxxxx_yyyyyyyy[_cN].png or .dds */
    unsigned a, b; int c = 0, n = 0;
    if (sscanf(s, "%8x_%8x%n", &a, &b, &n) != 2 || n != 17) return 0;
    if (s[n] == '_' && s[n + 1] == 'c') { if (sscanf(s + n + 2, "%d", &c) != 1 || c < 0 || c > 31) return 0; while (s[n + 2] >= '0' && s[n + 2] <= '9') n++; n += 2; }
    if (!strcmp(s + n, ".dds") || !strcmp(s + n, ".DDS")) *bc7 = 1;
    else if (!strcmp(s + n, ".png") || !strcmp(s + n, ".PNG")) *bc7 = 0;
    else return 0;
    *img = a; *clut = b; *csa = c; return 1;
}
static void init(void) {
    on = 0;
    const char *dir = getenv("HG_HDTEX");
    if (!dir || !*dir) dir = settings()->hd_textures;
    if (!dir || !*dir) {   /* the release's folder next to bin/ (the game runs in bin/), if there is one */
        DIR *t = opendir("../textures"); if (!t) return;
        closedir(t); dir = "../textures";
    }
    if (!strcmp(dir, "0") || !strcmp(dir, "off")) return;
    if (!settings()->renderer) { L("HD textures: %s not used (needs the GPU renderer)\n", dir); return; }
    DIR *d = opendir(dir);
    if (!d) { L("HD textures: no folder %s\n", dir); return; }
    struct dirent *de;
    while ((de = readdir(d))) {
        uint32_t img, clut; int csa, bc7;
        if (!parse_name(de->d_name, &img, &clut, &csa, &bc7)) continue;
        if (nents == cap_ents) { cap_ents = cap_ents ? cap_ents * 2 : 1024; ents = realloc(ents, sizeof *ents * cap_ents); }
        Ent *e = &ents[nents++]; memset(e, 0, sizeof *e);
        e->img = img; e->clut = clut; e->csa = csa; e->bc7 = bc7; e->slot = -1;
        const size_t len = strlen(dir) + strlen(de->d_name) + 2; e->path = malloc(len); snprintf(e->path, len, "%s/%s", dir, de->d_name);
    }
    closedir(d);
    if (!nents) { L("HD textures: no replacements in %s\n", dir); return; }
    uint32_t sz = 1; while (sz < (uint32_t)nents * 2) sz <<= 1;
    tab = malloc(sizeof *tab * sz); tabmask = sz - 1; for (uint32_t i = 0; i < sz; i++) tab[i] = -1;
    for (int k = 0; k < nents; k++) {   /* (both a .png and a .dds for one id: the .dds) */
        uint32_t i = hkey(ents[k].img, ents[k].clut, ents[k].csa) & tabmask;
        while (tab[i] >= 0 && !(ents[tab[i]].img == ents[k].img && ents[tab[i]].clut == ents[k].clut && ents[tab[i]].csa == ents[k].csa)) i = (i + 1) & tabmask;
        if (tab[i] < 0 || ents[k].bc7) tab[i] = k;
    }
    queue_ = malloc(sizeof *queue_ * nents);
    int nt = 4; const char *ne = getenv("HG_HDTEX_THREADS"); if (ne && atoi(ne) > 0) nt = atoi(ne);
    for (int k = 0; k < nt; k++) { pthread_t t; pthread_create(&t, 0, worker, 0); pthread_detach(t); }
    L("HD textures: %d replacements in %s\n", nents, dir);
    on = 1;
}
int hdtex_on(void) { if (on < 0) init(); return on; }
int hdtex_wants(uint32_t psm) {   /* (uploads are followed from the first one, before the GPU is set up) */
    if (hdtex_on() <= 0) return 0;
    return psm == 0x13 || psm == 0x14 || psm == 0x1B || psm == 0x24 || psm == 0x2C;
}

/* ------------------------------------------------------------------ the GS thread */
int hdtex_available(void) { return on; }
void hdtex_image(uint32_t img) {   /* an indexed image is in local memory: decode its replacements now (any CLUT), while
                                      the game is still loading (only when upscaling: at native resolution nothing takes them) */
    if (on <= 0 || settings_gs_scale() < 2 || !settings()->hd_on) return;
    for (int i = 0; i < nents; i++) if (ents[i].img == img) want(&ents[i]);
}
static int to_gpu(Ent *e) {
    if (atomic_load(&e->state) != 3) return 0;
    e->slot = nslots;
    if (!gsvk_hd_upload(e->slot, e->w, e->h, e->levels, e->pix, e->bc7)) { atomic_store(&e->state, -1); free(e->pix); e->pix = 0; return 0; }
    nslots++; free(e->pix); e->pix = 0; atomic_store(&e->state, 4);
    return 1;
}
void hdtex_vblank(void) {
    if (on <= 0) return;
    for (int i = 0; i < nents; i++) if (atomic_load(&ents[i].state) == 3) to_gpu(&ents[i]);
}
int hdtex_get(uint32_t img, uint32_t clut, uint32_t clut16, int sub) {
    if (on <= 0) return 0;
    static int gpu = -1;
    if (gpu < 0 && !(gpu = gsvk_hd_ok())) L("HD textures: not used (the GPU cannot sample them: descriptor indexing)\n");
    if (!gpu) return 0;
    Ent *e = find(img, clut, sub);
    if (!e && clut16) e = find(img, clut16, sub);   /* a 16-entry CLUT (64 bytes, TITLE_BACK) */
    if (!e) { n_miss++; return 0; }
    if (e->bc7 && !gsvk_hd_bc7()) { static int nl; if (!nl++) L("HD textures: the GPU cannot sample BC7 (.dds): those are not used\n"); n_miss++; return 0; }
    int st = atomic_load(&e->state);
    if (st == 0) { want(e); st = 1; }
    static int wait = -1;
    if (wait < 0) { const char *w = getenv("HG_HDTEX_WAIT"); wait = w ? atoi(w) != 0 : !host_window_wanted(); }
    if ((st == 1 || st == 2) && !wait) { n_late++; return -1; }   /* (still decoding: the original for now; asked again) */
    if (st == 1 || st == 2) {   /* still decoding: wait for it rather than change the texture on screen later */
        const double t0 = host_now(); n_wait++;
        pthread_mutex_lock(&mx);
        while ((st = atomic_load(&e->state)) == 1 || st == 2) pthread_cond_wait(&cv_done, &mx);
        pthread_mutex_unlock(&mx);
        wait_ms += (host_now() - t0) * 1000;
    }
    if (st == 3) to_gpu(e);
    if (atomic_load(&e->state) != 4) return 0;
    n_hit++;
    return e->slot + 1;
}
void hdtex_stats(void) {
    if (on <= 0) return;
    L("HD textures: %d on the GPU (%.0f MB), %llu setups replaced, %llu with no replacement, %llu waits for decoding (%.0f ms), %llu draws of the original while decoding\n", nslots, gsvk_hd_mb(),
      (unsigned long long)n_hit, (unsigned long long)n_miss, (unsigned long long)n_wait, wait_ms, (unsigned long long)n_late);
}
