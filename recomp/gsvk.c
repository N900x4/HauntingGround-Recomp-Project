/* GS on the GPU (Vulkan compute): a rasterizer (vk/gs_sprite.comp) that does per pixel exactly what gs.c does.
   GS local memory lives in video memory (dvram). Work is recorded into one command buffer that stays open until a
   result is needed (gsvk_finish / gsvk_download): primitives one at a time or in batches, uploads of local memory
   from the CPU (copied into a ring at once, so later CPU changes do not leak in), downloads.
   Two uses (gs.c): HG_GSVKCHECK compares the GPU's local memory with the software GS after every primitive or batch
   (a host-cached mirror, vram, holds the GPU's memory as last read back, so only changed 4 KB chunks go up);
   HG_GSVK=1 draws with the GPU only, gs.c keeping track of which pages each side has changed. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>
#include "ee_runtime.h"
#include "hle.h"
#include "gsvk.h"
#include "settings.h"
#include "gen/gs_sprite_spv.h"
#include "gen/gs_batch_spv.h"
#include "gen/gs_batchc_spv.h"
#include <time.h>
#include <pthread.h>
#include <stdatomic.h>

static VkInstance inst; static VkPhysicalDevice phys; static VkDevice dev; static VkQueue queue; static uint32_t qfam, ufam;   /* the GS's queue family, the window's */
static VkDescriptorSetLayout dsl; static VkPipelineLayout pl; static VkPipeline pipe1, pipeb, pipebc; static VkDescriptorPool dpool; static VkDescriptorSet dset;
/* command buffers in rotation: one is recorded while earlier ones may still run (gsvk_submit); gsvk_finish waits for all.
   A buffer is submitted every few batches (HG_GSVK_EAGER, default 2) so that the GPU works while the GS thread records
   the rest; otherwise it would sit idle until the next read-back and the GS thread would then wait for all of it. */
enum { NCMD = 256 };   /* 32 until 0.2: a frame's interpolation replay is about 45 submissions, so the GS thread waited for the GPU at every one (HG_GSVK_NCMD: fewer in rotation) */
static int ncmd_use = NCMD;
static int eager = -1, nb_open;
static VkCommandPool cpool; static VkCommandBuffer cmds[NCMD]; static VkFence fences[NCMD]; static int busy[NCMD]; static uint64_t sub_id[NCMD], n_sub_id; static int ci;
#define cmd (cmds[ci])
static VkQueryPool qpool; static float tsperiod;
/* GPU time: a timestamp opens and closes each command buffer. With HG_GSVK_TIMING=1 one more is written wherever the
   kind of work changes (phase()), and the time between two is counted for the phase in between, separately for the
   replays of frame interpolation (target set != 0); the copies dispatch then waits for the exact one, which it
   otherwise overlaps, so the total grows a little. */
enum { QPER = 1024, PH_EXACT = 0, PH_COPIES, PH_SINGLE, PH_COMPOSE, PH_XFER, PH_N };
static int tim_on, cur_ph; static int qcnt[NCMD]; static int8_t qseg[NCMD][QPER]; static double ph_ms[2 * PH_N];
/* gsvk_tag(): command buffers opened while the tag is on count their GPU time separately too (frame interpolation's
   replays and snapshot, for the adaptive policy in gs.c) */
static int tag_cur, tag_of[NCMD]; static double gpu_ms_tag;
typedef struct { VkBuffer b; VkDeviceMemory m; void *p; VkDeviceSize n; } Buf;
/* Every write into a ring (video memory mapped over the bus, write-combined) goes through the C library's memcpy, never
   one the compiler expands inline: gcc 13 (the Linux release's) turns the fixed-size copies (a primitive, a CLUT) into
   `rep movsq`, several times slower into such memory on the user's Ryzen. The interpolation replays then took 4.4 ms a
   frame instead of 1.6 and the hallway (Debilitas' entrance) stuttered in the release but not in the development build
   (gcc 16 calls memcpy). Through a volatile pointer no compiler can expand it. */
static void *(*volatile ring_put)(void *, const void *, size_t) = memcpy;
static Buf vram, dvram, xoffb, tabb, primb, clutb, upb, hrb, binb, auxb;   /* auxb: masked uploads (word, value, mask) */   /* hrb: the upscaled display read back; binb: bins of the copies' batches */
static VkDeviceSize outbase, outbytes;   /* in dvram, after the copies */
enum { PRIM_MAX = 65536, CLUT_MAX = 8192, UP_BYTES = 64 << 20, AUX_MAX = 4 << 20, BIN_MAX = 8 << 20, BIN = 16, BIN_MIN = 48 };
/* The primitive, CLUT and upload rings are used in two halves: when the current half is full the work recorded so far
   is submitted and recording goes on in the other half once the GPU is done with that half's last submission (a full
   wait, as before, left the GPU idle while the GS thread recorded the next work). */
static int rh, clut_ok; static uint64_t half_id[2];
static int state;   /* 0 untried, 1 ready, -1 unavailable */
/* presenting from video memory (the end of this file): the UI thread's queue (the family's second, or the same one
   under qmx), the timeline semaphore every GS submission signals with its id */
static VkQueue uqueue; static int shared_q, can_present; static pthread_mutex_t qmx = PTHREAD_MUTEX_INITIALIZER; static VkSemaphore tl;
static int can_wait, ui_prio; static PFN_vkWaitForPresentKHR wait_present;
static int have_caps2, can_ptim, can_prel, sc_ptim, sc_prel; static uint64_t pt_dom; static int pt_qpc;   /* (can_prel, sc_prel: presents at a time relative to the previous one) */
static PFN_vkGetSwapchainTimingPropertiesEXT pt_props;
/* HG_PACEDBG: the window's blits timed on the GPU (calibrated to the host clock): when they started and ended */
static int use_fse; static VkResult (VKAPI_PTR *fse_acquire)(VkDevice, VkSwapchainKHR); static void *fse_mon; static _Atomic int fse_want, fse_have;
void gsvk_present_fullscreen(int on) { atomic_store(&fse_want, on); }   /* any thread: the window is full screen (HG_FSE) */
static int use_cal; static PFN_vkGetCalibratedTimestampsKHR get_cal; static VkQueryPool uqp; static double cal_off, cal_t;
static struct { uint64_t pid; double sub, beg, end; } ugt[64]; static uint64_t upid[8];   /* VK_EXT_present_timing: on, on this swapchain, its QPC time domain */
static PFN_vkSetSwapchainPresentTimingQueueSizeEXT pt_qsize; static PFN_vkGetSwapchainTimeDomainPropertiesEXT pt_domains; static PFN_vkGetPastPresentationTimingEXT pt_past;
static struct { uint64_t pid; double t; } pt_ring[256]; static int pt_n;   /* scanout times collected, not yet taken (gsvk_present_scanout) */   /* VK_KHR_present_wait (gsvk_present_wait) */
static int skip_exact;   /* gsvk_skip_exact() */
/* HD texture replacements (hdtex.c): images sampled by the upscaling copies (binding 7, an array of HD_MAX). Every
   slot first holds a 1 x 1 image; a replacement's slot is written once (update after bind: the descriptor set stays
   bound in command buffers that are pending). Uploaded in the GS's stream, so it is there before the draws after it;
   its mipmaps come from the CPU (the GS's queue may be compute-only: no blits). */
enum { HD_MAX = 4096 };
static int hd_ok, hd_bc7; static VkSampler hd_samp; static VkImage hd_img[HD_MAX + 1]; static VkImageView hd_view[HD_MAX + 1]; static VkDeviceMemory hd_mem[HD_MAX + 1];
static double hd_bytes;
static struct { Buf b; uint64_t id; } hd_stage[256]; static int hd_nstage;
static void imgbar(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to, VkAccessFlags sa, VkAccessFlags da, VkPipelineStageFlags ss, VkPipelineStageFlags ds);
static uint32_t memtype(uint32_t bits, VkMemoryPropertyFlags want);
/* recording */
static int open_, bound; static VkPipeline cur_pipe; static uint32_t nprim, nclut, upoff;
static uint32_t bfirst, bn; static int bx0, by0, bx1, by1;   /* the batch being collected */
/* Bins (the copies of big batches): for 16 x 16 pixel squares of the batch's area, which of its primitives come near
   (a bit each), so that a tile of the copies dispatch tests only those instead of every primitive (most of the time
   of a batch of a thousand small triangles went into that scan). Made on the CPU from the rectangles gsvk_prim()
   kept (the ring is never read back), in a ring like the primitives'. */
static uint32_t naux; static int16_t (*brect)[4]; static uint32_t nbin, *bscratch; static size_t bscratch_n; static uint64_t n_binned;
/* HG_GSVK_DPROF=1 (with HG_GSVK_TIMING): GPU time of every copies dispatch, summed by the kind of batch (sprites only,
   triangles only, both) and the number of primitives, printed at the end (gsvk_dprof_print) */
static int dprof = -1; static uint32_t bspr, btri; static int32_t qinfo[NCMD][QPER];
enum { DP_K = 3, DP_B = 6 };   /* kinds; size classes 1, 2-4, 5-16, 17-64, 65-256, more */
static struct { uint64_t n; double ms, mpix, prims; } dp[2][DP_K][DP_B];   /* [replay?][kind][size] */
static int dp_cls(uint32_t n) { return n <= 1 ? 0 : n <= 4 ? 1 : n <= 16 ? 2 : n <= 64 ? 3 : n <= 256 ? 4 : 5; }
static struct { uint8_t r, k, c; uint32_t n; double mpix; } dpi[1 << 16]; static uint32_t dpi_n;
static int vfresh = 1;   /* the mirror does not hold the GPU's memory */
static uint32_t ncopy = 1;   /* copies of local memory in dvram, 4 MB apart */
static uint32_t mbase_add, up_first, up_count = 1;   /* gsvk_set_target() */
static uint64_t n_submits, n_dispatch; static double gpu_ms_all, wait_ms_all;
/* where the waits were (gsvk_wait_sites): a command buffer to record into, a ring half, everything (gsvk_finish), gs.c's gsvk_wait */
static double wait_site_ms[4]; static int wsite = 3;

/* mode: 0 host coherent, 1 + cached, 2 device local, 3 device local and host visible (else host coherent) */
static int mkbuf_(Buf *b, VkDeviceSize n, VkBufferUsageFlags use, int mode) {
    VkBufferCreateInfo bi = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = n, .usage = use, .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
    if (vkCreateBuffer(dev, &bi, 0, &b->b)) return 0;
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(dev, b->b, &mr);
    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(phys, &mp);
    const VkMemoryPropertyFlags hv = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    VkMemoryPropertyFlags want[2] = { mode == 2 ? VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT : mode == 3 ? hv | VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT :
                                      hv | (mode == 1 ? VK_MEMORY_PROPERTY_HOST_CACHED_BIT : 0), hv };
    uint32_t t = UINT32_MAX;
    for (int k = 0; k < (mode == 3 ? 2 : 1) && t == UINT32_MAX; k++)
        for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
            if ((mr.memoryTypeBits >> i & 1) && (mp.memoryTypes[i].propertyFlags & want[k]) == want[k]) { t = i; break; }
    if (t == UINT32_MAX) return 0;
    VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = t };
    if (vkAllocateMemory(dev, &ai, 0, &b->m) || vkBindBufferMemory(dev, b->b, b->m, 0)) return 0;
    if (mode != 2 && vkMapMemory(dev, b->m, 0, VK_WHOLE_SIZE, 0, &b->p)) return 0;
    b->n = n; return 1;
}
static int mkbuf(Buf *b, VkDeviceSize n, VkBufferUsageFlags use) { return mkbuf_(b, n, use, 0); }

/* local memory: ncopies copies of 4 MB, then the out region (6 picture slots), and its host-visible read-back hrb */
static int mk_local(int ncopies) {
    const VkBufferUsageFlags xfer = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, sto = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    ncopy = ncopies < 1 ? 1 : (uint32_t)ncopies; up_first = 0; up_count = ncopy; mbase_add = 0;
    outbase = (VkDeviceSize)ncopy * (1u << 22) + 16;
    outbytes = ncopy > 1 ? (VkDeviceSize)ncopy * 8 * 1024 * 1024 : (VkDeviceSize)32 << 20;
    if (!mkbuf_(&dvram, outbase + outbytes, sto | xfer, 2)) return 0;
    if (!mkbuf_(&hrb, outbytes, xfer, 1) && !mkbuf(&hrb, outbytes, xfer)) return 0;
    return 1;
}
static void free_buf(Buf *b) {
    if (b->b) vkDestroyBuffer(dev, b->b, 0);
    if (b->m) vkFreeMemory(dev, b->m, 0);   /* (unmaps it) */
    memset(b, 0, sizeof *b);
}
static _Atomic int ready = 0;   /* gsvk_ready(): state as other threads may see it, once gsvk_init() is over */
static int init_(const uint32_t *xoff32, const uint32_t *xoff32z, const int32_t *tables, int ncopies);
int gsvk_init(const uint32_t *xoff32, const uint32_t *xoff32z, const int32_t *tables, int ncopies) {
    if (state) return state > 0;
    const int r = init_(xoff32, xoff32z, tables, ncopies);
    atomic_store(&ready, r && state > 0 ? 1 : -1);
    return r;
}
static int init_(const uint32_t *xoff32, const uint32_t *xoff32z, const int32_t *tables, int ncopies) {
    state = -1;
    VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "hgboot", .apiVersion = VK_API_VERSION_1_2 };
    /* for the window (gsvk_present_init): the surface, the window system's surface (Linux: whichever SDL may use) and
       the surface capabilities 2 (present timing), those the loader has */
#ifdef _WIN32
    static const char *sext[] = { "VK_KHR_surface", "VK_KHR_get_surface_capabilities2", "VK_KHR_win32_surface" };
#else
    static const char *sext[] = { "VK_KHR_surface", "VK_KHR_get_surface_capabilities2", "VK_KHR_wayland_surface", "VK_KHR_xcb_surface", "VK_KHR_xlib_surface" };
#endif
    const int nsext = (int)(sizeof sext / sizeof *sext);
    const char *iext[8]; uint32_t niext = 0;
    { uint32_t ne = 0; vkEnumerateInstanceExtensionProperties(0, &ne, 0); VkExtensionProperties *ep = calloc(ne + 1, sizeof *ep);
      if (ep) {
          vkEnumerateInstanceExtensionProperties(0, &ne, ep); int h = 0;
          for (uint32_t i = 0; i < ne; i++) for (int w = 0; w < nsext; w++) if (!strcmp(ep[i].extensionName, sext[w])) h |= 1 << w;
          if ((h & 1) && (h >> 2)) {   /* a surface on some window system: niext >= 2 */
              for (int w = 0; w < nsext; w++) if (h >> w & 1) iext[niext++] = sext[w];
              have_caps2 = h >> 1 & 1;
          }
          free(ep);
      } }
    VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app, .enabledExtensionCount = niext, .ppEnabledExtensionNames = iext };
    if (vkCreateInstance(&ici, 0, &inst)) { L("GSVK: no Vulkan instance\n"); return 0; }
    uint32_t n = 0; vkEnumeratePhysicalDevices(inst, &n, 0);
    VkPhysicalDevice devs[8]; if (n > 8) n = 8; vkEnumeratePhysicalDevices(inst, &n, devs);
    for (uint32_t i = 0; i < n && !phys; i++) { VkPhysicalDeviceProperties pr; vkGetPhysicalDeviceProperties(devs[i], &pr); if (pr.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) phys = devs[i]; }
    if (!phys && n) phys = devs[0];
    if (!phys) { L("GSVK: no GPU\n"); return 0; }
    VkPhysicalDeviceProperties pr; vkGetPhysicalDeviceProperties(phys, &pr);
    uint32_t nq = 0; vkGetPhysicalDeviceQueueFamilyProperties(phys, &nq, 0);
    VkQueueFamilyProperties qf[16]; if (nq > 16) nq = 16; vkGetPhysicalDeviceQueueFamilyProperties(phys, &nq, qf);
    qfam = UINT32_MAX; for (uint32_t i = 0; i < nq; i++) if (qf[i].queueFlags & VK_QUEUE_COMPUTE_BIT) { qfam = i; break; }
    if (qfam == UINT32_MAX) { L("GSVK: no compute queue\n"); return 0; }
    ufam = qfam;
    /* The window's blits need a graphics queue; the GS's work goes to a compute-only family when there is one (the
       "async compute" queues), so the window's few blits at each refresh are not held up behind long GS dispatches
       (with both on the graphics family a present missed its refresh about once in 300 at x4 + 60 fps). HG_GSVK_QUEUE=
       graphics keeps both on the graphics family. */
    if (qf[qfam].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
        const char *qe = getenv("HG_GSVK_QUEUE");
        for (uint32_t i = 0; i < nq && !(qe && !strcmp(qe, "graphics")); i++)
            if ((qf[i].queueFlags & VK_QUEUE_COMPUTE_BIT) && !(qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && qf[i].timestampValidBits >= 36) { qfam = i; break; }
    }
    /* presenting needs the swapchain extension and timeline semaphores (Vulkan 1.2) */
    VkPhysicalDeviceTimelineSemaphoreFeatures tsf = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES };
    VkPhysicalDeviceFeatures2 f2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &tsf }; vkGetPhysicalDeviceFeatures2(phys, &f2);
    int has_pid = 0, has_pwait = 0, has_gprio = 0, has_ptim = 0, has_cal = 0, has_fse = 0, has_pexe = 0;
    { uint32_t ne = 0; vkEnumerateDeviceExtensionProperties(phys, 0, &ne, 0); VkExtensionProperties *ep = calloc(ne + 1, sizeof *ep); int sw = 0;
      if (ep) { vkEnumerateDeviceExtensionProperties(phys, 0, &ne, ep);
                for (uint32_t i = 0; i < ne; i++) { sw |= !strcmp(ep[i].extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME);
                    has_pid |= !strcmp(ep[i].extensionName, "VK_KHR_present_id"); has_pwait |= !strcmp(ep[i].extensionName, "VK_KHR_present_wait");
                    has_gprio |= !strcmp(ep[i].extensionName, "VK_EXT_global_priority");
                    has_pexe |= !strcmp(ep[i].extensionName, "VK_KHR_pipeline_executable_properties");
                    has_ptim |= !strcmp(ep[i].extensionName, "VK_EXT_present_timing");
                    has_cal |= !strcmp(ep[i].extensionName, "VK_KHR_calibrated_timestamps");
                    has_fse |= !strcmp(ep[i].extensionName, "VK_EXT_full_screen_exclusive"); }
                free(ep); }
      can_present = niext >= 2 && sw && tsf.timelineSemaphore && pr.apiVersion >= VK_API_VERSION_1_2; }
    /* the display's timing (display-locked pacing, host_sdl.c): VK_KHR_present_wait tells when each present reached the
       screen; without it the swapchain's own blocking (FIFO) still paces the window */
    VkPhysicalDevicePresentTimingFeaturesEXT ptf = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_TIMING_FEATURES_EXT };
    VkPhysicalDevicePresentWaitFeaturesKHR pwf = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR, .pNext = has_ptim ? &ptf : 0 };
    VkPhysicalDevicePresentIdFeaturesKHR pif = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR, .pNext = &pwf };
    if (can_present && has_pid && has_pwait && !(getenv("HG_PRESENTWAIT") && !atoi(getenv("HG_PRESENTWAIT")))) {
        VkPhysicalDeviceFeatures2 g2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &pif }; vkGetPhysicalDeviceFeatures2(phys, &g2);
        can_wait = pif.presentId && pwf.presentWait;
        /* VK_EXT_present_timing: when each present's first pixel went out to the screen (the true cadence, HG_PRESENTLOG) */
        can_ptim = can_wait && has_ptim && ptf.presentTiming && have_caps2 && !(getenv("HG_PRESENTTIMING") && !atoi(getenv("HG_PRESENTTIMING")));
    }
    const float prio[2] = { 0.5f, 1.0f };   /* the window's few blits go before the GS's work */
    VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = qfam, .queueCount = can_present && qf[qfam].queueCount >= 2 ? 2 : 1, .pQueuePriorities = prio };
    VkDeviceQueueCreateInfo qcis[2] = { qci, { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = ufam, .queueCount = 1, .pQueuePriorities = &prio[1] } };
    if (ufam != qfam) { if (!can_present) ufam = qfam; else qcis[0].queueCount = 1; }
    VkPhysicalDeviceFeatures have, want = { 0 }; vkGetPhysicalDeviceFeatures(phys, &have);
    if (!have.shaderFloat64) { L("GSVK: no 64-bit floats in shaders (triangle Z)\n"); return 0; }
    want.shaderFloat64 = VK_TRUE;
    VkPhysicalDeviceDescriptorIndexingFeatures dif = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES };
    { VkPhysicalDeviceFeatures2 h2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &dif }; vkGetPhysicalDeviceFeatures2(phys, &h2); }
    hd_ok = pr.apiVersion >= VK_API_VERSION_1_2 && dif.shaderSampledImageArrayNonUniformIndexing && dif.descriptorBindingSampledImageUpdateAfterBind;
    VkPhysicalDeviceDescriptorIndexingFeatures difw = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES,
        .shaderSampledImageArrayNonUniformIndexing = VK_TRUE, .descriptorBindingSampledImageUpdateAfterBind = VK_TRUE };
    want.samplerAnisotropy = have.samplerAnisotropy;
    {   /* HD textures as BC7 (hdtex.c, .dds): a quarter of RGBA8's memory */
        VkFormatProperties fp; vkGetPhysicalDeviceFormatProperties(phys, VK_FORMAT_BC7_UNORM_BLOCK, &fp);
        hd_bc7 = have.textureCompressionBC && (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT);
        want.textureCompressionBC = hd_bc7 ? VK_TRUE : VK_FALSE;
    }
    VkPhysicalDeviceTimelineSemaphoreFeatures tsw = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES, .timelineSemaphore = VK_TRUE };
    const char *dext[8] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME, "VK_KHR_present_id", "VK_KHR_present_wait", "VK_EXT_present_timing", "VK_EXT_global_priority", "VK_KHR_calibrated_timestamps", "VK_EXT_full_screen_exclusive" };
    if (can_wait) { pif.presentId = VK_TRUE; pwf.presentWait = VK_TRUE; pwf.pNext = 0; tsw.pNext = &pif; }
    if (can_ptim) { can_prel = ptf.presentAtRelativeTime && !(getenv("HG_PRESENTREL") && !atoi(getenv("HG_PRESENTREL"))); ptf.presentAtAbsoluteTime = VK_FALSE; ptf.presentAtRelativeTime = can_prel ? VK_TRUE : VK_FALSE; ptf.pNext = 0; pwf.pNext = &ptf; }
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = can_present ? &tsw : 0, .queueCreateInfoCount = ufam != qfam ? 2 : 1, .pQueueCreateInfos = qcis,
                               .enabledExtensionCount = can_present ? (can_wait ? (can_ptim ? 4 : 3) : 1) : 0, .ppEnabledExtensionNames = dext, .pEnabledFeatures = &want };
    /* the GS's queue at a low global priority (VK_EXT_global_priority; lowering needs no privileges), so the system's
       GPU scheduler runs the window's blits ahead of the GS's long dispatches (HG_GSVK_UIPRIO=0: not) */
    VkDeviceQueueGlobalPriorityCreateInfoKHR gp = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_GLOBAL_PRIORITY_CREATE_INFO_KHR, .globalPriority = VK_QUEUE_GLOBAL_PRIORITY_LOW_KHR };
    const char *upe = getenv("HG_GSVK_UIPRIO");
    if (has_gprio && can_present && ufam != qfam && !(upe && !atoi(upe))) {
        dext[dci.enabledExtensionCount] = "VK_EXT_global_priority"; dci.enabledExtensionCount++;
        qcis[0].pNext = &gp; ui_prio = 1;
    }
    /* HG_FSE=1 (a test): full-screen exclusive mode when the window is full screen (VK_EXT_full_screen_exclusive) */
    if (has_fse && can_present && have_caps2 && getenv("HG_FSE") && atoi(getenv("HG_FSE"))) { dext[dci.enabledExtensionCount] = "VK_EXT_full_screen_exclusive"; dci.enabledExtensionCount++; use_fse = 1; }
    if (has_cal && can_present && getenv("HG_PACEDBG")) { dext[dci.enabledExtensionCount] = "VK_KHR_calibrated_timestamps"; dci.enabledExtensionCount++; use_cal = 1; }
    if (hd_ok) { difw.pNext = (void *)dci.pNext; dci.pNext = &difw; }
    /* HG_GSVK_PSTATS=1: the driver's statistics of the three pipelines (registers, spills, ...; VK_KHR_pipeline_executable_properties) */
    VkPhysicalDevicePipelineExecutablePropertiesFeaturesKHR pexf = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_EXECUTABLE_PROPERTIES_FEATURES_KHR, .pipelineExecutableInfo = VK_TRUE };
    const int pstats = has_pexe && getenv("HG_GSVK_PSTATS") && atoi(getenv("HG_GSVK_PSTATS"));
    if (pstats) { dext[dci.enabledExtensionCount] = "VK_KHR_pipeline_executable_properties"; dci.enabledExtensionCount++; pexf.pNext = (void *)dci.pNext; dci.pNext = &pexf; }
    if (vkCreateDevice(phys, &dci, 0, &dev)) { L("GSVK: no device\n"); return 0; }
    if (use_cal && !(get_cal = (PFN_vkGetCalibratedTimestampsKHR)vkGetDeviceProcAddr(dev, "vkGetCalibratedTimestampsKHR"))) use_cal = 0;
    if (use_fse && !(fse_acquire = (VkResult (VKAPI_PTR *)(VkDevice, VkSwapchainKHR))vkGetDeviceProcAddr(dev, "vkAcquireFullScreenExclusiveModeEXT"))) use_fse = 0;
    if (can_wait && !(wait_present = (PFN_vkWaitForPresentKHR)vkGetDeviceProcAddr(dev, "vkWaitForPresentKHR"))) can_wait = 0;
    if (can_ptim) {
        pt_qsize = (PFN_vkSetSwapchainPresentTimingQueueSizeEXT)vkGetDeviceProcAddr(dev, "vkSetSwapchainPresentTimingQueueSizeEXT");
        pt_domains = (PFN_vkGetSwapchainTimeDomainPropertiesEXT)vkGetDeviceProcAddr(dev, "vkGetSwapchainTimeDomainPropertiesEXT");
        pt_past = (PFN_vkGetPastPresentationTimingEXT)vkGetDeviceProcAddr(dev, "vkGetPastPresentationTimingEXT");
        pt_props = (PFN_vkGetSwapchainTimingPropertiesEXT)vkGetDeviceProcAddr(dev, "vkGetSwapchainTimingPropertiesEXT");
        if (!pt_qsize || !pt_domains || !pt_past) can_ptim = 0;
    }
    vkGetDeviceQueue(dev, qfam, 0, &queue);
    if (ufam != qfam) vkGetDeviceQueue(dev, ufam, 0, &uqueue);
    else if (qcis[0].queueCount == 2) vkGetDeviceQueue(dev, qfam, 1, &uqueue); else { uqueue = queue; shared_q = 1; }
    if (can_present) {
        VkSemaphoreTypeCreateInfo sti = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO, .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE };
        VkSemaphoreCreateInfo sci = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &sti };
        if (vkCreateSemaphore(dev, &sci, 0, &tl)) can_present = 0;
    }
    const VkBufferUsageFlags xfer = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, sto = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    const VkDeviceSize vbytes = ((1u << 20) + 4) * 4;
    if (!mkbuf_(&vram, vbytes, xfer, 1)) { vkDestroyBuffer(dev, vram.b, 0); vram.b = 0; L("GSVK: no host-cached memory, mirror uncached\n"); }
    if ((!vram.p && !mkbuf(&vram, vbytes, xfer)) || !mk_local(ncopies) || !mkbuf(&xoffb, 66u * 2048 * 4, sto) ||
        !mkbuf(&tabb, GSVK_TABLES * 4, sto) || !mkbuf_(&primb, (VkDeviceSize)PRIM_MAX * sizeof(GsVkSprite), sto, 3) ||
        !mkbuf_(&clutb, (VkDeviceSize)CLUT_MAX * 1024, sto, 3) || !mkbuf(&upb, UP_BYTES, xfer) || !mkbuf_(&binb, (VkDeviceSize)BIN_MAX * 4, sto, 3) || !mkbuf_(&auxb, (VkDeviceSize)AUX_MAX * 4, sto, 3)) { L("GSVK: buffers failed\n"); return 0; }
    memcpy(xoffb.p, xoff32, 33u * 2048 * 4); memcpy((uint32_t *)xoffb.p + 33u * 2048, xoff32z, 33u * 2048 * 4); memcpy(tabb.p, tables, GSVK_TABLES * 4);
    /* one layout for both pipelines: 0 local memory, 1 32-bit address tables, 2 CLUTs, 3 primitives, 4 other tables;
       push constants: n, bx0, by0, first (batch) or the primitive's index (one primitive) */
    VkDescriptorSetLayoutBinding bd[8];
    for (int i = 0; i < 7; i++) bd[i] = (VkDescriptorSetLayoutBinding){ .binding = (uint32_t)i, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                                                         .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT };
    bd[7] = (VkDescriptorSetLayoutBinding){ .binding = 7, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = HD_MAX, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT };
    VkDescriptorBindingFlags bfl[8] = { 0 }; bfl[7] = hd_ok ? VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT : 0;
    VkDescriptorSetLayoutBindingFlagsCreateInfo bfi = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO, .bindingCount = 8, .pBindingFlags = bfl };
    VkDescriptorSetLayoutCreateInfo dli = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .pNext = &bfi, .bindingCount = 8, .pBindings = bd,
                                            .flags = hd_ok ? VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT : 0 };
    VkPushConstantRange pcr = { VK_SHADER_STAGE_COMPUTE_BIT, 0, 32 };
    VkPipelineLayoutCreateInfo pli = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &dsl, .pushConstantRangeCount = 1, .pPushConstantRanges = &pcr };
    if (vkCreateDescriptorSetLayout(dev, &dli, 0, &dsl) || vkCreatePipelineLayout(dev, &pli, 0, &pl)) { L("GSVK: pipeline layout failed\n"); return 0; }
    for (int k = 0; k < 3; k++) {   /* one primitive; a batch; a batch's upscaling copies */
        VkShaderModuleCreateInfo smi = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = k == 2 ? sizeof gs_batchc_spv : k ? sizeof gs_batch_spv : sizeof gs_sprite_spv,
                                         .pCode = k == 2 ? gs_batchc_spv : k ? gs_batch_spv : gs_sprite_spv };
        VkShaderModule sm;
        if (vkCreateShaderModule(dev, &smi, 0, &sm)) { L("GSVK: shader failed\n"); return 0; }
        VkComputePipelineCreateInfo cpi = { .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, .layout = pl, .flags = pstats ? VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR : 0,
            .stage = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = sm, .pName = "main" } };
        if (vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpi, 0, k == 2 ? &pipebc : k ? &pipeb : &pipe1)) { L("GSVK: pipeline failed\n"); return 0; }
        if (pstats) {
            PFN_vkGetPipelineExecutablePropertiesKHR gpe = (PFN_vkGetPipelineExecutablePropertiesKHR)vkGetDeviceProcAddr(dev, "vkGetPipelineExecutablePropertiesKHR");
            PFN_vkGetPipelineExecutableStatisticsKHR gps = (PFN_vkGetPipelineExecutableStatisticsKHR)vkGetDeviceProcAddr(dev, "vkGetPipelineExecutableStatisticsKHR");
            VkPipelineInfoKHR pinf = { .sType = VK_STRUCTURE_TYPE_PIPELINE_INFO_KHR, .pipeline = k == 2 ? pipebc : k ? pipeb : pipe1 };
            uint32_t ne = 0; if (gpe && gps && !gpe(dev, &pinf, &ne, 0)) for (uint32_t e = 0; e < ne; e++) {
                VkPipelineExecutableInfoKHR ei = { .sType = VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_INFO_KHR, .pipeline = pinf.pipeline, .executableIndex = e };
                VkPipelineExecutableStatisticKHR st[64]; uint32_t ns = 64;
                for (uint32_t i = 0; i < 64; i++) st[i] = (VkPipelineExecutableStatisticKHR){ .sType = VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_STATISTIC_KHR };
                if (gps(dev, &ei, &ns, st) < 0) continue;
                for (uint32_t i = 0; i < ns; i++) {
                    const VkPipelineExecutableStatisticKHR *t = &st[i];
                    if (t->format == VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_UINT64_KHR) L("GSVK pstats %s: %s = %llu\n", k == 2 ? "copies" : k ? "batch" : "single", t->name, (unsigned long long)t->value.u64);
                    else if (t->format == VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_INT64_KHR) L("GSVK pstats %s: %s = %lld\n", k == 2 ? "copies" : k ? "batch" : "single", t->name, (long long)t->value.i64);
                    else if (t->format == VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_FLOAT64_KHR) L("GSVK pstats %s: %s = %g\n", k == 2 ? "copies" : k ? "batch" : "single", t->name, t->value.f64);
                }
            }
        }
    }
    VkDescriptorPoolSize ps[2] = { { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 7 }, { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, HD_MAX } };
    VkDescriptorPoolCreateInfo dpi = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1, .poolSizeCount = 2, .pPoolSizes = ps,
                                       .flags = hd_ok ? VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT : 0 };
    VkDescriptorSetAllocateInfo dai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorSetCount = 1, .pSetLayouts = &dsl };
    if (vkCreateDescriptorPool(dev, &dpi, 0, &dpool)) return 0;
    dai.descriptorPool = dpool; if (vkAllocateDescriptorSets(dev, &dai, &dset)) return 0;
    VkDescriptorBufferInfo bi[7] = { { dvram.b, 0, VK_WHOLE_SIZE }, { xoffb.b, 0, VK_WHOLE_SIZE }, { clutb.b, 0, VK_WHOLE_SIZE }, { primb.b, 0, VK_WHOLE_SIZE }, { tabb.b, 0, VK_WHOLE_SIZE }, { binb.b, 0, VK_WHOLE_SIZE }, { auxb.b, 0, VK_WHOLE_SIZE } };
    VkWriteDescriptorSet wr[7];
    for (int i = 0; i < 7; i++) wr[i] = (VkWriteDescriptorSet){ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = dset, .dstBinding = (uint32_t)i, .descriptorCount = 1,
                                                               .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &bi[i] };
    vkUpdateDescriptorSets(dev, 7, wr, 0, 0);
    {   VkSamplerCreateInfo sci = { .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_LINEAR, .minFilter = VK_FILTER_LINEAR,
            .mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR, .addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT, .addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT,
            .addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT, .anisotropyEnable = want.samplerAnisotropy, .maxAnisotropy = want.samplerAnisotropy ? 16.0f : 1.0f,
            .maxLod = VK_LOD_CLAMP_NONE };
        if (vkCreateSampler(dev, &sci, 0, &hd_samp)) return 0;
        VkImageCreateInfo ii = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = { 1, 1, 1 },
            .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_SAMPLED_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
        if (vkCreateImage(dev, &ii, 0, &hd_img[HD_MAX])) return 0;
        VkMemoryRequirements mr; vkGetImageMemoryRequirements(dev, hd_img[HD_MAX], &mr);
        VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = memtype(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) };
        if (ai.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(dev, &ai, 0, &hd_mem[HD_MAX]) || vkBindImageMemory(dev, hd_img[HD_MAX], hd_mem[HD_MAX], 0)) return 0;
        VkImageViewCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = hd_img[HD_MAX], .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
        if (vkCreateImageView(dev, &vi, 0, &hd_view[HD_MAX])) return 0;
        VkDescriptorImageInfo *ii2 = malloc(sizeof *ii2 * HD_MAX);
        for (int i = 0; i < HD_MAX; i++) ii2[i] = (VkDescriptorImageInfo){ hd_samp, hd_view[HD_MAX], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkWriteDescriptorSet w7 = { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = dset, .dstBinding = 7, .descriptorCount = HD_MAX,
                                    .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = ii2 };
        vkUpdateDescriptorSets(dev, 1, &w7, 0, 0); free(ii2);
    }
    VkCommandPoolCreateInfo cpci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = qfam };
    VkCommandBufferAllocateInfo cbai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = NCMD };
    VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    if (vkCreateCommandPool(dev, &cpci, 0, &cpool)) return 0;
    cbai.commandPool = cpool;
    if (vkAllocateCommandBuffers(dev, &cbai, cmds)) return 0;
    for (int k = 0; k < NCMD; k++) if (vkCreateFence(dev, &fci, 0, &fences[k])) return 0;
    VkQueryPoolCreateInfo qpi = { .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = QPER * NCMD };
    tim_on = getenv("HG_GSVK_TIMING") && atoi(getenv("HG_GSVK_TIMING")) > 0;
    if (vkCreateQueryPool(dev, &qpi, 0, &qpool)) return 0;
    tsperiod = pr.limits.timestampPeriod;
    {   /* the 1 x 1 image's layout, before anything uses the set */
        VkCommandBufferBeginInfo cbi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
        vkBeginCommandBuffer(cmds[0], &cbi);
        imgbar(cmds[0], hd_img[HD_MAX], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, VK_ACCESS_SHADER_READ_BIT,
               VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        vkEndCommandBuffer(cmds[0]);
        VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmds[0] };
        if (vkQueueSubmit(queue, 1, &si, fences[0]) || vkWaitForFences(dev, 1, &fences[0], VK_TRUE, UINT64_MAX)) return 0;
        vkResetFences(dev, 1, &fences[0]);
    }
    L("GSVK: Vulkan compute on %s%s%s%s\n", pr.deviceName, can_present ? (shared_q ? " (can present the window from video memory; one queue)" : " (can present the window from video memory)") : "",
      can_wait ? " (present wait: yes)" : "", ufam != qfam ? (ui_prio ? " (GS on an async compute queue; the window's at high priority)" : " (GS on an async compute queue)") : "");
    if (getenv("HG_GSVK_NCMD")) { ncmd_use = atoi(getenv("HG_GSVK_NCMD")); if (ncmd_use < 2) ncmd_use = 2; if (ncmd_use > NCMD) ncmd_use = NCMD; }
    state = 1; return 1;
}

/* ------------------------------------------------------------------ recording */
static void barrier(void) {   /* everything before (transfers, dispatches) is visible to everything after */
    VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
                           .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 1, &mb, 0, 0, 0, 0);
}
static void slot_wait(int k) {   /* the submission in command buffer k is done; its GPU time counted */
    if (!busy[k]) return;
    struct timespec t0, t1; clock_gettime(CLOCK_MONOTONIC, &t0);
    if (vkWaitForFences(dev, 1, &fences[k], VK_TRUE, 10000000000ull)) { L("GSVK: wait failed\n"); state = -1; }
    clock_gettime(CLOCK_MONOTONIC, &t1); { const double ms = (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6; wait_ms_all += ms; wait_site_ms[wsite] += ms; }
    static uint64_t ts[QPER]; const int nq = qcnt[k];
    if (nq >= 2 && !vkGetQueryPoolResults(dev, qpool, QPER * k, (uint32_t)nq, sizeof ts, ts, 8, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT)) {
        gpu_ms_all += (double)(ts[nq - 1] - ts[0]) * tsperiod / 1e6;
        if (tag_of[k]) gpu_ms_tag += (double)(ts[nq - 1] - ts[0]) * tsperiod / 1e6;
        for (int i = 0; i + 1 < nq; i++) if (qseg[k][i] >= 0) {
            const double ms = (double)(ts[i + 1] - ts[i]) * tsperiod / 1e6;
            ph_ms[qseg[k][i]] += ms;
            if (dprof > 0 && qinfo[k][i] >= 0) { const uint32_t j = (uint32_t)qinfo[k][i]; dp[dpi[j].r][dpi[j].k][dpi[j].c].n++; dp[dpi[j].r][dpi[j].k][dpi[j].c].ms += ms; dp[dpi[j].r][dpi[j].k][dpi[j].c].mpix += dpi[j].mpix; dp[dpi[j].r][dpi[j].k][dpi[j].c].prims += dpi[j].n; }
        }
    }
    busy[k] = 0;
}
static void rec_open(void) {
    if (open_) return;
    ci = (ci + 1) % ncmd_use; { const int w = wsite; wsite = 0; slot_wait(ci); wsite = w; }
    VkCommandBufferBeginInfo cbi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    vkResetCommandBuffer(cmd, 0); vkBeginCommandBuffer(cmd, &cbi);
    vkCmdResetQueryPool(cmd, qpool, QPER * ci, tim_on ? QPER : 2);
    vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, qpool, QPER * ci);
    qcnt[ci] = 1; qseg[ci][0] = -1; qinfo[ci][0] = -1; cur_ph = -1; tag_of[ci] = tag_cur;
    open_ = 1; bound = 0; cur_pipe = VK_NULL_HANDLE; nb_open = 0;
}
static void phase(int ph) {   /* HG_GSVK_TIMING: what is recorded next (see above) */
    if (!tim_on) return;
    ph += up_first ? PH_N : 0;
    if (ph == cur_ph || qcnt[ci] >= QPER - 1) return;
    vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, qpool, QPER * ci + (uint32_t)qcnt[ci]);
    qinfo[ci][qcnt[ci]] = -1; qseg[ci][qcnt[ci]++] = (int8_t)ph; cur_ph = ph;
}
static void use(VkPipeline p) {
    if (!bound) { vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &dset, 0, 0); bound = 1; }
    if (cur_pipe != p) { vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p); cur_pipe = p; }
}
static uint64_t submit(int wait);
static void close_batch(void) {
    if (!bn) return;
    int x0 = bx0 < 0 ? 0 : bx0, y0 = by0 < 0 ? 0 : by0, x1 = bx1 > 2048 ? 2048 : bx1, y1 = by1 > 2048 ? 2048 : by1;
    if (x1 > x0 && y1 > y0) {
        rec_open(); barrier();
        if (!skip_exact) {
            phase(PH_EXACT); use(pipeb);
            int32_t pc[5] = { (int32_t)bn, x0, y0, (int32_t)bfirst, (int32_t)up_count };   /* a primitive has a copy per copy of the set */
            vkCmdPushConstants(cmd, pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, pc);
            vkCmdDispatch(cmd, (uint32_t)(x1 - x0 + 7) / 8, (uint32_t)(y1 - y0 + 7) / 8, 1);   /* the exact copy */
            n_dispatch++;
        }
        if (up_count > 1) {   /* the upscaling copies (their own memory: no barrier), moved by under a pixel: a margin of 2 */
            const int cx0 = x0 >= 2 ? x0 - 2 : 0, cy0 = y0 >= 2 ? y0 - 2 : 0, cx1 = x1 + 2 > 2048 ? 2048 : x1 + 2, cy1 = y1 + 2 > 2048 ? 2048 : y1 + 2;
            if (tim_on) barrier();
            phase(PH_COPIES); use(pipebc);
            int32_t pcc[8] = { (int32_t)bn, cx0, cy0, (int32_t)bfirst, (int32_t)up_count, -1, 0, 0 };
            static int nobin = -1; if (nobin < 0) nobin = getenv("HG_GSVK_BINS") && !atoi(getenv("HG_GSVK_BINS"));
            if (bn >= BIN_MIN && brect && !nobin) {   /* the bins (see brect) */
                const int bw = (cx1 - cx0 + BIN - 1) / BIN, bh = (cy1 - cy0 + BIN - 1) / BIN; const uint32_t nw = (bn + 31) / 32, words = (uint32_t)(bw * bh) * nw;
                if (words <= BIN_MAX / 4) {
                    if (nbin + words <= (uint32_t)(rh + 1) * (BIN_MAX / 2)) {
                        if (bscratch_n < words) { free(bscratch); bscratch = malloc((size_t)words * 4); bscratch_n = bscratch ? words : 0; }
                        if (bscratch) {
                            memset(bscratch, 0, (size_t)words * 4);
                            for (uint32_t j = 0; j < bn; j++) {   /* a tile spans up to 5 pixels from its bin's start; copies move by up to 2 */
                                if (brect[j][1] <= brect[j][0] || brect[j][3] <= brect[j][2]) continue;
                                int a0 = (brect[j][0] - 24 - cx0) / BIN, a1 = (brect[j][1] + 2 - cx0) / BIN, b0 = (brect[j][2] - 24 - cy0) / BIN, b1 = (brect[j][3] + 2 - cy0) / BIN;
                                if (a0 < 0 || brect[j][0] - 24 - cx0 < 0) a0 = 0; if (b0 < 0 || brect[j][2] - 24 - cy0 < 0) b0 = 0; if (a1 < 0 || b1 < 0) continue;
                                if (a1 >= bw) a1 = bw - 1; if (b1 >= bh) b1 = bh - 1;
                                for (int y = b0; y <= b1; y++) for (int x = a0; x <= a1; x++) bscratch[(uint32_t)(y * bw + x) * nw + (j >> 5)] |= 1u << (j & 31);
                            }
                            ring_put((uint32_t *)binb.p + nbin, bscratch, (size_t)words * 4);
                            pcc[5] = (int32_t)nbin; pcc[6] = bw; pcc[7] = bh; nbin += words; n_binned++;
                        }
                    }
                }
            }
            vkCmdPushConstants(cmd, pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pcc, pcc);
            uint32_t N = 1; while ((N + 1) * (N + 1) <= up_count - 1) N++;
            if (dprof < 0) dprof = tim_on && getenv("HG_GSVK_DPROF") && atoi(getenv("HG_GSVK_DPROF"));
            const int dpo = dprof && qcnt[ci] < QPER - 2;
            if (dpo) {   /* a timestamp on each side of this dispatch, its segment pointing at its description */
                const uint32_t j = dpi_n++ & 0xFFFF;
                dpi[j].r = up_first != 0; dpi[j].k = bspr && btri ? 2 : btri ? 1 : 0; dpi[j].c = (uint8_t)dp_cls(bn); dpi[j].n = bn;
                dpi[j].mpix = (double)(cx1 - cx0) * N * (cy1 - cy0) * N / 1e6;
                vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, qpool, QPER * ci + (uint32_t)qcnt[ci]);
                qseg[ci][qcnt[ci]] = (int8_t)(PH_COPIES + (up_first ? PH_N : 0)); qinfo[ci][qcnt[ci]] = (int32_t)j; qcnt[ci]++;
            }
            vkCmdDispatch(cmd, ((uint32_t)(cx1 - cx0) * N + 7) / 8, ((uint32_t)(cy1 - cy0) * N + 7) / 8, 1);   /* tiles of the upscaled picture */
            if (dpo) { barrier(); vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, qpool, QPER * ci + (uint32_t)qcnt[ci]); qseg[ci][qcnt[ci]] = (int8_t)(PH_COPIES + (up_first ? PH_N : 0)); qinfo[ci][qcnt[ci]] = -1; qcnt[ci]++; cur_ph = -1; }
            n_dispatch++;
        }
    }
    bn = 0;
    if (eager < 0) eager = getenv("HG_GSVK_EAGER") ? atoi(getenv("HG_GSVK_EAGER")) : 2;
    if (open_ && eager > 0 && ++nb_open >= eager) submit(0);
}
void gsvk_break(void) { if (state > 0) close_batch(); }
/* submit what is recorded; with wait, wait for everything submitted, and the rings start over. Returns the
   submission's id (for gsvk_wait), 0 if nothing was recorded. */
static uint64_t submit(int wait) {
    if (state <= 0) return 0;
    close_batch();
    uint64_t id = 0;
    if (open_) {
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, qpool, QPER * ci + (uint32_t)qcnt[ci]++);
        vkEndCommandBuffer(cmd); open_ = 0;
        id = n_sub_id + 1;
        VkTimelineSemaphoreSubmitInfo tsi = { .sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO, .signalSemaphoreValueCount = 1, .pSignalSemaphoreValues = &id };
        VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .pNext = tl ? &tsi : 0, .commandBufferCount = 1, .pCommandBuffers = &cmd,
                            .signalSemaphoreCount = tl ? 1 : 0, .pSignalSemaphores = &tl };
        vkResetFences(dev, 1, &fences[ci]);
        if (shared_q) pthread_mutex_lock(&qmx);
        const VkResult r = vkQueueSubmit(queue, 1, &si, fences[ci]);
        if (shared_q) pthread_mutex_unlock(&qmx);
        if (r) { L("GSVK: submit failed\n"); state = -1; return 0; }
        busy[ci] = 1; sub_id[ci] = id; n_sub_id = id; n_submits++;
    }
    if (wait) { const int w = wsite; wsite = 2; for (int k = 0; k < NCMD; k++) slot_wait(k); wsite = w; nprim = nclut = 0; upoff = 0; nbin = 0; naux = 0; rh = 0; clut_ok = 0; }
    return id;
}
static void ring_switch(void) {
    submit(0); half_id[rh] = n_sub_id; rh ^= 1;
    { const int w = wsite; wsite = 1; gsvk_wait(half_id[rh]); wsite = w; }
    nprim = rh * (PRIM_MAX / 2); nclut = rh * (CLUT_MAX / 2); upoff = rh * (UP_BYTES / 2); nbin = (uint32_t)rh * (BIN_MAX / 2); naux = (uint32_t)rh * (AUX_MAX / 2); clut_ok = 0;
}
void gsvk_finish(void) { submit(1); }
uint64_t gsvk_submit(void) { return submit(0); }
void gsvk_wait(uint64_t id) { for (int k = 0; k < NCMD; k++) if (busy[k] && sub_id[k] <= id) slot_wait(k); }
int gsvk_poll(uint64_t id) {   /* that submission (and earlier) done? never waits (the finished ones are collected) */
    if (state <= 0) return 1;
    int done = 1;
    for (int k = 0; k < NCMD; k++) if (busy[k] && sub_id[k] <= id) { if (vkGetFenceStatus(dev, fences[k]) == VK_SUCCESS) slot_wait(k); else done = 0; }
    return done;
}
void gsvk_tag(int on) { tag_cur = on; }
double gsvk_tag_ms(void) { return gpu_ms_tag; }
int gsvk_hd_ok(void) { return state > 0 && hd_ok; }
int gsvk_hd_bc7(void) { return gsvk_hd_ok() && hd_bc7; }
double gsvk_hd_mb(void) { return hd_bytes / 1048576.0; }
/* an HD replacement into slot (levels: the mip chain, level 0 first, each half the one before, packed RGBA) */
static VkDeviceSize hd_level_bytes(int lw, int lh, int bc7) { return bc7 ? (VkDeviceSize)((lw + 3) / 4) * ((lh + 3) / 4) * 16 : (VkDeviceSize)lw * lh * 4; }
int gsvk_hd_upload(int slot, int w, int h, int levels, const uint8_t *pix, int bc7) {   /* RGBA8, or BC7 blocks; the mip chain level after level */
    if (!gsvk_hd_ok() || slot < 0 || slot >= HD_MAX || hd_img[slot] || (bc7 && !hd_bc7)) return 0;
    const VkFormat fmt = bc7 ? VK_FORMAT_BC7_UNORM_BLOCK : VK_FORMAT_R8G8B8A8_UNORM;
    for (int i = 0; i < hd_nstage; ) {   /* staging buffers whose copies are done */
        if (hd_stage[i].id <= n_sub_id && gsvk_poll(hd_stage[i].id)) { free_buf(&hd_stage[i].b); hd_stage[i] = hd_stage[--hd_nstage]; } else i++;
    }
    if (hd_nstage == 256) { submit(1); for (int i = 0; i < hd_nstage; i++) free_buf(&hd_stage[i].b); hd_nstage = 0; }
    if (levels > 16) levels = 16;
    VkDeviceSize total = 0; for (int l = 0, lw = w, lh = h; l < levels; l++, lw = lw > 1 ? lw / 2 : 1, lh = lh > 1 ? lh / 2 : 1) total += hd_level_bytes(lw, lh, bc7);
    VkImageCreateInfo ii = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = fmt, .extent = { (uint32_t)w, (uint32_t)h, 1 },
        .mipLevels = (uint32_t)levels, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
    if (vkCreateImage(dev, &ii, 0, &hd_img[slot])) { hd_img[slot] = 0; return 0; }
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(dev, hd_img[slot], &mr);
    VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = memtype(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) };
    Buf st = { 0 };
    if (ai.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(dev, &ai, 0, &hd_mem[slot])) { vkDestroyImage(dev, hd_img[slot], 0); hd_img[slot] = 0; L("GSVK: no video memory for an HD texture\n"); return 0; }
    vkBindImageMemory(dev, hd_img[slot], hd_mem[slot], 0);
    VkImageViewCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = hd_img[slot], .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = fmt,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, (uint32_t)levels, 0, 1 } };
    if (vkCreateImageView(dev, &vi, 0, &hd_view[slot]) || !mkbuf(&st, total, VK_BUFFER_USAGE_TRANSFER_SRC_BIT)) {
        vkDestroyImage(dev, hd_img[slot], 0); vkFreeMemory(dev, hd_mem[slot], 0); hd_img[slot] = 0; free_buf(&st); return 0; }
    ring_put(st.p, pix, (size_t)total);
    close_batch(); rec_open(); phase(PH_XFER);
    VkImageMemoryBarrier b = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = 0, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = hd_img[slot], .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, (uint32_t)levels, 0, 1 } };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, 0, 0, 0, 1, &b);
    VkBufferImageCopy rg[16]; VkDeviceSize off = 0; int nr = 0;
    for (int l = 0, lw = w, lh = h; l < levels && nr < 16; l++, lw = lw > 1 ? lw / 2 : 1, lh = lh > 1 ? lh / 2 : 1) {
        rg[nr++] = (VkBufferImageCopy){ .bufferOffset = off, .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, (uint32_t)l, 0, 1 }, .imageExtent = { (uint32_t)lw, (uint32_t)lh, 1 } };
        off += hd_level_bytes(lw, lh, bc7);
    }
    vkCmdCopyBufferToImage(cmd, st.b, hd_img[slot], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, (uint32_t)nr, rg);
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, 0, 0, 0, 1, &b);
    VkDescriptorImageInfo di = { hd_samp, hd_view[slot], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkWriteDescriptorSet wr = { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = dset, .dstBinding = 7, .dstArrayElement = (uint32_t)slot, .descriptorCount = 1,
                                .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &di };
    vkUpdateDescriptorSets(dev, 1, &wr, 0, 0);
    hd_stage[hd_nstage].b = st; hd_stage[hd_nstage].id = n_sub_id + 1; hd_nstage++;   /* the open command buffer's id when submitted */
    hd_bytes += (double)mr.size;
    return 1;
}
static void ring_switch(void);
/* local memory words [w, w+n) from src, at this point of the stream */
void gsvk_upload(uint32_t w, uint32_t n, const uint32_t *src) {
    if (state <= 0 || !n) return;
    close_batch();
    if (upoff + n * 4 > (uint32_t)(rh + 1) * (UP_BYTES / 2)) ring_switch();
    ring_put((uint8_t *)upb.p + upoff, src, (size_t)n * 4);
    rec_open(); barrier(); phase(PH_XFER);
    VkBufferCopy c0 = { upoff, (VkDeviceSize)up_first * (1u << 22) + (VkDeviceSize)w * 4, (VkDeviceSize)n * 4 };
    vkCmdCopyBuffer(cmd, upb.b, dvram.b, 1, &c0);   /* over the bus once, into the set's exact copy... */
    upoff += (n * 4 + 255) & ~255u;
    if (up_count > 1) {   /* ...then spread from there into its interleaved copies, in video memory (prim 6, set_va) */
        GsVkSprite s; memset(&s, 0, sizeof s);
        const uint32_t total = n * (up_count - 1);
        s.prim = 6; s.px0 = 0; s.px1 = 1024; s.py0 = 0; s.py1 = (int32_t)((total + 1023) / 1024); s.z = w; s.ly1 = (int32_t)n; s.lx0 = (int32_t)up_first;
        const uint32_t keep = mbase_add; mbase_add = 0;
        static const uint32_t noclut[256];
        gsvk_prim(&s, noclut, 1); mbase_add = keep;
    }
}
/* n (word, value, mask) triples: those bits of those words become the values', in each copy of the set (gs.c xm_flush) */
void gsvk_upload_masked(const uint32_t *trip, uint32_t n) {
    if (state <= 0 || !n) return;
    close_batch();
    const uint32_t words = (n * 3 + 63) & ~63u;
    if (words > AUX_MAX / 2) {   /* (more than a half: in parts) */
        const uint32_t part = (AUX_MAX / 2) / 3 - 64;
        for (uint32_t i = 0; i < n; i += part) gsvk_upload_masked(trip + (size_t)i * 3, n - i < part ? n - i : part);
        return;
    }
    if (naux + words > (uint32_t)(rh + 1) * (AUX_MAX / 2)) ring_switch();
    ring_put((uint32_t *)auxb.p + naux, trip, (size_t)n * 12);
    GsVkSprite s; memset(&s, 0, sizeof s);
    s.prim = 5; s.px0 = 0; s.px1 = 1024; s.py0 = 0; s.py1 = (int32_t)((n + 1023) / 1024); s.z = naux; s.ly1 = (int32_t)n; s.lx0 = (int32_t)up_first; s.ln = (int32_t)up_count;
    naux += words;
    const uint32_t keep = mbase_add; mbase_add = 0;
    static const uint32_t noclut[256];
    gsvk_prim(&s, noclut, 1); mbase_add = keep;
}
/* one primitive: alone (lines, serial ones) or added to the batch being collected */
void gsvk_prim(const GsVkSprite *s, const uint32_t *clut, int alone) {
    if (state <= 0) return;
    if (s->mbase == 0 && (nprim + up_count > (uint32_t)(rh + 1) * (PRIM_MAX / 2) || nclut == (uint32_t)(rh + 1) * (CLUT_MAX / 2))) ring_switch();   /* a primitive's copies stay together */
    static uint32_t last[256];   /* the ring may be video memory mapped over the bus: never read it back */
    if (!clut_ok || memcmp(last, clut, 1024)) { memcpy(last, clut, 1024); ring_put((uint32_t *)clutb.p + nclut++ * 256, clut, 1024); clut_ok = 1; }
    GsVkSprite r = *s; r.clutoff = (nclut - 1) * 256; r.mbase += mbase_add;
    ring_put((GsVkSprite *)primb.p + nprim, &r, sizeof r);   /* written once: the ring may be video memory over the bus, never read it */
    if (alone && skip_exact && !(s->mbase >> 20) && s->prim < 4) { nprim++; return; }   /* the set's exact copy is not drawn */
    if (alone) {
        close_batch(); rec_open(); barrier(); phase(s->prim == 4 ? PH_COMPOSE : s->prim >= 5 ? PH_XFER : PH_SINGLE); use(pipe1);
        int32_t pc[5] = { (int32_t)nprim, 0, 0, 0, (int32_t)up_count };   /* the copies in a set (the shader's set_va) */
        vkCmdPushConstants(cmd, pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, pc);
        if (s->serial) vkCmdDispatch(cmd, 1, 1, 1);
        else vkCmdDispatch(cmd, (uint32_t)(s->px1 - s->px0 + 7) / 8, (uint32_t)(s->py1 - s->py0 + 7) / 8, 1);
        n_dispatch++;
    } else {
        if (!bn) { bfirst = nprim; bx0 = by0 = 1 << 30; bx1 = by1 = -(1 << 30); bspr = btri = 0; }
        if (s->prim == 1) btri++; else bspr++;
        bn++;
        if (brect || (brect = malloc(sizeof *brect * PRIM_MAX))) { const uint32_t j = bn - 1; if (j < PRIM_MAX) { brect[j][0] = (int16_t)s->px0; brect[j][1] = (int16_t)s->px1; brect[j][2] = (int16_t)s->py0; brect[j][3] = (int16_t)s->py1; } }
        if (s->px1 > s->px0 && s->py1 > s->py0) {   /* (a copy's empty place holder adds nothing) */
            if (s->px0 < bx0) bx0 = s->px0; if (s->py0 < by0) by0 = s->py0; if (s->px1 > bx1) bx1 = s->px1; if (s->py1 > by1) by1 = s->py1; }
    }
    nprim++;
}
/* the 8 KB pages set in `pages` (512 bits) from the GPU (copy k) into dst */
void gsvk_download(const uint64_t pages[8], uint32_t *dst) { gsvk_download_copy(0, pages, dst); }
void gsvk_download_copy(int k, const uint64_t pages[8], uint32_t *dst) {
    if (state <= 0 || k < 0 || (uint32_t)k >= ncopy) return;
    close_batch();
    VkBufferCopy c[512]; uint32_t nc = 0;
    for (uint32_t p = 0; p < 512; p++) if (pages[p >> 6] >> (p & 63) & 1) {
        if (nc && c[nc - 1].srcOffset + c[nc - 1].size == (VkDeviceSize)p * 8192) c[nc - 1].size += 8192;
        else c[nc++] = (VkBufferCopy){ (VkDeviceSize)p * 8192, (VkDeviceSize)p * 8192, 8192 };
    }
    if (!nc) return;
    for (uint32_t i = 0; i < nc; i++) c[i].srcOffset += (VkDeviceSize)k * (1u << 22);
    rec_open(); barrier(); phase(PH_XFER);
    vkCmdCopyBuffer(cmd, dvram.b, vram.b, nc, c);
    for (uint32_t i = 0; i < nc; i++) c[i].srcOffset -= (VkDeviceSize)k * (1u << 22);
    VkMemoryBarrier hb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &hb, 0, 0, 0, 0);
    gsvk_finish();
    for (uint32_t i = 0; i < nc; i++) memcpy((uint8_t *)dst + c[i].srcOffset, (uint8_t *)vram.p + c[i].srcOffset, c[i].size);
    vfresh = 1;   /* the mirror no longer holds the whole of the GPU's memory */
}
void gsvk_skip_exact(int on) { if (state > 0) { close_batch(); skip_exact = on; } }
void gsvk_set_target(uint32_t first, uint32_t count) {
    if (state <= 0) return;
    close_batch();   /* a batch draws into one set */
    if (first + count > ncopy) return;
    mbase_add = first << 20; up_first = first; up_count = count;
}
/* a different number of copies (the upscaling factor or frame interpolation changed while the game runs): all GPU
   work is finished and local memory is made again at the new size. Its contents are gone: the caller uploads local
   memory again. The window's picture images are separate and stay. */
int gsvk_resize(int ncopies) {
    if (state <= 0) return 0;
    submit(1);
    const uint32_t old = ncopy;
    free_buf(&dvram); free_buf(&hrb);
    if (!mk_local(ncopies)) {
        L("GSVK: no video memory for %d copies of local memory; back to %u\n", ncopies, old);
        free_buf(&dvram); free_buf(&hrb);
        if (!mk_local((int)old)) { L("GSVK: local memory lost\n"); state = -1; return 0; }
    }
    VkDescriptorBufferInfo bi = { dvram.b, 0, VK_WHOLE_SIZE };
    VkWriteDescriptorSet wr = { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = dset, .dstBinding = 0, .descriptorCount = 1,
                                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &bi };
    vkUpdateDescriptorSets(dev, 1, &wr, 0, 0);
    vfresh = 1;
    return ncopy == (uint32_t)ncopies;
}
void gsvk_copy_copies(uint32_t dst, uint32_t src, uint32_t count) {
    if (state <= 0 || dst + count > ncopy || src + count > ncopy || !count) return;
    close_batch(); rec_open(); barrier(); phase(PH_XFER);
    VkBufferCopy c = { (VkDeviceSize)src << 22, (VkDeviceSize)dst << 22, (VkDeviceSize)count << 22 };
    vkCmdCopyBuffer(cmd, dvram.b, dvram.b, 1, &c);
}
/* the out region holds 6 pictures (slots); compose_rec records one, compose_fetch reads it once its submission is done */
int gsvk_compose_rec(const GsVkSprite *s0, int slot) {
    if (state <= 0 || !outbytes || slot < 0 || slot > 5) return 0;
    GsVkSprite s = *s0;
    const VkDeviceSize n = (VkDeviceSize)s.px1 * s.py1 * 4, slotb = (outbytes / 6) & ~(VkDeviceSize)255;
    if (n > slotb || s.px1 <= 0 || s.py1 <= 0) return 0;
    s.prim = 4; s.px0 = s.py0 = 0; s.serial = 0; s.z = (uint32_t)((outbase + slot * slotb) / 4); s.mbase = 0;
    const uint32_t keep = mbase_add; mbase_add = 0;   /* the compose primitive reads copies by itself */
    static const uint32_t noclut[256];
    gsvk_prim(&s, noclut, 1); mbase_add = keep;
    rec_open(); barrier(); phase(PH_XFER);
    VkBufferCopy c = { outbase + slot * slotb, slot * slotb, n };
    vkCmdCopyBuffer(cmd, dvram.b, hrb.b, 1, &c);
    VkMemoryBarrier hb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &hb, 0, 0, 0, 0);
    return 1;
}
void gsvk_compose_fetch(int slot, uint32_t *dst, size_t words) {
    const VkDeviceSize slotb = (outbytes / 6) & ~(VkDeviceSize)255;
    if (state > 0 && slot >= 0 && slot < 6 && words * 4 <= slotb) memcpy(dst, (uint8_t *)hrb.p + slot * slotb, words * 4);
}
int gsvk_compose(const GsVkSprite *s0, uint32_t *dst) {
    if (!gsvk_compose_rec(s0, 0)) return 0;
    gsvk_finish();
    if (state <= 0) return 0;
    gsvk_compose_fetch(0, dst, (size_t)s0->px1 * s0->py1);
    return 1;
}
void gsvk_wait_sites(double ms[4]) { for (int i = 0; i < 4; i++) ms[i] = wait_site_ms[i]; }
void gsvk_stats(uint64_t *submits, uint64_t *dispatches, double *gpu_ms, double *wait_ms) { *submits = n_submits; *dispatches = n_dispatch; *gpu_ms = gpu_ms_all; *wait_ms = wait_ms_all; }
void gsvk_dprof_print(void) {
    if (dprof <= 0) return;
    static const char *kn[DP_K] = { "sprites", "triangles", "both" }, *cn[DP_B] = { "1", "2-4", "5-16", "17-64", "65-256", ">256" };
    for (int r = 0; r < 2; r++) for (int k = 0; k < DP_K; k++) for (int c = 0; c < DP_B; c++) if (dp[r][k][c].n)
        L("GSVK dprof %s %-9s %6s prims: %7llu dispatches, %9.1f ms, %8.1f Mpix (%.2f ns per pixel-copy), %.0f prims\n", r ? "replay" : "real  ", kn[k], cn[c],
          (unsigned long long)dp[r][k][c].n, dp[r][k][c].ms, dp[r][k][c].mpix, dp[r][k][c].mpix > 0 ? dp[r][k][c].ms * 1e3 / dp[r][k][c].mpix : 0, dp[r][k][c].prims);
}
void gsvk_phase_stats(double ms[10]) { for (int i = 0; i < 2 * PH_N; i++) ms[i] = ph_ms[i]; }

/* ------------------------------------------------------------------ checking against the CPU */
/* the GPU's memory becomes `now`: the mirror takes it, the 4 KB chunks that changed go up */
void gsvk_check_begin(const uint32_t *now) {
    if (state <= 0) return;
    static VkBufferCopy regs[1024]; uint32_t nregs = 0;
    uint32_t *g = vram.p;
    for (uint32_t i = 0; i < (1u << 20); i += 1024) {
        if (!vfresh && !memcmp(g + i, now + i, 4096)) continue;
        memcpy(g + i, now + i, 4096);
        if (nregs && regs[nregs - 1].srcOffset + regs[nregs - 1].size == (VkDeviceSize)i * 4) regs[nregs - 1].size += 4096;
        else regs[nregs++] = (VkBufferCopy){ (VkDeviceSize)i * 4, (VkDeviceSize)i * 4, 4096 };   /* at most 512 runs */
    }
    vfresh = 0;
    if (nregs) { close_batch(); rec_open(); barrier(); vkCmdCopyBuffer(cmd, vram.b, dvram.b, nregs, regs); }
}
/* everything recorded is drawn; the GPU's whole memory, read back into the mirror, is returned (GPU ms in *ms) */
const uint32_t *gsvk_check_end(double *ms) {
    if (state <= 0) return 0;
    double before = gpu_ms_all;
    close_batch(); rec_open(); barrier();
    VkBufferCopy all = { 0, 0, (VkDeviceSize)(1u << 20) * 4 };
    vkCmdCopyBuffer(cmd, dvram.b, vram.b, 1, &all);
    VkMemoryBarrier hb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &hb, 0, 0, 0, 0);
    gsvk_finish();
    if (state <= 0) return 0;
    if (ms) *ms = gpu_ms_all - before;
    return vram.p;
}

/* ------------------------------------------------------------------ presenting from video memory
   The window's pictures stay on the GPU. gsvk_pic() (GS thread) composes a display picture, as gsvk_compose() does,
   and copies it into one of NPIC picture images; the UI thread's gsvk_present() blits a picture image into the
   window's swapchain (scaled, letterboxed like SDL_RenderSetLogicalSize(640, 448) did) after the GS submission that
   made it, which every GS submission signals on the timeline semaphore tl with its id. The UI thread records into its
   own command buffers and submits to its own queue. A picture image is free again (pic_free) once the UI thread has
   moved on to a newer picture and its blits of it are done, or when the picture is dropped unseen. No picture crosses
   the bus, and the GS thread never waits for the GPU because of the window. */
enum { NPIC = 12, NFR = 3, OVW = 320, OVH = 176 };   /* (the overlay's largest size) */
static struct { VkImage img; VkDeviceMemory mem; int w, h; } pic[NPIC];
static _Atomic uint32_t pic_free = (1u << NPIC) - 1;
static _Atomic int pres_ok;
static uint32_t memtype(uint32_t bits, VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(phys, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) if ((bits >> i & 1) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    return UINT32_MAX;
}
static int mkimg(VkImage *img, VkDeviceMemory *mem, int w, int h) {   /* RGBA, device local, a transfer source and target */
    VkImageCreateInfo ii = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
        .extent = { (uint32_t)w, (uint32_t)h, 1 }, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
    const uint32_t fams[2] = { qfam, ufam };   /* written by the GS's queue, blitted by the window's */
    if (ufam != qfam) { ii.sharingMode = VK_SHARING_MODE_CONCURRENT; ii.queueFamilyIndexCount = 2; ii.pQueueFamilyIndices = fams; }
    if (vkCreateImage(dev, &ii, 0, img)) return 0;
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(dev, *img, &mr);
    VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = memtype(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) };
    if (ai.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(dev, &ai, 0, mem)) { vkDestroyImage(dev, *img, 0); *img = 0; return 0; }
    vkBindImageMemory(dev, *img, *mem, 0); return 1;
}
static void imgbar(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to, VkAccessFlags sa, VkAccessFlags da, VkPipelineStageFlags ss, VkPipelineStageFlags ds) {
    VkImageMemoryBarrier b = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = sa, .dstAccessMask = da, .oldLayout = from, .newLayout = to,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = img, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    vkCmdPipelineBarrier(cb, ss, ds, 0, 0, 0, 0, 0, 1, &b);
}
int gsvk_ready(void) { return atomic_load(&ready); }
int gsvk_present_ready(void) { return atomic_load(&pres_ok); }
uint64_t gsvk_done(void) { uint64_t v = 0; if (tl) vkGetSemaphoreCounterValue(dev, tl, &v); return v; }
uint64_t gsvk_last(void) { return n_sub_id; }
void gsvk_pic_release(int k) { if (k >= 0 && k < NPIC) atomic_fetch_or(&pic_free, 1u << k); }
/* GS thread: the picture s (as for gsvk_compose: px1 x py1) into a free picture image; its index and, in *id, the
   submission that makes it; -1 when there is none (no window, or no image came free within a second) */
int gsvk_pic(const GsVkSprite *s0, uint64_t *id) {
    if (state <= 0 || !atomic_load(&pres_ok)) return -1;
    const int W = s0->px1, H = s0->py1;
    const VkDeviceSize slotb = (outbytes / 6) & ~(VkDeviceSize)255, area = outbase + 3 * slotb;   /* the out region's slot 3 */
    if (W <= 0 || H <= 0 || (VkDeviceSize)W * H * 4 > slotb) return -1;
    int k = -1;
    for (int tries = 0; k < 0 && tries < 2000; tries++) {
        uint32_t f = atomic_load(&pic_free);
        if (!f) { struct timespec ts = { 0, 500000 }; nanosleep(&ts, 0); continue; }
        const int b = __builtin_ctz(f);
        if (atomic_compare_exchange_weak(&pic_free, &f, f & ~(1u << b))) k = b;
    }
    if (k < 0) return -1;
    if (pic[k].w != W || pic[k].h != H) {   /* free, so no UI work uses it */
        if (pic[k].img) { vkDestroyImage(dev, pic[k].img, 0); vkFreeMemory(dev, pic[k].mem, 0); pic[k].img = 0; }
        if (!mkimg(&pic[k].img, &pic[k].mem, W, H)) { pic[k].w = 0; gsvk_pic_release(k); return -1; }
        pic[k].w = W; pic[k].h = H;
    }
    GsVkSprite s = *s0; s.prim = 4; s.px0 = s.py0 = 0; s.serial = 0; s.z = (uint32_t)(area / 4); s.mbase = 0;
    const uint32_t keep = mbase_add; mbase_add = 0;   /* the compose primitive reads copies by itself */
    const int skip = skip_exact; skip_exact = 0;
    static const uint32_t noclut[256];
    gsvk_prim(&s, noclut, 1); mbase_add = keep; skip_exact = skip;
    rec_open(); barrier(); phase(PH_COMPOSE);
    imgbar(cmd, pic[k].img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy r = { .bufferOffset = area, .bufferRowLength = (uint32_t)W, .bufferImageHeight = (uint32_t)H,
                            .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { (uint32_t)W, (uint32_t)H, 1 } };
    vkCmdCopyBufferToImage(cmd, dvram.b, pic[k].img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
    imgbar(cmd, pic[k].img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    *id = submit(0);
    if (!*id) { gsvk_pic_release(k); return -1; }
    return k;
}

/* the UI thread's side */
static VkSurfaceKHR surf; static VkSwapchainKHR sc; static VkFormat scfmt; static VkColorSpaceKHR sccs; static VkExtent2D scext; static VkPresentModeKHR pmode;
static int pm_has[4];                    /* present modes the surface offers */
static _Atomic int pm_want = 0, pm_set = -1;   /* gsvk_present_mode(): 0 mailbox, 1 FIFO (vsync), 2 immediate */
void gsvk_present_mode(int v) { atomic_store(&pm_want, v); }
static VkPresentModeKHR pick_mode(int v) {
    const VkPresentModeKHR want = v == 1 ? VK_PRESENT_MODE_FIFO_KHR : v == 2 ? VK_PRESENT_MODE_IMMEDIATE_KHR : VK_PRESENT_MODE_MAILBOX_KHR;
    return pm_has[want] ? want : pm_has[VK_PRESENT_MODE_MAILBOX_KHR] ? VK_PRESENT_MODE_MAILBOX_KHR : VK_PRESENT_MODE_FIFO_KHR;
}
static const char *mode_name(VkPresentModeKHR m) { return m == VK_PRESENT_MODE_FIFO_KHR ? "vsync" : m == VK_PRESENT_MODE_MAILBOX_KHR ? "mailbox" : "immediate"; }
static uint32_t scn; static VkImage scimg[8]; static VkSemaphore scdone[8]; static int sc_stale;
static void (*win_size)(int *w, int *h); static int sc_free;   /* the window's size (Linux); the surface leaves the size to us (Wayland: 0xFFFFFFFF) */
static uint64_t pres_id; static _Atomic int sc_gen; double gsvk_present_ms[5];   /* the last present's steps (debugging): fence, acquire, record, submit, present */   /* present ids (VK_KHR_present_id, increasing over all swapchains); swapchains made */
static VkCommandPool upool; static VkCommandBuffer ucb[NFR]; static VkFence ufence[NFR]; static VkSemaphore uacq[NFR]; static int ufr, usub[NFR], uslot[NFR];
static int ucur = -1, uref[NPIC]; static uint64_t ucur_id;
static Buf ovbuf; static VkImage ovimg; static VkDeviceMemory ovmem;
static void ui_unref(int k) { if (k >= 0 && --uref[k] == 0) gsvk_pic_release(k); }
static int mk_swapchain(void) {
    VkSurfaceCapabilitiesKHR cap; if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys, surf, &cap)) return 0;
    sc_free = cap.currentExtent.width == 0xFFFFFFFFu;
    if (sc_free) {   /* Wayland: the swapchain's size is the window's */
        int w = 0, h = 0; if (win_size) win_size(&w, &h);
        if (w <= 0 || h <= 0) return 0;
        cap.currentExtent.width = (uint32_t)w < cap.minImageExtent.width ? cap.minImageExtent.width : (uint32_t)w > cap.maxImageExtent.width ? cap.maxImageExtent.width : (uint32_t)w;
        cap.currentExtent.height = (uint32_t)h < cap.minImageExtent.height ? cap.minImageExtent.height : (uint32_t)h > cap.maxImageExtent.height ? cap.maxImageExtent.height : (uint32_t)h;
    }
    if (!cap.currentExtent.width || !cap.currentExtent.height) return 0;   /* minimized */
    if (!(cap.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) { L("GSVK: the window's images cannot be blitted into\n"); return 0; }
    if (shared_q) pthread_mutex_lock(&qmx);
    vkQueueWaitIdle(uqueue);   /* the old swapchain's images and semaphores are no longer in use */
    if (shared_q) pthread_mutex_unlock(&qmx);
    uint32_t n = cap.minImageCount + (getenv("HG_SCIMAGES") ? (uint32_t)atoi(getenv("HG_SCIMAGES")) : 1); if (cap.maxImageCount && n > cap.maxImageCount) n = cap.maxImageCount;
    VkCompositeAlphaFlagBitsKHR ca = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if (!(cap.supportedCompositeAlpha & ca)) for (uint32_t b = 1; b; b <<= 1) if (cap.supportedCompositeAlpha & b) { ca = (VkCompositeAlphaFlagBitsKHR)b; break; }
    VkSwapchainCreateInfoKHR ci = { .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, .surface = surf, .minImageCount = n, .imageFormat = scfmt, .imageColorSpace = sccs,
        .imageExtent = cap.currentExtent, .imageArrayLayers = 1, .imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT, .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .preTransform = cap.currentTransform, .compositeAlpha = ca, .presentMode = pmode, .clipped = VK_TRUE, .oldSwapchain = sc };
    int ptim = 0;
    if (can_ptim) {   /* does this surface report present timing? */
        VkPresentTimingSurfaceCapabilitiesEXT ptc = { .sType = VK_STRUCTURE_TYPE_PRESENT_TIMING_SURFACE_CAPABILITIES_EXT };
        VkSurfaceCapabilities2KHR c2 = { .sType = VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_2_KHR, .pNext = &ptc };
        VkPhysicalDeviceSurfaceInfo2KHR si = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SURFACE_INFO_2_KHR, .surface = surf };
        PFN_vkGetPhysicalDeviceSurfaceCapabilities2KHR g = (PFN_vkGetPhysicalDeviceSurfaceCapabilities2KHR)vkGetInstanceProcAddr(inst, "vkGetPhysicalDeviceSurfaceCapabilities2KHR");
        ptim = g && !g(phys, &si, &c2) && ptc.presentTimingSupported && (ptc.presentStageQueries & VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_OUT_BIT_EXT);
        sc_prel = ptim && can_prel && ptc.presentAtRelativeTimeSupported;
        if (ptim) ci.flags |= VK_SWAPCHAIN_CREATE_PRESENT_TIMING_BIT_EXT;
    }
    struct { VkStructureType sType; const void *pNext; void *hmonitor; } fw = { (VkStructureType)1000255001, 0, fse_mon };   /* VkSurfaceFullScreenExclusiveWin32InfoEXT */
    struct { VkStructureType sType; void *pNext; int mode; } fx = { (VkStructureType)1000255000, &fw, 3 };   /* VkSurfaceFullScreenExclusiveInfoEXT, application controlled */
    const int fse = use_fse && fse_mon && atomic_load(&fse_want);
    if (fse) { fx.pNext = (void *)&fw; fw.pNext = ci.pNext; ci.pNext = &fx; }
    VkSwapchainKHR nsc;
    { const VkResult r = vkCreateSwapchainKHR(dev, &ci, 0, &nsc);
      if (r) { static int nl; if (nl++ < 4) L("GSVK: no swapchain (%d; %ux%u, %u images, format %d/%d, mode %d, flags 0x%x, usage 0x%x)\n", (int)r, ci.imageExtent.width, ci.imageExtent.height, n, (int)scfmt, (int)sccs, (int)pmode, (unsigned)ci.flags, (unsigned)cap.supportedUsageFlags); return 0; } }
    atomic_store(&fse_have, 0);
    if (fse) { const VkResult fr = fse_acquire(dev, nsc); atomic_store(&fse_have, fr == VK_SUCCESS); static int nl; if (nl++ < 8) L("GSVK: full-screen exclusive: %s (%d)\n", fr == VK_SUCCESS ? "acquired" : "not acquired", (int)fr); }
    sc_ptim = 0; if (!ptim) sc_prel = 0;
    if (ptim && !pt_qsize(dev, nsc, 64)) {   /* the time domain: QueryPerformanceCounter, the host clock (host_now) */
        VkTimeDomainKHR dm[8]; uint64_t di[8];
        VkSwapchainTimeDomainPropertiesEXT tdp = { .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_TIME_DOMAIN_PROPERTIES_EXT, .timeDomainCount = 8, .pTimeDomains = dm, .pTimeDomainIds = di };
        uint64_t cnt = 0;
        char dl[128] = ""; pt_qpc = 0;
        if (!pt_domains(dev, nsc, &tdp, &cnt)) for (uint32_t i = 0; i < tdp.timeDomainCount; i++) {
            snprintf(dl + strlen(dl), sizeof dl - strlen(dl), " %d", (int)dm[i]);
            if (dm[i] == VK_TIME_DOMAIN_QUERY_PERFORMANCE_COUNTER_KHR) { pt_dom = di[i]; sc_ptim = 1; pt_qpc = 1; }
            else if (!sc_ptim) { pt_dom = di[i]; sc_ptim = 1; }   /* (a local clock in ns: the intervals, not the host's time) */
        }
        static int nl; if (nl++ < 4) L("GSVK: present timing %s (time domains%s)\n", sc_ptim ? pt_qpc ? "on (first pixel out, QueryPerformanceCounter)" : "on (first pixel out, a local clock)" : "off", dl);
    }
    if (sc) vkDestroySwapchainKHR(dev, sc, 0);
    for (uint32_t i = 0; i < scn; i++) vkDestroySemaphore(dev, scdone[i], 0);
    sc = nsc; scext = cap.currentExtent; scn = 8; vkGetSwapchainImagesKHR(dev, sc, &scn, scimg); atomic_fetch_add(&sc_gen, 1);
    VkSemaphoreCreateInfo sci = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    for (uint32_t i = 0; i < scn; i++) vkCreateSemaphore(dev, &sci, 0, &scdone[i]);
    sc_stale = 0; return 1;
}
static int present_init_(void);
#ifndef _WIN32
/* UI thread (Linux): a swapchain on the surface mksurf makes for the window (SDL_Vulkan_CreateSurface) */
int gsvk_present_init_with(int (*mksurf)(void *inst, void *surf), void (*size)(int *w, int *h)) {
    win_size = size;
    if (gsvk_ready() <= 0 || !can_present) return 0;
    if (!mksurf(inst, &surf)) { L("GSVK: no surface for the window\n"); return 0; }
    return present_init_();
}
#endif
/* UI thread: a swapchain on the window (Windows: its HINSTANCE and HWND); 0 if presenting from video memory is not
   possible (the caller then draws with SDL) */
int gsvk_present_init(void *hinstance, void *hwnd) {
    if (gsvk_ready() <= 0 || !can_present) return 0;
#ifdef _WIN32
    { extern __declspec(dllimport) void *__stdcall MonitorFromWindow(void *, unsigned long); fse_mon = MonitorFromWindow(hwnd, 2); }
#endif
#ifdef _WIN32
    typedef struct { VkStructureType sType; const void *pNext; VkFlags flags; void *hinstance, *hwnd; } W32SurfaceInfo;   /* VkWin32SurfaceCreateInfoKHR */
    typedef VkResult (VKAPI_PTR *CreateW32Surface)(VkInstance, const W32SurfaceInfo *, const VkAllocationCallbacks *, VkSurfaceKHR *);
    CreateW32Surface cs = (CreateW32Surface)vkGetInstanceProcAddr(inst, "vkCreateWin32SurfaceKHR");
    W32SurfaceInfo wi = { VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR, 0, 0, hinstance, hwnd };
    if (!cs || cs(inst, &wi, 0, &surf)) { L("GSVK: no surface for the window\n"); return 0; }
#else
    (void)hinstance; (void)hwnd; return 0;
#endif
    return present_init_();
}
static int present_init_(void) {
    VkBool32 sup = 0; vkGetPhysicalDeviceSurfaceSupportKHR(phys, ufam, surf, &sup);
    if (!sup) { L("GSVK: the queue cannot present to the window\n"); return 0; }
    VkSurfaceFormatKHR fm[64]; uint32_t nf = 64; vkGetPhysicalDeviceSurfaceFormatsKHR(phys, surf, &nf, fm);
    if (!nf) return 0;
    scfmt = fm[0].format; sccs = fm[0].colorSpace;   /* a UNORM format: the picture's values go to the screen as they are */
    for (uint32_t i = 0; i < nf; i++) if (fm[i].format == VK_FORMAT_B8G8R8A8_UNORM || fm[i].format == VK_FORMAT_R8G8B8A8_UNORM) { scfmt = fm[i].format; sccs = fm[i].colorSpace; break; }
    /* Mailbox: the picture replaces any not yet shown and is shown at the display's next refresh, no tearing; the UI
       thread's own clock (59.94 Hz) decides when. FIFO (HG_VSYNC=1) stalled the UI thread's submissions 60-200 ms at a
       time in a window on the 120 Hz screen, even at native resolution. HG_VSYNC=0: immediate (may tear). */
    { VkPresentModeKHR pm[16]; uint32_t np = 16; vkGetPhysicalDeviceSurfacePresentModesKHR(phys, surf, &np, pm);   /* (vsync: settings.c, HG_VSYNC) */
      for (uint32_t i = 0; i < np; i++) if ((uint32_t)pm[i] < 4) pm_has[pm[i]] = 1;
      pm_set = atomic_load(&pm_want); pmode = pick_mode(pm_set); }
    VkCommandPoolCreateInfo cpci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = ufam };
    VkCommandBufferAllocateInfo cbai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = NFR };
    VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO }; VkSemaphoreCreateInfo sci = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    if (vkCreateCommandPool(dev, &cpci, 0, &upool)) return 0;
    cbai.commandPool = upool; if (vkAllocateCommandBuffers(dev, &cbai, ucb)) return 0;
    for (int f = 0; f < NFR; f++) { if (vkCreateFence(dev, &fci, 0, &ufence[f]) || vkCreateSemaphore(dev, &sci, 0, &uacq[f])) return 0; uslot[f] = -1; }
    if (!mkbuf(&ovbuf, (VkDeviceSize)NFR * OVW * OVH * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT) || !mkimg(&ovimg, &ovmem, OVW, OVH)) return 0;
    if (use_cal) { VkQueryPoolCreateInfo q = { .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = 2 * NFR }; if (vkCreateQueryPool(dev, &q, 0, &uqp)) use_cal = 0; }
    if (!mk_swapchain()) sc_stale = 1;
    L("GSVK: the window is presented from video memory (%s)\n", mode_name(pmode));
    atomic_store(&pres_ok, 1); return 1;
}
/* UI thread: show picture k (k < 0: the current one again) once GS submission id is done, with the overlay ov
   (OVW x OVH, 0x00BBGGRR, or 0) at the picture's top left */
uint64_t gsvk_present(int k, uint64_t id, const uint32_t *ov, int ovw, int ovh, uint64_t rel_ns) {
    if (ovw <= 0 || ovh <= 0 || ovw > OVW || ovh > OVH) ov = 0;
    if (!atomic_load(&pres_ok)) return 0;
    if (k >= 0 && k < NPIC) { uref[k]++; ui_unref(ucur); ucur = k; ucur_id = id; }
    const int f = ufr;
    const double p0 = host_now();
    if (usub[f]) {
        vkWaitForFences(dev, 1, &ufence[f], VK_TRUE, UINT64_MAX); usub[f] = 0; ui_unref(uslot[f]); uslot[f] = -1;
        uint64_t ts[2];
        if (use_cal && !vkGetQueryPoolResults(dev, uqp, 2 * (uint32_t)f, 2, sizeof ts, ts, 8, VK_QUERY_RESULT_64_BIT)) {
            if (p0 - cal_t > 1) {   /* the GPU's clock against QueryPerformanceCounter */
                VkCalibratedTimestampInfoKHR ci2[2] = { { .sType = VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_KHR, .timeDomain = VK_TIME_DOMAIN_DEVICE_KHR },
                                                        { .sType = VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_KHR, .timeDomain =
#ifdef _WIN32
                                                          VK_TIME_DOMAIN_QUERY_PERFORMANCE_COUNTER_KHR
#else   /* host_now() is CLOCK_MONOTONIC there (host_qpc_seconds: ns) */
                                                          VK_TIME_DOMAIN_CLOCK_MONOTONIC_KHR
#endif
                                                        } };
                uint64_t v[2], dv; if (!get_cal(dev, 2, ci2, v, &dv)) { cal_off = host_qpc_seconds(v[1]) - v[0] * tsperiod * 1e-9; cal_t = p0; }
            }
            const int j = (int)(upid[f] & 63); ugt[j].pid = upid[f]; ugt[j].beg = ts[0] * tsperiod * 1e-9 + cal_off; ugt[j].end = ts[1] * tsperiod * 1e-9 + cal_off;
        }
    }
    const double p1 = host_now();
    { const int w = atomic_load(&pm_want);   /* vsync changed (the Options menu): a new swapchain in that mode */
      if (w != pm_set) { pm_set = w; const VkPresentModeKHR m = pick_mode(w); if (m != pmode) { pmode = m; sc_stale = 1; L("GSVK: presenting with %s\n", mode_name(m)); } } }
    if (use_fse && atomic_load(&fse_want) != atomic_load(&fse_have)) { static int last = -1; const int w = atomic_load(&fse_want); if (w != last) { last = w; sc_stale = 1; } }
    if (sc && sc_free && win_size) { int w = 0, h = 0; win_size(&w, &h); if (w > 0 && h > 0 && ((uint32_t)w != scext.width || (uint32_t)h != scext.height)) sc_stale = 1; }   /* resized (Wayland never says so) */
    if (sc_stale || !sc) { if (!mk_swapchain()) return 0; }
    uint32_t ii;
    VkResult r = vkAcquireNextImageKHR(dev, sc, 100000000ull, uacq[f], VK_NULL_HANDLE, &ii);
    const double p2 = host_now();
    if (r == VK_ERROR_OUT_OF_DATE_KHR) { sc_stale = 1; return 0; }
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) return 0;
    if (r == VK_SUBOPTIMAL_KHR) sc_stale = 1;
    VkCommandBuffer cb = ucb[f];
    VkCommandBufferBeginInfo cbi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    vkResetCommandBuffer(cb, 0); vkBeginCommandBuffer(cb, &cbi);
    if (use_cal) { vkCmdResetQueryPool(cb, uqp, 2 * (uint32_t)f, 2); vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, uqp, 2 * (uint32_t)f); }
    const VkImage dst = scimg[ii];
    imgbar(cb, dst, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    const VkClearColorValue black = { { 0, 0, 0, 1 } }; const VkImageSubresourceRange all = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vkCmdClearColorImage(cb, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &all);
    imgbar(cb, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    /* the picture in a 640:448 rectangle (widescreen: the display's shape, settings_display_aspect) as large as the
       window allows, centred */
    settings_ui_window_size((int)scext.width, (int)scext.height);
    const double aw = 448.0 * settings_display_aspect();
    const double sx = scext.width / aw, sy = scext.height / 448.0, s = sx < sy ? sx : sy;
    const int rw = (int)(aw * s + 0.5), rh = (int)(448 * s + 0.5), x0 = ((int)scext.width - rw) / 2, y0 = ((int)scext.height - rh) / 2;
    if (ucur >= 0) {
        VkImageBlit b = { .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .srcOffsets = { { 0, 0, 0 }, { pic[ucur].w, pic[ucur].h, 1 } },
                          .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .dstOffsets = { { x0, y0, 0 }, { x0 + rw, y0 + rh, 1 } } };
        vkCmdBlitImage(cb, pic[ucur].img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b, VK_FILTER_LINEAR);
    }
    if (ov) {   /* the F3 overlay: 4 picture pixels from the corner, scaled with the picture */
        ring_put((uint8_t *)ovbuf.p + (size_t)f * OVW * OVH * 4, ov, (size_t)ovw * ovh * 4);
        imgbar(cb, ovimg, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy r2 = { .bufferOffset = (VkDeviceSize)f * OVW * OVH * 4, .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { (uint32_t)ovw, (uint32_t)ovh, 1 } };
        vkCmdCopyBufferToImage(cb, ovbuf.b, ovimg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r2);
        imgbar(cb, ovimg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        const double k2 = rh / 448.0; const int ox = x0 + (int)(4 * k2), oy = y0 + (int)(4 * k2);
        VkImageBlit b = { .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .srcOffsets = { { 0, 0, 0 }, { ovw, ovh, 1 } },
                          .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .dstOffsets = { { ox, oy, 0 }, { ox + (int)(ovw * k2), oy + (int)(ovh * k2), 1 } } };
        vkCmdBlitImage(cb, ovimg, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b, VK_FILTER_NEAREST);
    }
    imgbar(cb, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_ACCESS_TRANSFER_WRITE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    if (use_cal) vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, uqp, 2 * (uint32_t)f + 1);
    vkEndCommandBuffer(cb);
    const VkSemaphore ws[2] = { uacq[f], tl }; const uint64_t wv[2] = { 0, ucur_id }; const uint64_t sv = 0;
    const VkPipelineStageFlags wst[2] = { VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT };
    VkTimelineSemaphoreSubmitInfo tsi = { .sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO, .waitSemaphoreValueCount = ucur >= 0 ? 2 : 1, .pWaitSemaphoreValues = wv,
                                          .signalSemaphoreValueCount = 1, .pSignalSemaphoreValues = &sv };
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .pNext = &tsi, .waitSemaphoreCount = ucur >= 0 ? 2 : 1, .pWaitSemaphores = ws, .pWaitDstStageMask = wst,
                        .commandBufferCount = 1, .pCommandBuffers = &cb, .signalSemaphoreCount = 1, .pSignalSemaphores = &scdone[ii] };
    const uint64_t pid = pres_id + 1;
    VkPresentTimingInfoEXT pti = { .sType = VK_STRUCTURE_TYPE_PRESENT_TIMING_INFO_EXT, .timeDomainId = pt_dom, .presentStageQueries = VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_OUT_BIT_EXT };
    if (rel_ns && sc_ptim && sc_prel) {   /* shown rel_ns after the previous present reached the screen, on the nearest refresh */
        pti.flags = VK_PRESENT_TIMING_INFO_PRESENT_AT_RELATIVE_TIME_BIT_EXT | VK_PRESENT_TIMING_INFO_PRESENT_AT_NEAREST_REFRESH_CYCLE_BIT_EXT;
        pti.targetTime = rel_ns; pti.targetTimeDomainPresentStage = VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_OUT_BIT_EXT;
    }
    VkPresentTimingsInfoEXT ptis = { .sType = VK_STRUCTURE_TYPE_PRESENT_TIMINGS_INFO_EXT, .swapchainCount = 1, .pTimingInfos = &pti };
    VkPresentIdKHR pidi = { .sType = VK_STRUCTURE_TYPE_PRESENT_ID_KHR, .pNext = sc_ptim ? &ptis : 0, .swapchainCount = 1, .pPresentIds = &pid };
    VkPresentInfoKHR pi = { .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .pNext = can_wait ? &pidi : 0, .waitSemaphoreCount = 1, .pWaitSemaphores = &scdone[ii], .swapchainCount = 1, .pSwapchains = &sc, .pImageIndices = &ii };
    vkResetFences(dev, 1, &ufence[f]);
    if (shared_q) pthread_mutex_lock(&qmx);
    const double p3 = host_now();
    r = vkQueueSubmit(uqueue, 1, &si, ufence[f]);
    const double p4 = host_now();
    const VkResult pr = r ? r : vkQueuePresentKHR(uqueue, &pi);
    const double p5 = host_now();
    gsvk_present_ms[0] = (p1 - p0) * 1e3; gsvk_present_ms[1] = (p2 - p1) * 1e3; gsvk_present_ms[2] = (p3 - p2) * 1e3; gsvk_present_ms[3] = (p4 - p3) * 1e3; gsvk_present_ms[4] = (p5 - p4) * 1e3;
    if (shared_q) pthread_mutex_unlock(&qmx);
    if (r) { L("GSVK: window submit failed (%d)\n", (int)r); atomic_store(&pres_ok, 0); return 0; }
    if (pr == VK_ERROR_OUT_OF_DATE_KHR || pr == VK_SUBOPTIMAL_KHR || (int)pr == -1000255000) sc_stale = 1;   /* (or full-screen exclusive lost) */
    usub[f] = 1; uslot[f] = ucur; if (ucur >= 0) uref[ucur]++;
    ufr = (f + 1) % NFR;
    if (pr != VK_SUCCESS && pr != VK_SUBOPTIMAL_KHR) return 0;
    pres_id = pid; upid[f] = pid; ugt[pid & 63].pid = 0; ugt[pid & 63].sub = p3;
    if (sc_ptim) {   /* the scanout times reported since the last present */
        VkPresentStageTimeEXT st[16]; VkPastPresentationTimingEXT pt[16];
        for (int i = 0; i < 16; i++) { st[i] = (VkPresentStageTimeEXT){ 0 }; pt[i] = (VkPastPresentationTimingEXT){ .sType = VK_STRUCTURE_TYPE_PAST_PRESENTATION_TIMING_EXT, .presentStageCount = 1, .pPresentStages = &st[i] }; }
        VkPastPresentationTimingInfoEXT pii = { .sType = VK_STRUCTURE_TYPE_PAST_PRESENTATION_TIMING_INFO_EXT, .swapchain = sc };
        VkPastPresentationTimingPropertiesEXT pp = { .sType = VK_STRUCTURE_TYPE_PAST_PRESENTATION_TIMING_PROPERTIES_EXT, .presentationTimingCount = 16, .pPresentationTimings = pt };
        const VkResult q = pt_past(dev, &pii, &pp);
        if (q == VK_SUCCESS || q == VK_INCOMPLETE)
            for (uint32_t i = 0; i < pp.presentationTimingCount; i++) if (pt[i].reportComplete && st[i].stage && st[i].time) {   /* (a 0 time: no scanout reported) */
                if (pt_n < 256) { pt_ring[pt_n].pid = pt[i].presentId; pt_ring[pt_n].t = pt_qpc ? host_qpc_seconds(pt[i].pPresentStages[0].time) : pt[i].pPresentStages[0].time * 1e-9; pt_n++; }
            }
    }
    return pid;
}
/* UI thread: the scanout times collected (VK_EXT_present_timing): up to max {present id, host_now() seconds}; the count */
/* UI thread: 1 if this swapchain shows presents at a time relative to the previous one (gsvk_present's rel_ns) */
/* HG_PACEDBG: present pid's blit on the GPU, host_now() seconds: submitted, began, ended; 0 if not known (yet) */
int gsvk_present_gpu(uint64_t pid, double *sub, double *beg, double *end) {
    const int j = (int)(pid & 63); if (!use_cal || ugt[j].pid != pid) return 0;
    *sub = ugt[j].sub; *beg = ugt[j].beg; *end = ugt[j].end; return 1;
}
int gsvk_present_timed(void) { return sc && sc_ptim && sc_prel; }
/* UI thread: the display's refresh period the swapchain reports, in seconds (0: not known) */
double gsvk_present_refresh(void) {
    if (!sc || !sc_ptim || !pt_props) return 0;
    VkSwapchainTimingPropertiesEXT tp = { .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_TIMING_PROPERTIES_EXT }; uint64_t cnt = 0;
    const VkResult r = pt_props(dev, sc, &tp, &cnt);
    return r == VK_SUCCESS && tp.refreshDuration > 0 ? tp.refreshDuration * 1e-9 : 0;
}
int gsvk_present_scanout(uint64_t *pid, double *t, int max) {
    int n = pt_n < max ? pt_n : max;
    for (int i = 0; i < n; i++) { pid[i] = pt_ring[i].pid; t[i] = pt_ring[i].t; }
    memmove(pt_ring, pt_ring + n, (size_t)(pt_n - n) * sizeof pt_ring[0]); pt_n -= n;
    return n;
}
/* UI thread: wait until present `pid` (from gsvk_present) has reached the screen: 1 done, 0 not within timeout_ns,
   -1 not possible (no VK_KHR_present_wait, or the swapchain is gone) */
int gsvk_present_wait(uint64_t pid, uint64_t timeout_ns) {
    if (!can_wait || !sc || !pid || !atomic_load(&pres_ok)) return -1;
    const VkResult r = wait_present(dev, sc, pid, timeout_ns);
    if (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR) return 1;
    if (r == VK_TIMEOUT) return 0;
    if (r == VK_ERROR_OUT_OF_DATE_KHR) sc_stale = 1;
    return -1;
}
int gsvk_present_can_wait(void) { return can_wait; }
int gsvk_present_gen(void) { return atomic_load(&sc_gen); }   /* any thread: swapchains made so far (a new one: the display may have changed) */
int gsvk_present_fifo(void) { return sc && pmode == VK_PRESENT_MODE_FIFO_KHR; }   /* UI thread: the swapchain waits for the display */
/* UI thread, before the process ends: nothing more is presented, the GPU is done with the window */
void gsvk_present_shutdown(void) {
    if (!atomic_load(&pres_ok)) return;
    atomic_store(&pres_ok, 0);
    for (int f = 0; f < NFR; f++) if (usub[f]) vkWaitForFences(dev, 1, &ufence[f], VK_TRUE, 1000000000ull);
    if (shared_q) pthread_mutex_lock(&qmx);
    vkQueueWaitIdle(uqueue);
    if (shared_q) pthread_mutex_unlock(&qmx);
    if (sc) vkDestroySwapchainKHR(dev, sc, 0);
    vkDestroySurfaceKHR(inst, surf, 0); sc = 0; surf = 0;
}
